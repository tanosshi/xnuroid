#include "msl_glsl.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace msl {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------------------------------------------
struct Tok {
    enum Kind { Id, Num, Punct, Space } k;
    std::string s;
};
using Toks = std::vector<Tok>;

Toks lex(const std::string& src) {
    Toks out;
    size_t i = 0, n = src.size();
    static const char* const multi[] = {"::", "<<=", ">>=", "<=", ">=", "==", "!=", "&&", "||", "<<", ">>", "++", "--", "+=",
                                        "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->", nullptr};
    while (i < n) {
        char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n && std::isspace(static_cast<unsigned char>(src[j]))) ++j;
            out.push_back({Tok::Space, src.substr(i, j - i)});
            i = j;
        } else if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            size_t j = src.find('\n', i);
            if (j == std::string::npos) j = n;
            out.push_back({Tok::Space, " "});
            i = j;
        } else if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t j = src.find("*/", i + 2);
            j = j == std::string::npos ? n : j + 2;
            out.push_back({Tok::Space, " "});
            i = j;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) ++j;
            out.push_back({Tok::Id, src.substr(i, j - i)});
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '.' ||
                             ((src[j] == '+' || src[j] == '-') && (src[j - 1] == 'e' || src[j - 1] == 'E') &&
                              !(src[i] == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')))))
                ++j;
            out.push_back({Tok::Num, src.substr(i, j - i)});
            i = j;
        } else {
            size_t len = 1;
            for (const char* const* m = multi; *m; ++m) {
                size_t ml = std::strlen(*m);
                if (src.compare(i, ml, *m) == 0) { len = ml; break; }
            }
            out.push_back({Tok::Punct, src.substr(i, len)});
            i += len;
        }
    }
    return out;
}

std::string join(const Toks& t, size_t b, size_t e) {
    std::string s;
    for (size_t i = b; i < e && i < t.size(); ++i) s += t[i].s;
    return s;
}

// Index of the next non-space token at or after i (t.size() if none).
size_t next_sig(const Toks& t, size_t i) {
    while (i < t.size() && t[i].k == Tok::Space) ++i;
    return i;
}
size_t prev_sig(const Toks& t, size_t i) {   // last non-space index before i, or SIZE_MAX
    while (i > 0) {
        --i;
        if (t[i].k != Tok::Space) return i;
    }
    return static_cast<size_t>(-1);
}
bool is(const Toks& t, size_t i, const char* s) { return i < t.size() && t[i].s == s; }

// Matching close for the bracket at t[open] ('(' '[' '{' '<'), or t.size().
size_t match(const Toks& t, size_t open) {
    const std::string o = t[open].s;
    const char* c = o == "(" ? ")" : o == "[" ? "]" : o == "{" ? "}" : ">";
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        if (t[i].s == o) ++depth;
        else if (t[i].s == c && --depth == 0) return i;
        else if (o == "<" && t[i].s == ">>") { depth -= 2; if (depth <= 0) return i; }
    }
    return t.size();
}

// Splits [b, e) at top-level commas; with templates, <...> also nests (declarations only, never expressions).
std::vector<std::pair<size_t, size_t>> split_args(const Toks& t, size_t b, size_t e, bool templates = false) {
    std::vector<std::pair<size_t, size_t>> out;
    int depth = 0;
    size_t start = b;
    for (size_t i = b; i < e; ++i) {
        const std::string& s = t[i].s;
        if (s == "(" || s == "[" || s == "{" || (templates && s == "<")) ++depth;
        else if (s == ")" || s == "]" || s == "}" || (templates && s == ">")) --depth;
        else if (s == "," && depth == 0) { out.push_back({start, i}); start = i + 1; }
    }
    if (next_sig(t, start) < e || !out.empty()) out.push_back({start, e});
    return out;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// ---------------------------------------------------------------------------------------------------------------
// Preprocessor: conditionals and object-like macros are resolved here; function-like macros are passed to GLSL.
// ---------------------------------------------------------------------------------------------------------------
struct Pre {
    std::map<std::string, std::string> defines;
    std::vector<std::string> function_macros;   // "#define name(args) body" lines (Metal syntax, translated later)

    long long eval(const std::string& expr) {
        Toks t;
        for (const Tok& k : lex(expr)) if (k.k != Tok::Space) t.push_back(k);
        // defined(X) / defined X
        Toks r;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i].s == "defined") {
                size_t j = i + 1;
                bool paren = j < t.size() && t[j].s == "(";
                if (paren) ++j;
                std::string name = j < t.size() ? t[j].s : "";
                r.push_back({Tok::Num, defines.count(name) || is_function_macro(name) ? "1" : "0"});
                i = paren ? j + 1 : j;
            } else {
                r.push_back(t[i]);
            }
        }
        // expand macros (a few rounds); unknown identifiers are 0
        for (int round = 0; round < 8; ++round) {
            Toks x;
            bool changed = false;
            for (const Tok& k : r) {
                auto it = k.k == Tok::Id ? defines.find(k.s) : defines.end();
                if (it != defines.end()) {
                    for (const Tok& b : lex(it->second)) if (b.k != Tok::Space) x.push_back(b);
                    changed = true;
                } else {
                    x.push_back(k);
                }
            }
            r.swap(x);
            if (!changed) break;
        }
        pos_ = 0;
        toks_ = &r;
        return expr_or();
    }
    bool is_function_macro(const std::string& name) const {
        for (const std::string& f : function_macros) {
            size_t a = f.find("define");
            std::string rest = trim(f.substr(a + 6));
            if (rest.compare(0, name.size(), name) == 0 && rest.size() > name.size() && rest[name.size()] == '(') return true;
        }
        return false;
    }

    std::string run(const std::string& src) {
        std::istringstream in(src);
        std::string line, out;
        std::vector<std::pair<bool, bool>> stack;   // (this branch active, some branch already taken)
        auto active = [&] {
            for (auto& s : stack) if (!s.first) return false;
            return true;
        };
        std::string pending;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!pending.empty()) { line = pending + line; pending.clear(); }
            if (!line.empty() && line.back() == '\\') { pending = line.substr(0, line.size() - 1); continue; }
            std::string t = trim(line);
            if (t.empty() || t[0] != '#') {
                if (active()) out += line + "\n";
                continue;
            }
            std::string d = trim(t.substr(1));
            size_t sp = d.find_first_of(" \t(");
            std::string kw = d.substr(0, sp), rest = sp == std::string::npos ? "" : trim(d.substr(sp));
            if (kw == "if" || kw == "ifdef" || kw == "ifndef") {
                bool parent = active();
                bool v = kw == "if" ? eval(rest) != 0 : kw == "ifdef" ? defines.count(rest) != 0 : defines.count(rest) == 0;
                stack.push_back({parent && v, parent && v});
                if (!parent) stack.back() = {false, true};
            } else if (kw == "elif") {
                if (stack.empty()) continue;
                auto& top = stack.back();
                stack.pop_back();
                bool parent = active();
                bool v = !top.second && parent && eval(rest) != 0;
                stack.push_back({v, top.second || v});
            } else if (kw == "else") {
                if (stack.empty()) continue;
                auto top = stack.back();
                stack.pop_back();
                bool parent = active();
                stack.push_back({parent && !top.second, true});
            } else if (kw == "endif") {
                if (!stack.empty()) stack.pop_back();
            } else if (!active()) {
                continue;
            } else if (kw == "define") {
                size_t ne = 0;
                while (ne < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[ne])) || rest[ne] == '_')) ++ne;
                std::string name = rest.substr(0, ne);
                if (ne < rest.size() && rest[ne] == '(') function_macros.push_back("#define " + rest);
                else defines[name] = trim(rest.substr(ne));
            } else if (kw == "undef") {
                defines.erase(rest);
            }
            // #include, #pragma, #error ... are dropped
        }
        return out;
    }

private:
    const Toks* toks_ = nullptr;
    size_t pos_ = 0;
    const std::string& peek() {
        static const std::string empty;
        return pos_ < toks_->size() ? (*toks_)[pos_].s : empty;
    }
    long long primary() {
        std::string s = peek();
        ++pos_;
        if (s == "(") { long long v = expr_or(); if (peek() == ")") ++pos_; return v; }
        if (s == "!") return !primary();
        if (s == "-") return -primary();
        if (s == "~") return ~primary();
        if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) return strtoll(s.c_str(), nullptr, 0);
        return 0;
    }
    long long bin(int level) {
        static const std::vector<std::vector<std::string>> ops = {
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", ">", "<=", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}};
        if (level == static_cast<int>(ops.size())) return primary();
        long long v = bin(level + 1);
        for (;;) {
            std::string op = peek();
            bool found = false;
            for (const std::string& o : ops[level]) found |= o == op;
            if (!found) return v;
            ++pos_;
            long long r = bin(level + 1);
            if (op == "||") v = v || r; else if (op == "&&") v = v && r; else if (op == "|") v |= r; else if (op == "^") v ^= r;
            else if (op == "&") v &= r; else if (op == "==") v = v == r; else if (op == "!=") v = v != r; else if (op == "<") v = v < r;
            else if (op == ">") v = v > r; else if (op == "<=") v = v <= r; else if (op == ">=") v = v >= r; else if (op == "<<") v <<= r;
            else if (op == ">>") v >>= r; else if (op == "+") v += r; else if (op == "-") v -= r; else if (op == "*") v *= r;
            else if (op == "/") v = r ? v / r : 0; else v = r ? v % r : 0;
        }
    }
    long long expr_or() { return bin(0); }
};

// ---------------------------------------------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------------------------------------------
struct TypeInfo {
    char base = 'f';      // f float, h half, i int, u uint, b bool, s short, S ushort, c char, C uchar
    int comps = 1;        // vector width (column height for matrices)
    int cols = 0;         // matrix columns (0 = not a matrix)
    int size = 4, align = 4;
    bool packed = false;
    std::string glsl;
};

bool type_info(const std::string& t, TypeInfo& ti) {
    std::string s = t;
    ti = TypeInfo();
    if (s.compare(0, 7, "packed_") == 0) { ti.packed = true; s = s.substr(7); }
    static const std::pair<const char*, std::pair<char, int>> bases[] = {
        {"float", {'f', 4}}, {"half", {'h', 2}}, {"uint", {'u', 4}}, {"int", {'i', 4}}, {"bool", {'b', 1}},
        {"ushort", {'S', 2}}, {"short", {'s', 2}}, {"uchar", {'C', 1}}, {"char", {'c', 1}}};
    int scalar = 0;
    std::string rest;
    for (const auto& b : bases) {
        size_t l = std::strlen(b.first);
        if (s.compare(0, l, b.first) == 0 && (s.size() == l || std::isdigit(static_cast<unsigned char>(s[l])))) {
            ti.base = b.second.first;
            scalar = b.second.second;
            rest = s.substr(l);
            break;
        }
    }
    if (!scalar) return false;
    int a = 0, c = 0;
    if (rest.empty()) a = 1;
    else if (rest.size() == 1) a = rest[0] - '0';
    else if (rest.size() == 3 && rest[1] == 'x') { c = rest[0] - '0'; a = rest[2] - '0'; }
    else return false;
    if (a < 1 || a > 4 || c < 0 || c > 4 || (c && ti.base != 'f' && ti.base != 'h')) return false;
    ti.comps = a;
    ti.cols = c;
    int vsize = ti.packed ? a * scalar : (a == 3 ? 4 * scalar : a * scalar);
    int valign = ti.packed ? scalar : vsize;
    ti.size = c ? c * vsize : vsize;
    ti.align = valign;
    const bool fl = ti.base == 'f' || ti.base == 'h';
    if (c) {
        ti.glsl = c == a ? "mat" + std::to_string(c) : "mat" + std::to_string(c) + "x" + std::to_string(a);
    } else if (a == 1) {
        ti.glsl = fl ? "float" : ti.base == 'b' ? "bool" : (ti.base == 'u' || ti.base == 'S' || ti.base == 'C') ? "uint" : "int";
    } else {
        const char* p = fl ? "vec" : ti.base == 'b' ? "bvec" : (ti.base == 'u' || ti.base == 'S' || ti.base == 'C') ? "uvec" : "ivec";
        ti.glsl = p + std::to_string(a);
    }
    return true;
}

std::string glsl_type(const std::string& metal) {
    TypeInfo ti;
    return type_info(metal, ti) ? ti.glsl : metal;
}

struct Field {
    std::string type, name;
    int count = 0;           // array length, 0 = not an array
    int offset = 0, size = 0, align = 4, stride = 0;
    std::string attr;        // contents of [[ ... ]]
};
struct Struct {
    std::string name;
    std::vector<Field> fields;
    int size = 0, align = 4;
};

// ---------------------------------------------------------------------------------------------------------------
// Translator
// ---------------------------------------------------------------------------------------------------------------
struct Texture {
    std::string name;
    TexKind kind = TexKind::Tex2D;
    char elem = 'f';
    int index = 0;
};
struct Param {
    std::string type, name, attr;
};

class Translator {
public:
    Translator(const std::vector<std::pair<std::string, std::string>>& macros, bool clip_control) : clip_control_(clip_control) {
        for (const auto& m : macros) pre_.defines[m.first] = m.second;
        pre_.defines["__HAVE_FMA__"] = "1";
        pre_.defines["__METAL_VERSION__"] = "220";
    }

    Shader run(const std::string& source) {
        Shader sh;
        std::string text = pre_.run(source);
        toks_ = expand(lex(text));
        if (!parse_top(sh)) { sh.error = err_; return sh; }
        if (!emit(sh)) { sh.error = err_; return sh; }
        sh.ok = true;
        return sh;
    }

private:
    Pre pre_;
    bool clip_control_;
    Toks toks_;
    std::string err_;
    std::map<std::string, Struct> structs_;
    std::vector<int> remap_ = {0, 1, 2, 3, 4, 5, 6, 7};
    std::string globals_;              // translated constant arrays and helper functions from the source
    bool vertex_ = false;
    std::string ret_type_;
    std::vector<Param> params_;
    size_t body_b_ = 0, body_e_ = 0;   // token range of the entry body (inside braces)
    // per shader
    std::string prefix_;
    std::map<std::string, std::string> blocks_;     // param name -> struct type
    std::map<std::string, int> block_index_;
    std::map<std::string, Texture> textures_;
    std::map<std::string, int> samplers_;
    std::map<std::string, std::string> builtins_;   // param name -> GLSL expression
    std::string stage_in_struct_;
    std::map<std::string, SamplerUse> pairs_;       // uniform -> use
    std::map<std::string, std::string> accessors_;  // function name -> definition
    std::set<std::string> used_blocks_;
    std::map<std::string, std::pair<char, int>> locals_;   // local var -> (base, comps)
    std::set<std::string> helpers_;

    Toks expand(const Toks& in) {
        Toks out;
        for (const Tok& t : in) {
            auto it = t.k == Tok::Id ? pre_.defines.find(t.s) : pre_.defines.end();
            if (it != pre_.defines.end() && !it->second.empty()) {
                Toks e = lex(it->second);
                Toks again = expand(e);
                out.insert(out.end(), again.begin(), again.end());
            } else {
                out.push_back(t);
            }
        }
        return out;
    }

    bool fail(const std::string& m) { if (err_.empty()) err_ = m; return false; }

    // [[ ... ]] starting at i (t[i] == "[" and t[i+1] == "["): returns contents, sets end past "]]".
    std::string attribute(size_t i, size_t& end) {
        size_t j = i + 2;
        int depth = 0;
        for (; j + 1 < toks_.size(); ++j) {
            if (toks_[j].s == "[") ++depth;
            else if (toks_[j].s == "]") {
                if (depth == 0 && toks_[j + 1].s == "]") break;
                --depth;
            }
        }
        end = j + 2;
        return trim(join(toks_, i + 2, j));
    }
    bool at_attribute(size_t i) { return is(toks_, i, "[") && is(toks_, next_sig(toks_, i + 1), "["); }

    bool parse_struct(size_t& i) {
        size_t n = next_sig(toks_, i + 1);
        Struct st;
        st.name = toks_[n].s;
        size_t ob = next_sig(toks_, n + 1);
        if (!is(toks_, ob, "{")) return fail("struct without body");
        size_t cb = match(toks_, ob);
        size_t p = ob + 1;
        int cur = 0;
        while (true) {
            p = next_sig(toks_, p);
            if (p >= cb) break;
            // type [template] name [ [N] ] [[attr]] ;
            Field f;
            size_t semi = p;
            while (semi < cb && toks_[semi].s != ";") ++semi;
            std::vector<size_t> sig;
            for (size_t k = p; k < semi; ++k) {
                if (at_attribute(k)) {
                    size_t e;
                    std::string a = attribute(next_sig(toks_, k) == k ? k : k, e);
                    f.attr += (f.attr.empty() ? "" : ",") + a;
                    k = e - 1;
                    continue;
                }
                if (toks_[k].k != Tok::Space) sig.push_back(k);
            }
            if (sig.size() < 2) { p = semi + 1; continue; }
            size_t name_at = sig.size() - 1;
            if (toks_[sig.back()].s == "]") {   // array
                size_t ob2 = sig.size() - 1;
                while (ob2 > 0 && toks_[sig[ob2]].s != "[") --ob2;
                f.count = std::atoi(toks_[sig[ob2 + 1]].s.c_str());
                name_at = ob2 - 1;
            }
            f.name = toks_[sig[name_at]].s;
            for (size_t k = 0; k < name_at; ++k) f.type += toks_[sig[k]].s;
            TypeInfo ti;
            if (type_info(f.type, ti)) { f.size = ti.size; f.align = ti.align; }
            else if (structs_.count(f.type)) { f.size = structs_[f.type].size; f.align = structs_[f.type].align; }
            else return fail("unknown field type " + f.type + " in struct " + st.name);
            f.stride = (f.size + f.align - 1) / f.align * f.align;
            cur = (cur + f.align - 1) / f.align * f.align;
            f.offset = cur;
            cur += f.count ? f.stride * f.count : f.size;
            if (f.align > st.align) st.align = f.align;
            st.fields.push_back(f);
            p = semi + 1;
        }
        st.size = (cur + st.align - 1) / st.align * st.align;
        structs_[st.name] = st;
        i = cb + 1;
        if (is(toks_, next_sig(toks_, i), ";")) i = next_sig(toks_, i) + 1;
        return true;
    }

    bool parse_params(size_t b, size_t e) {
        for (auto [pb, pe] : split_args(toks_, b, e, true)) {
            Param p;
            std::vector<size_t> sig;
            for (size_t k = pb; k < pe; ++k) {
                if (at_attribute(k)) {
                    size_t end;
                    p.attr = attribute(k, end);
                    k = end - 1;
                    continue;
                }
                if (toks_[k].k != Tok::Space) sig.push_back(k);
            }
            if (sig.empty()) continue;
            p.name = toks_[sig.back()].s;
            for (size_t k = 0; k + 1 < sig.size(); ++k) p.type += (p.type.empty() || toks_[sig[k]].k != Tok::Id ? "" : " ") + toks_[sig[k]].s;
            params_.push_back(p);
        }
        return true;
    }

    bool parse_global_const(size_t& i) {
        // [constexpr] constant <type> name [ [N] ] = { ... } ;
        size_t semi = i;
        int depth = 0;
        for (; semi < toks_.size(); ++semi) {
            if (toks_[semi].s == "{") ++depth;
            else if (toks_[semi].s == "}") --depth;
            else if (toks_[semi].s == ";" && depth == 0) break;
        }
        std::vector<size_t> sig;
        for (size_t k = i; k < semi; ++k) if (toks_[k].k != Tok::Space) sig.push_back(k);
        size_t eq = 0;
        while (eq < sig.size() && toks_[sig[eq]].s != "=") ++eq;
        size_t s0 = 0;
        while (s0 < eq && (toks_[sig[s0]].s == "constexpr" || toks_[sig[s0]].s == "constant" || toks_[sig[s0]].s == "static" ||
                           toks_[sig[s0]].s == "const"))
            ++s0;
        if (eq >= sig.size() || s0 + 1 >= eq) return fail("unparsed global constant");
        std::string type = toks_[sig[s0]].s;
        std::string name = toks_[sig[s0 + 1]].s;
        bool array = eq > s0 + 2 && toks_[sig[s0 + 2]].s == "[";
        size_t vb = sig[eq] + 1;
        std::string init = trim(join(toks_, vb, semi));
        if (name == "xlt_remap_o") {
            remap_.clear();
            for (const Tok& t : lex(init)) if (t.k == Tok::Num) remap_.push_back(std::atoi(t.s.c_str()));
            i = semi + 1;
            return true;
        }
        std::string gt = glsl_type(type);
        if (array) {
            size_t ob = next_sig(toks_, vb);
            if (!is(toks_, ob, "{")) return fail("array constant without braces: " + name);
            size_t cb = match(toks_, ob);
            auto elems = split_args(toks_, ob + 1, cb);
            globals_ += "const " + gt + " " + name + "[" + std::to_string(elems.size()) + "] = " + gt + "[" + std::to_string(elems.size()) + "](";
            for (size_t k = 0; k < elems.size(); ++k) globals_ += (k ? ", " : "") + rewrite(elems[k].first, elems[k].second);
            globals_ += ");\n";
        } else {
            globals_ += "const " + gt + " " + name + " = " + rewrite(vb, semi) + ";\n";
        }
        i = semi + 1;
        return true;
    }

    bool parse_top(Shader& sh) {
        size_t i = 0;
        while (true) {
            i = next_sig(toks_, i);
            if (i >= toks_.size()) break;
            const std::string& s = toks_[i].s;
            if (s == "using") { while (i < toks_.size() && toks_[i].s != ";") ++i; ++i; continue; }
            if (s == "struct") { if (!parse_struct(i)) return false; continue; }
            if (s == "constexpr" || s == "constant") { if (!parse_global_const(i)) return false; continue; }
            if (s == "vertex" || s == "fragment") {
                vertex_ = s == "vertex";
                size_t rt = next_sig(toks_, i + 1);
                ret_type_ = toks_[rt].s;
                size_t nm = next_sig(toks_, rt + 1);
                size_t op = next_sig(toks_, nm + 1);
                if (!is(toks_, op, "(")) return fail("entry point without parameter list");
                size_t cp = match(toks_, op);
                parse_params(op + 1, cp);
                size_t ob = next_sig(toks_, cp + 1);
                if (!is(toks_, ob, "{")) return fail("entry point without body");
                body_b_ = ob + 1;
                body_e_ = match(toks_, ob);
                i = body_e_ + 1;
                continue;
            }
            if (s == "kernel") return fail("compute kernels are not supported");
            // helper function: <type> name ( params ) { body }  -> translated verbatim
            size_t semi = i, ob = toks_.size();
            for (size_t k = i; k < toks_.size(); ++k) {
                if (toks_[k].s == ";") { semi = k; break; }
                if (toks_[k].s == "{") { ob = k; break; }
            }
            if (ob < toks_.size()) {
                size_t cb = match(toks_, ob);
                globals_ += rewrite(i, cb + 1) + "\n";
                i = cb + 1;
            } else {
                i = semi + 1;
            }
        }
        sh.vertex = vertex_;
        if (!body_e_) return fail("no vertex/fragment entry point");
        return true;
    }

    // ---- references ----
    std::string block_word_fn(const std::string& block) { return prefix_ + block + "_w"; }

    // GLSL expression reading a leaf value of Metal type `type` at byte offset expression `off` of `block`.
    std::string read_leaf(const std::string& block, const TypeInfo& ti, const std::string& off) {
        auto at = [&](int byte) { return "(" + off + " + " + std::to_string(byte) + ")"; };
        auto scalar = [&](int byte) -> std::string {
            std::string o = at(byte), w = block_word_fn(block) + "(" + o + ")";
            switch (ti.base) {
                case 'f': return "uintBitsToFloat(" + w + ")";
                case 'h': return "unpackHalf2x16(" + w + " >> uint((" + o + " & 2) * 8)).x";
                case 'i': return "int(" + w + ")";
                case 'u': return w;
                case 'b': return "(((" + w + " >> uint((" + o + " & 3) * 8)) & 0xffu) != 0u)";
                case 's': return "(int(" + w + " << uint(16 - (" + o + " & 2) * 8)) >> 16)";
                case 'S': return "((" + w + " >> uint((" + o + " & 2) * 8)) & 0xffffu)";
                case 'c': return "(int(" + w + " << uint(24 - (" + o + " & 3) * 8)) >> 24)";
                default: return "((" + w + " >> uint((" + o + " & 3) * 8)) & 0xffu)";
            }
        };
        int ss = ti.base == 'h' || ti.base == 's' || ti.base == 'S' ? 2 : (ti.base == 'b' || ti.base == 'c' || ti.base == 'C') ? 1 : 4;
        auto vec = [&](int byte, int n) -> std::string {
            if (n == 1) return scalar(byte);
            std::string t = ti.base == 'f' || ti.base == 'h' ? "vec" : ti.base == 'b' ? "bvec" :
                            (ti.base == 'u' || ti.base == 'S' || ti.base == 'C') ? "uvec" : "ivec";
            std::string s = t + std::to_string(n) + "(";
            for (int k = 0; k < n; ++k) s += (k ? ", " : "") + scalar(byte + k * ss);
            return s + ")";
        };
        if (!ti.cols) return vec(0, ti.comps);
        int colsize = ti.packed ? ti.comps * ss : (ti.comps == 3 ? 4 * ss : ti.comps * ss);
        std::string s = ti.glsl + "(";
        for (int c = 0; c < ti.cols; ++c) s += (c ? ", " : "") + vec(c * colsize, ti.comps);
        return s + ")";
    }

    // Rewrites BLOCK.field[...].sub... starting at the block identifier i; returns the GLSL call, sets end.
    std::string block_ref(size_t i, size_t& end) {
        const std::string& block = toks_[i].s;
        const Struct* st = &structs_[blocks_[block]];
        used_blocks_.insert(block);
        std::string fname = prefix_ + block, off = "0";
        std::vector<std::string> args;
        size_t p = i + 1;
        TypeInfo leaf;
        bool have_leaf = false;
        while (true) {
            size_t d = next_sig(toks_, p);
            if (!is(toks_, d, ".") || have_leaf) break;
            size_t m = next_sig(toks_, d + 1);
            const Field* f = nullptr;
            for (const Field& x : st->fields) if (x.name == toks_[m].s) f = &x;
            if (!f) { fail("unknown member " + toks_[m].s + " of " + st->name); end = m + 1; return "0"; }
            fname += "_" + f->name;
            off = "(" + off + " + " + std::to_string(f->offset) + ")";
            p = m + 1;
            size_t b = next_sig(toks_, p);
            if (f->count && is(toks_, b, "[")) {
                size_t cb = match(toks_, b);
                args.push_back("int(" + rewrite(b + 1, cb) + ")");
                off = "(" + off + " + _i" + std::to_string(args.size() - 1) + " * " + std::to_string(f->stride) + ")";
                p = cb + 1;
            }
            if (structs_.count(f->type)) { st = &structs_[f->type]; continue; }
            if (!type_info(f->type, leaf)) { fail("bad leaf type " + f->type); end = p; return "0"; }
            have_leaf = true;
        }
        end = p;
        if (!have_leaf) { fail("reference to a whole struct in " + block); return "0"; }
        if (!accessors_.count(fname)) {
            std::string sig = leaf.glsl + " " + fname + "(";
            for (size_t k = 0; k < args.size(); ++k) sig += (k ? ", " : "") + std::string("int _i") + std::to_string(k);
            accessors_[fname] = sig + ") { return " + read_leaf(block, leaf, off) + "; }\n";
        }
        std::string call = fname + "(";
        for (size_t k = 0; k < args.size(); ++k) call += (k ? ", " : "") + args[k];
        return call + ")";
    }

    std::string sampler_uniform(const Texture& t, int sampler, bool compare) {
        std::string u = prefix_ + t.name + (sampler >= 0 ? "_s" + std::to_string(sampler) : std::string("_r")) + (compare ? "_cmp" : "");
        if (!pairs_.count(u)) {
            SamplerUse su;
            su.uniform = u;
            su.texture = t.index;
            su.sampler = sampler;
            su.kind = t.kind;
            su.compare = compare;
            pairs_[u] = su;
        }
        return u;
    }

    // tex.method(args)
    std::string texture_call(size_t i, size_t& end) {
        const Texture& t = textures_[toks_[i].s];
        size_t d = next_sig(toks_, i + 1);
        size_t m = next_sig(toks_, d + 1);
        std::string method = toks_[m].s;
        size_t op = next_sig(toks_, m + 1);
        size_t cp = match(toks_, op);
        end = cp + 1;
        auto args = split_args(toks_, op + 1, cp);
        std::vector<std::string> a;
        for (auto& r : args) a.push_back(trim(rewrite(r.first, r.second)));
        const bool array = t.kind == TexKind::Tex2DArray || t.kind == TexKind::Depth2DArray || t.kind == TexKind::TexCubeArray;
        const bool depth = t.kind == TexKind::Depth2D || t.kind == TexKind::Depth2DArray || t.kind == TexKind::DepthCube;
        const bool cube = t.kind == TexKind::TexCube || t.kind == TexKind::DepthCube || t.kind == TexKind::TexCubeArray;
        const int dims = t.kind == TexKind::Tex3D || cube ? 3 : 2;
        if (method == "sample" || method == "sample_compare" || method == "gather") {
            if (a.size() < 2) { fail("sample with too few arguments"); return "vec4(0.0)"; }
            const bool cmp = method == "sample_compare";
            int sidx = samplers_.count(trim(join(toks_, args[0].first, args[0].second))) ? samplers_[trim(join(toks_, args[0].first, args[0].second))] : 0;
            std::string u = sampler_uniform(t, sidx, cmp);
            std::string coord = a[1];
            size_t k = 2;
            if (array && k < a.size()) { coord = "vec" + std::to_string(dims + 1) + "(" + coord + ", float(" + a[k] + "))"; ++k; }
            std::string ref;
            if (cmp && k < a.size()) { ref = a[k]; ++k; }
            std::string lod, bias, grad, offset;
            for (; k < a.size(); ++k) {
                const std::string& o = a[k];
                auto inner = [&](size_t pre) { return o.substr(pre, o.size() - pre - 1); };
                if (o.compare(0, 6, "level(") == 0) lod = inner(6);
                else if (o.compare(0, 5, "bias(") == 0) bias = inner(5);
                else if (o.compare(0, 11, "gradient2d(") == 0 || o.compare(0, 11, "gradient3d(") == 0) grad = inner(11);
                else if (o.compare(0, 13, "gradientcube(") == 0) grad = inner(13);
                else if (o.compare(0, 14, "min_lod_clamp(") == 0) {}
                else offset = o;
            }
            if (cmp) {
                int n = dims + (array ? 1 : 0);
                if (n >= 4) coord = coord + ", float(" + ref + ")";   // samplerCubeArrayShadow / sampler2DArrayShadow (ref separate)
                else coord = "vec" + std::to_string(n + 1) + "(" + coord + ", float(" + ref + "))";
                if (!lod.empty() && n < 3) return "textureLod(" + u + ", " + coord + ", float(" + lod + "))";
                return "texture(" + u + ", " + coord + ")";
            }
            std::string sfx = depth ? ".x" : "";
            if (method == "gather") return "textureGather(" + u + ", " + coord + ")";
            if (!grad.empty()) return "textureGrad(" + u + ", " + coord + ", " + grad + ")" + sfx;
            if (!lod.empty()) return (offset.empty() ? "textureLod(" + u + ", " + coord + ", float(" + lod + "))"
                                                     : "textureLodOffset(" + u + ", " + coord + ", float(" + lod + "), " + offset + ")") + sfx;
            if (!offset.empty()) return "textureOffset(" + u + ", " + coord + ", " + offset + (bias.empty() ? "" : ", float(" + bias + ")") + ")" + sfx;
            return "texture(" + u + ", " + coord + (bias.empty() ? "" : ", float(" + bias + ")") + ")" + sfx;
        }
        std::string u = sampler_uniform(t, -1, false);
        if (method == "read") {
            if (a.empty()) { fail("read without coordinates"); return "vec4(0.0)"; }
            std::string coord = a[0], lod = "0";
            size_t k = 1;
            if (array && k < a.size()) { coord = "ivec3(ivec2(" + coord + "), int(" + a[k] + "))"; ++k; }
            else coord = "ivec" + std::to_string(dims) + "(" + coord + ")";
            if (k < a.size()) lod = a[k];
            return "texelFetch(" + u + ", " + coord + ", int(" + lod + "))" + (depth ? ".x" : "");
        }
        std::string lod = a.empty() ? "0" : a[0];
        if (method == "get_width") return "uint(textureSize(" + u + ", int(" + lod + ")).x)";
        if (method == "get_height") return "uint(textureSize(" + u + ", int(" + lod + ")).y)";
        if (method == "get_depth") return "uint(textureSize(" + u + ", int(" + lod + ")).z)";
        if (method == "get_array_size") return "uint(textureSize(" + u + ", 0).z)";
        if (method == "get_num_mip_levels") { helpers_.insert("levels"); return "uint(1)"; }
        fail("unsupported texture method " + method);
        return "vec4(0.0)";
    }

    std::string as_type_call(size_t i, size_t& end) {
        size_t lt = next_sig(toks_, i + 1);
        size_t gt = match(toks_, lt);
        std::string target = trim(join(toks_, lt + 1, gt));
        size_t op = next_sig(toks_, gt + 1);
        size_t cp = match(toks_, op);
        end = cp + 1;
        std::string arg = rewrite(op + 1, cp);
        TypeInfo ti;
        type_info(target, ti);
        char src = arg_base(op + 1, cp);
        if (ti.base == 'f') return (src == 'i' ? "intBitsToFloat(" : "uintBitsToFloat(") + arg + ")";
        if (src == 'f') return (ti.base == 'i' ? "floatBitsToInt(" : "floatBitsToUint(") + arg + ")";
        return ti.glsl + "(" + arg + ")";
    }

    // Best-effort base type of an expression: the first local/input identifier or constructor in it.
    char arg_base(size_t b, size_t e) {
        for (size_t k = b; k < e; ++k) {
            if (toks_[k].k != Tok::Id) continue;
            auto it = locals_.find(toks_[k].s);
            if (it != locals_.end()) return it->second.first == 'h' ? 'f' : it->second.first;
            TypeInfo ti;
            if (type_info(toks_[k].s, ti)) return ti.base == 'h' ? 'f' : ti.base;
            if (toks_[k].s == "as_type") break;
        }
        return 'f';
    }

    std::string vary_name(const std::string& attr, const std::string& field) {
        size_t u = attr.find("user(");
        if (u == std::string::npos) return "v_" + field;
        size_t e = attr.find(')', u);
        return "v_" + trim(attr.substr(u + 5, e - u - 5));
    }

    // Name of the GLSL entity for input.<f> / output.<f>.
    std::string io_name(bool out, const std::string& field) {
        const std::string& sname = out ? ret_type_ : stage_in_struct_;
        auto it = structs_.find(sname);
        if (it == structs_.end()) return field;
        for (const Field& f : it->second.fields) {
            if (f.name != field) continue;
            if (!out && vertex_) return "in_" + f.name;
            if (out && vertex_) {
                if (f.attr.find("position") != std::string::npos) return "gl_Position";
                if (f.attr.find("point_size") != std::string::npos) return "gl_PointSize";
                return vary_name(f.attr, f.name);
            }
            if (out) {
                if (f.attr.find("depth") != std::string::npos) return "gl_FragDepth";
                if (f.attr.find("sample_mask") != std::string::npos) return "gl_SampleMask[0]";
                return "o_" + f.name;
            }
            if (f.attr.find("position") != std::string::npos) return "gl_FragCoord";
            return vary_name(f.attr, f.name);
        }
        return field;
    }

    static std::string cmp_fn(const std::string& op) {
        return op == "<" ? "lessThan" : op == ">" ? "greaterThan" : op == "<=" ? "lessThanEqual" : op == ">=" ? "greaterThanEqual"
             : op == "==" ? "equal" : "notEqual";
    }

    // Top-level comparison operator in [b, e), or SIZE_MAX.
    size_t top_cmp(size_t b, size_t e) {
        int depth = 0;
        for (size_t k = b; k < e; ++k) {
            const std::string& s = toks_[k].s;
            if (s == "(" || s == "[") ++depth;
            else if (s == ")" || s == "]") --depth;
            else if (depth == 0 && (s == "<" || s == ">" || s == "<=" || s == ">=" || s == "==" || s == "!=")) return k;
        }
        return static_cast<size_t>(-1);
    }
    // [b, e) without one enclosing pair of parentheses, if it has one.
    void strip_parens(size_t& b, size_t& e) {
        size_t f = next_sig(toks_, b), l = prev_sig(toks_, e);
        if (is(toks_, f, "(") && match(toks_, f) == l) { b = f + 1; e = l; }
    }

    // Vector comparisons are component-wise in Metal (bool4 = a < b) but need lessThan() etc. in GLSL.
    std::string vector_compare(size_t b, size_t e, bool& done) {
        size_t c = top_cmp(b, e);
        done = c != static_cast<size_t>(-1);
        if (!done) return "";
        return cmp_fn(toks_[c].s) + "(" + rewrite(b, c) + ", " + rewrite(c + 1, e) + ")";
    }

public:
    // Translates tokens [b, e) to GLSL text.
    std::string rewrite(size_t b, size_t e) {
        std::string out;
        for (size_t i = b; i < e; ++i) {
            const Tok& t = toks_[i];
            if (t.k == Tok::Num) {
                std::string n = t.s;
                if (!n.empty() && (n.back() == 'h' || n.back() == 'H') && n.find_first_of("xX") == std::string::npos) n.pop_back();
                if (n.find_first_of(".eE") != std::string::npos && n.find_first_of("xX") == std::string::npos &&
                    !n.empty() && (n.back() == 'f' || n.back() == 'F'))
                    n.pop_back();
                out += n;
                continue;
            }
            if (t.k != Tok::Id) { out += t.s; continue; }
            size_t nx = next_sig(toks_, i + 1);
            const std::string& s = t.s;
            if ((s == "metal" || s == "precise" || s == "fast") && is(toks_, nx, "::")) { i = nx; continue; }
            if (s == "input" && is(toks_, nx, ".")) {
                size_t f = next_sig(toks_, nx + 1);
                out += io_name(false, toks_[f].s);
                i = f;
                continue;
            }
            if (s == "output" && is(toks_, nx, ".")) {
                size_t f = next_sig(toks_, nx + 1);
                out += io_name(true, toks_[f].s);
                i = f;
                continue;
            }
            if (blocks_.count(s) && is(toks_, nx, ".")) { size_t end; out += block_ref(i, end); i = end - 1; continue; }
            if (textures_.count(s) && is(toks_, nx, ".")) { size_t end; out += texture_call(i, end); i = end - 1; continue; }
            if (builtins_.count(s)) { out += builtins_[s]; continue; }
            if (s == "as_type" && is(toks_, nx, "<")) { size_t end; out += as_type_call(i, end); i = end - 1; continue; }
            if (s == "discard_fragment" && is(toks_, nx, "(")) { out += "discard"; i = match(toks_, nx); continue; }
            if (is(toks_, nx, "(")) {
                // boolN(a < b) -> lessThan(a, b)
                TypeInfo ti;
                if (type_info(s, ti) && ti.base == 'b' && ti.comps > 1 && !ti.cols) {
                    size_t cp = match(toks_, nx);
                    bool done = false;
                    std::string v = vector_compare(nx + 1, cp, done);
                    if (done) { out += v; i = cp; continue; }
                }
                static const std::map<std::string, std::string> fns = {
                    {"rsqrt", "inversesqrt"}, {"dfdx", "dFdx"}, {"dfdy", "dFdy"}, {"atan2", "atan"}, {"fmax", "max"}, {"fmin", "min"},
                    {"rint", "roundEven"}, {"select", "mix"}, {"powr", "pow"}, {"saturate", "_sat"}, {"fmod", "_fmod"},
                    {"log10", "_log10"}, {"fast_normalize", "normalize"}};
                auto it = fns.find(s);
                if (it != fns.end()) {
                    if (it->second[0] == '_') helpers_.insert(it->second);
                    out += it->second;
                    continue;
                }
            }
            out += glsl_type(s);
        }
        return out;
    }

private:
    // Collects local declarations (for bool-vector assignments and as_type) and rewrites the body.
    std::string body() {
        static const std::set<std::string> skip_starts = {"return", "if", "else", "for", "while", "do", "discard_fragment"};
        for (size_t k = body_b_; k < body_e_; ++k) {
            if (toks_[k].k != Tok::Id) continue;
            size_t p = prev_sig(toks_, k);
            if (p != static_cast<size_t>(-1) && !(toks_[p].s == ";" || toks_[p].s == "{" || toks_[p].s == "}" || toks_[p].s == ")")) continue;
            TypeInfo ti;
            if (!type_info(toks_[k].s, ti)) continue;
            size_t n = next_sig(toks_, k + 1);
            if (n < body_e_ && toks_[n].k == Tok::Id) locals_[toks_[n].s] = {ti.base, ti.cols ? 0 : ti.comps};
        }
        std::string out;
        size_t i = body_b_;
        while (i < body_e_) {
            size_t s = next_sig(toks_, i);
            if (s >= body_e_) { out += join(toks_, i, body_e_); break; }
            out += join(toks_, i, s);
            // `<RetStruct> output;` declaration -> dropped
            if (toks_[s].s == ret_type_ && is(toks_, next_sig(toks_, s + 1), "output")) {
                size_t semi = s;
                while (semi < body_e_ && toks_[semi].s != ";") ++semi;
                i = semi + 1;
                continue;
            }
            // return output;  -> fix up clip space, plain return
            if (toks_[s].s == "return") {
                size_t semi = s;
                while (semi < body_e_ && toks_[semi].s != ";") ++semi;
                out += vertex_ ? fixup() + "return;" : "return;";
                i = semi + 1;
                continue;
            }
            // statement: boolvec[.swz] = (a < b);
            if (toks_[s].k == Tok::Id && locals_.count(toks_[s].s) && locals_[toks_[s].s].first == 'b' && !skip_starts.count(toks_[s].s)) {
                size_t p = next_sig(toks_, s + 1);
                int width = locals_[toks_[s].s].second;
                if (is(toks_, p, ".")) {
                    size_t sw = next_sig(toks_, p + 1);
                    width = static_cast<int>(toks_[sw].s.size());
                    p = next_sig(toks_, sw + 1);
                }
                if (is(toks_, p, "=") && width > 1) {
                    size_t semi = p;
                    int depth = 0;
                    for (; semi < body_e_; ++semi) {
                        if (toks_[semi].s == "(") ++depth;
                        else if (toks_[semi].s == ")") --depth;
                        else if (toks_[semi].s == ";" && depth == 0) break;
                    }
                    size_t rb = p + 1, re = semi;
                    strip_parens(rb, re);
                    bool done = false;
                    std::string v = vector_compare(rb, re, done);
                    if (done) {
                        out += rewrite(s, p) + "= " + v + ";";
                        i = semi + 1;
                        continue;
                    }
                }
            }
            // ordinary token run up to the next statement boundary
            size_t e = s;
            int depth = 0;
            for (; e < body_e_; ++e) {
                const std::string& x = toks_[e].s;
                if (x == "(") ++depth;
                else if (x == ")") --depth;
                else if (depth == 0 && (x == ";" || x == "{" || x == "}")) { ++e; break; }
            }
            out += rewrite(s, e);
            i = e;
        }
        return out;
    }

    std::string fixup() {
        std::string s = "gl_Position.y = -gl_Position.y; ";
        if (!clip_control_) s += "gl_Position.z = gl_Position.z * 2.0 - gl_Position.w; ";
        return s;
    }

    bool emit(Shader& sh) {
        prefix_ = vertex_ ? "vs_" : "fs_";
        for (const Param& p : params_) {
            const std::string& a = p.attr;
            auto idx = [&](const char* key) {
                size_t k = a.find(key);
                return k == std::string::npos ? -1 : std::atoi(a.c_str() + a.find('(', k) + 1);
            };
            if (a.find("stage_in") != std::string::npos) { stage_in_struct_ = trim(p.type); continue; }
            if (a.find("buffer") != std::string::npos) {
                std::string t = p.type;
                if (t.find('&') == std::string::npos) return fail("buffer parameter that is not a constant reference: " + p.type + " " + p.name);
                std::string sname;
                for (const Tok& k : lex(t)) if (k.k == Tok::Id && k.s != "constant" && k.s != "const") sname = k.s;
                if (!structs_.count(sname)) return fail("constant buffer of non-struct type " + sname);
                blocks_[p.name] = sname;
                block_index_[p.name] = idx("buffer");
                continue;
            }
            if (a.find("sampler") != std::string::npos && trim(p.type) == "sampler") { samplers_[p.name] = idx("sampler"); continue; }
            if (a.find("texture") != std::string::npos) {
                Texture t;
                t.name = p.name;
                t.index = idx("texture");
                std::string ty = p.type;
                std::string head = ty.substr(0, ty.find('<'));
                head = trim(head);
                if (head == "texture2d") t.kind = TexKind::Tex2D;
                else if (head == "texture2d_array") t.kind = TexKind::Tex2DArray;
                else if (head == "texture3d") t.kind = TexKind::Tex3D;
                else if (head == "texturecube") t.kind = TexKind::TexCube;
                else if (head == "texturecube_array") t.kind = TexKind::TexCubeArray;
                else if (head == "depth2d") t.kind = TexKind::Depth2D;
                else if (head == "depth2d_array") t.kind = TexKind::Depth2DArray;
                else if (head == "depthcube") t.kind = TexKind::DepthCube;
                else return fail("unsupported texture type " + head);
                size_t lt = ty.find('<');
                std::string elem = lt == std::string::npos ? "float" : trim(ty.substr(lt + 1, ty.find_first_of(",>", lt) - lt - 1));
                TypeInfo ti;
                t.elem = type_info(elem, ti) ? (ti.base == 'h' ? 'f' : ti.base) : 'f';
                textures_[p.name] = t;
                continue;
            }
            if (a.find("vertex_id") != std::string::npos) { builtins_[p.name] = "uint(gl_VertexID)"; continue; }
            if (a.find("instance_id") != std::string::npos) { builtins_[p.name] = "uint(gl_InstanceID)"; continue; }
            if (a.find("front_facing") != std::string::npos) { builtins_[p.name] = "gl_FrontFacing"; continue; }
            if (a.find("position") != std::string::npos) { builtins_[p.name] = "gl_FragCoord"; continue; }
            if (a.find("sample_id") != std::string::npos) { builtins_[p.name] = "uint(gl_SampleID)"; continue; }
            if (a.find("sample_mask") != std::string::npos) { builtins_[p.name] = "uint(gl_SampleMaskIn[0])"; continue; }
            return fail("unsupported entry parameter [[" + a + "]] " + p.type + " " + p.name);
        }

        std::string main_body = body();
        if (!err_.empty()) return false;

        std::ostringstream g;
        g << "#version 320 es\n";
        g << "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler3D;\n"
             "precision highp samplerCube;\nprecision highp sampler2DArray;\nprecision highp sampler2DShadow;\n"
             "precision highp samplerCubeShadow;\nprecision highp sampler2DArrayShadow;\nprecision highp usampler2D;\n"
             "precision highp isampler2D;\n";
        // uniform blocks: raw bytes
        for (const std::string& b : used_blocks_) {
            const Struct& st = structs_[blocks_[b]];
            int vecs = (st.size + 15) / 16;
            if (vecs < 1) vecs = 1;
            std::string bn = prefix_ + b;
            g << "layout(std140) uniform " << bn << "_block { uvec4 " << bn << "_u[" << vecs << "]; };\n";
            g << "uint " << block_word_fn(b) << "(int o) { return " << bn << "_u[o >> 4][(o >> 2) & 3]; }\n";
            BlockUse bu;
            bu.block = bn + "_block";
            bu.buffer = block_index_[b];
            bu.size = vecs * 16;
            sh.blocks.push_back(bu);
        }
        for (auto& a : accessors_) g << a.second;
        // samplers
        for (auto& pr : pairs_) {
            const SamplerUse& su = pr.second;
            const Texture* tex = nullptr;
            for (auto& t : textures_) if (t.second.index == su.texture) tex = &t.second;
            char e = tex ? tex->elem : 'f';
            std::string pre = e == 'u' ? "u" : e == 'i' ? "i" : "";
            std::string ty;
            switch (su.kind) {
                case TexKind::Tex2D: ty = pre + "sampler2D"; break;
                case TexKind::Tex2DArray: ty = pre + "sampler2DArray"; break;
                case TexKind::Tex3D: ty = pre + "sampler3D"; break;
                case TexKind::TexCube: ty = pre + "samplerCube"; break;
                case TexKind::TexCubeArray: ty = pre + "samplerCubeArray"; break;
                case TexKind::Depth2D: ty = su.compare ? "sampler2DShadow" : "sampler2D"; break;
                case TexKind::Depth2DArray: ty = su.compare ? "sampler2DArrayShadow" : "sampler2DArray"; break;
                case TexKind::DepthCube: ty = su.compare ? "samplerCubeShadow" : "samplerCube"; break;
            }
            if (ty.find("CubeArray") != std::string::npos) g << "precision highp " << ty << ";\n";
            g << "uniform " << ty << " " << su.uniform << ";\n";
            sh.samplers.push_back(su);
        }
        // stage inputs / outputs
        auto decl_io = [&](const std::string& sname, bool out) -> bool {
            if (sname.empty()) return true;
            auto it = structs_.find(sname);
            if (it == structs_.end()) return fail("missing struct " + sname);
            for (const Field& f : it->second.fields) {
                TypeInfo ti;
                if (!type_info(f.type, ti)) return fail("unsupported io type " + f.type);
                std::string name = io_name(out, f.name);
                if (name.compare(0, 3, "gl_") == 0) continue;
                const bool integer = ti.base != 'f' && ti.base != 'h';
                if (!out && vertex_) {
                    size_t ap = f.attr.find("attribute(");
                    int loc = ap == std::string::npos ? 0 : std::atoi(f.attr.c_str() + ap + 10);
                    g << "layout(location = " << loc << ") in " << ti.glsl << " " << name << ";\n";
                    VertexInput vi;
                    vi.name = name;
                    vi.location = loc;
                    vi.components = ti.comps;
                    vi.base = ti.base == 'h' ? 'f' : (ti.base == 'u' || ti.base == 'S' || ti.base == 'C') ? 'u' : integer ? 'i' : 'f';
                    sh.inputs.push_back(vi);
                } else if (out && !vertex_) {
                    size_t cp = f.attr.find("color(");
                    if (cp == std::string::npos) return fail("fragment output without color(): " + f.attr);
                    std::string inner = f.attr.substr(cp + 6);
                    int loc = 0;
                    size_t lb = inner.find('[');
                    if (inner.compare(0, 11, "xlt_remap_o") == 0 && lb != std::string::npos) {
                        int k = std::atoi(inner.c_str() + lb + 1);
                        loc = k < static_cast<int>(remap_.size()) ? remap_[k] : k;
                    } else {
                        loc = std::atoi(inner.c_str());
                    }
                    g << "layout(location = " << loc << ") out " << ti.glsl << " " << name << ";\n";
                    sh.color_outputs.push_back(loc);
                } else {
                    bool flat = integer || f.attr.find("flat") != std::string::npos;
                    g << (flat ? "flat " : "") << (out ? "out " : "in ") << ti.glsl << " " << name << ";\n";
                }
            }
            return true;
        };
        if (!decl_io(stage_in_struct_, false)) return false;
        if (!decl_io(ret_type_, true)) return false;
        if (helpers_.count("_sat"))
            g << "float _sat(float x) { return clamp(x, 0.0, 1.0); }\nvec2 _sat(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
                 "vec3 _sat(vec3 x) { return clamp(x, 0.0, 1.0); }\nvec4 _sat(vec4 x) { return clamp(x, 0.0, 1.0); }\n";
        if (helpers_.count("_fmod"))
            g << "float _fmod(float a, float b) { return a - b * trunc(a / b); }\nvec2 _fmod(vec2 a, vec2 b) { return a - b * trunc(a / b); }\n"
                 "vec3 _fmod(vec3 a, vec3 b) { return a - b * trunc(a / b); }\nvec4 _fmod(vec4 a, vec4 b) { return a - b * trunc(a / b); }\n";
        if (helpers_.count("_log10"))
            g << "float _log10(float x) { return log2(x) * 0.30102999566; }\nvec2 _log10(vec2 x) { return log2(x) * 0.30102999566; }\n"
                 "vec3 _log10(vec3 x) { return log2(x) * 0.30102999566; }\nvec4 _log10(vec4 x) { return log2(x) * 0.30102999566; }\n";
        for (const std::string& m : pre_.function_macros) {
            Toks mt = lex(m);
            Toks saved;
            saved.swap(toks_);
            toks_ = mt;
            g << rewrite(0, toks_.size()) << "\n";
            toks_.swap(saved);
        }
        g << globals_;
        g << "void main()\n{" << main_body;
        if (vertex_ && main_body.find("return;") == std::string::npos) g << fixup();
        g << "}\n";
        sh.glsl = g.str();
        return true;
    }
};

}  // namespace

Shader translate(const std::string& source, const std::vector<std::pair<std::string, std::string>>& macros, bool clip_control) {
    Translator t(macros, clip_control);
    return t.run(source);
}

}  // namespace msl
