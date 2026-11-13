#pragma once
// Managed-state introspection: calls Unity's exported il2cpp API from the loader to ask the game what its managed
// world contains. Everything here uses *static* methods (or reads a boxed value's raw field) because invoking an
// instance method on a value type through il2cpp_runtime_invoke is layout-sensitive.
//
//   LOADER_MANAGED_QUERY=<frame>   report at that tick (default 300): Time.frameCount, Application.isPlaying,
//                                  SceneManager scene list with each scene's name and root-object count, then
//                                  Object.FindObjectsOfType counts for Object, Renderer, Camera, Canvas.
//
// Layouts used: Il2CppObject { Il2CppClass* klass; void* monitor; } then the boxed value at +16; Il2CppString
// { Il2CppObject obj; int32_t length; char16_t chars[]; } so length is at +16 and characters at +20.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "guest_symtab.hpp"

struct Il2CppApi {
    void* (*domain_get)();
    void** (*domain_get_assemblies)(void*, size_t*);
    void* (*assembly_get_image)(void*);
    void* (*class_from_name)(void*, const char*, const char*);
    void* (*class_get_method_from_name)(void*, const char*, int);
    void* (*runtime_invoke)(void*, void*, void**, void**);
    void* (*class_get_type)(void*);
    void* (*type_get_object)(void*);
    int32_t (*array_length)(void*);
    const char* (*class_get_name)(void*);
    const char* (*class_get_namespace)(void*);
    void* (*class_get_parent)(void*);
    void* (*string_new)(const char*);
    void* (*class_get_methods)(void*, void**);
    void* (*class_get_fields)(void*, void**);
    const char* (*field_get_name)(void*);
    size_t (*field_get_offset)(void*);
    void* (*field_get_type)(void*);
    void* (*object_get_class)(void*);
    const char* (*type_get_name)(void*);
    const char* (*method_get_name)(void*);
    uint32_t (*method_get_param_count)(void*);
    void* (*method_get_param)(void*, uint32_t);
    int (*type_get_type)(void*);
    void* (*thread_attach)(void*);
    bool ok = false;
};

template <typename T>
static void il2cpp_resolve(T& slot, uint64_t image_base, const char* name) {
    slot = reinterpret_cast<T>(guest_sym_addr(image_base, name));
}

static Il2CppApi il2cpp_api(uint64_t image_base) {
    Il2CppApi api{};
    il2cpp_resolve(api.domain_get, image_base, "il2cpp_domain_get");
    il2cpp_resolve(api.domain_get_assemblies, image_base, "il2cpp_domain_get_assemblies");
    il2cpp_resolve(api.assembly_get_image, image_base, "il2cpp_assembly_get_image");
    il2cpp_resolve(api.class_from_name, image_base, "il2cpp_class_from_name");
    il2cpp_resolve(api.class_get_method_from_name, image_base, "il2cpp_class_get_method_from_name");
    il2cpp_resolve(api.runtime_invoke, image_base, "il2cpp_runtime_invoke");
    il2cpp_resolve(api.class_get_type, image_base, "il2cpp_class_get_type");
    il2cpp_resolve(api.type_get_object, image_base, "il2cpp_type_get_object");
    il2cpp_resolve(api.array_length, image_base, "il2cpp_array_length");
    il2cpp_resolve(api.class_get_name, image_base, "il2cpp_class_get_name");
    il2cpp_resolve(api.class_get_namespace, image_base, "il2cpp_class_get_namespace");
    il2cpp_resolve(api.class_get_parent, image_base, "il2cpp_class_get_parent");
    il2cpp_resolve(api.string_new, image_base, "il2cpp_string_new");
    il2cpp_resolve(api.class_get_methods, image_base, "il2cpp_class_get_methods");
    il2cpp_resolve(api.class_get_fields, image_base, "il2cpp_class_get_fields");
    il2cpp_resolve(api.field_get_name, image_base, "il2cpp_field_get_name");
    il2cpp_resolve(api.field_get_offset, image_base, "il2cpp_field_get_offset");
    il2cpp_resolve(api.field_get_type, image_base, "il2cpp_field_get_type");
    il2cpp_resolve(api.object_get_class, image_base, "il2cpp_object_get_class");
    il2cpp_resolve(api.type_get_name, image_base, "il2cpp_type_get_name");
    il2cpp_resolve(api.method_get_name, image_base, "il2cpp_method_get_name");
    il2cpp_resolve(api.method_get_param_count, image_base, "il2cpp_method_get_param_count");
    il2cpp_resolve(api.method_get_param, image_base, "il2cpp_method_get_param");
    il2cpp_resolve(api.type_get_type, image_base, "il2cpp_type_get_type");
    il2cpp_resolve(api.thread_attach, image_base, "il2cpp_thread_attach");
    api.ok = api.domain_get && api.domain_get_assemblies && api.assembly_get_image && api.class_from_name &&
             api.class_get_method_from_name && api.runtime_invoke && api.class_get_type && api.type_get_object &&
             api.array_length;
    return api;
}

static void* il2cpp_class(Il2CppApi& api, const char* ns, const char* name) {
    size_t count = 0;
    void* domain = api.domain_get();
    if (!domain) return nullptr;
    void** assemblies = api.domain_get_assemblies(domain, &count);
    if (!assemblies) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        void* image = api.assembly_get_image(assemblies[i]);
        if (!image) continue;
        if (void* klass = api.class_from_name(image, ns, name)) return klass;
    }
    return nullptr;
}

// Invokes a static method taking no arguments and returns its boxed int32 value.
static bool il2cpp_static_int(Il2CppApi& api, void* klass, const char* method, int* out) {
    if (!klass) return false;
    void* mi = api.class_get_method_from_name(klass, method, 0);
    if (!mi) return false;
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, nullptr, nullptr, &exc);
    if (exc || !ret) return false;
    *out = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(ret) + 16);
    return true;
}

static bool il2cpp_static_int_arg(Il2CppApi& api, void* klass, const char* method, int arg, int* out) {
    if (!klass) return false;
    void* mi = api.class_get_method_from_name(klass, method, 1);
    if (!mi) return false;
    void* args[1] = {&arg};
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, nullptr, args, &exc);
    if (exc || !ret) return false;
    *out = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(ret) + 16);
    return true;
}

static bool il2cpp_static_float(Il2CppApi& api, void* klass, const char* method, float* out) {
    if (!klass) return false;
    void* mi = api.class_get_method_from_name(klass, method, 0);
    if (!mi) return false;
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, nullptr, nullptr, &exc);
    if (exc || !ret) return false;
    *out = *reinterpret_cast<float*>(reinterpret_cast<char*>(ret) + 16);
    return true;
}

// Instance getters on reference types (safe: no value-type unboxing involved).
static void* il2cpp_get_method(Il2CppApi& api, void* klass, const char* name) {
    return klass ? api.class_get_method_from_name(klass, name, 0) : nullptr;
}

static bool il2cpp_get_number(Il2CppApi& api, void* obj, void* klass, const char* name, double* out) {
    void* mi = il2cpp_get_method(api, klass, name);
    if (!mi || !obj) return false;
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, obj, nullptr, &exc);
    if (exc || !ret) return false;
    *out = *reinterpret_cast<double*>(reinterpret_cast<char*>(ret) + 16);
    return true;
}

static bool il2cpp_get_int(Il2CppApi& api, void* obj, void* klass, const char* name, long long* out) {
    void* mi = il2cpp_get_method(api, klass, name);
    if (!mi || !obj) return false;
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, obj, nullptr, &exc);
    if (exc || !ret) return false;
    *out = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(ret) + 16);
    return true;
}

// Static method taking an int and returning a string; prints through `out`.
static bool il2cpp_static_string(Il2CppApi& api, void* klass, const char* method, int arg, std::string& out) {
    if (!klass) return false;
    void* mi = api.class_get_method_from_name(klass, method, 1);
    if (!mi) return false;
    void* args[1] = {&arg};
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, nullptr, args, &exc);
    if (exc || !ret) return false;
    const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(ret) + 16);
    if (len < 0 || len > 4096) return false;
    const char16_t* chars = reinterpret_cast<const char16_t*>(reinterpret_cast<char*>(ret) + 20);
    out.assign(len, ' ');
    for (int32_t i = 0; i < len; ++i) out[i] = chars[i] < 0x80 ? static_cast<char>(chars[i]) : '?';
    return true;
}

// Static int returning a boxed result through one `Type` argument (Object.FindObjectsOfType).
static bool il2cpp_find_count(Il2CppApi& api, void* klass, const char* type_ns, const char* type_name, int* out) {
    void* target = il2cpp_class(api, type_ns, type_name);
    if (!target || !klass) return false;
    void* mi = api.class_get_method_from_name(klass, "FindObjectsOfType", 1);
    if (!mi) return false;
    void* type_obj = api.type_get_object(api.class_get_type(target));
    if (!type_obj) return false;
    void* args[1] = {type_obj};
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, nullptr, args, &exc);
    if (exc || !ret) return false;
    *out = api.array_length(ret);
    return true;
}

// Il2CppArray is { Il2CppObject obj; Il2CppArrayBounds* bounds; il2cpp_array_size_t max_length; void* vector[]; }
// so elements start at +32 and the length is readable at +24.
static void* il2cpp_array_at(void* array, int index) {
    if (!array) return nullptr;
    const int32_t max_length = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(array) + 24);
    if (index < 0 || index >= max_length) return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(array) + 32)[index];
}

// "Namespace.Type" of a live object, from the class pointer in its object header.
static std::string il2cpp_object_class_name(Il2CppApi& api, void* obj) {
    if (!obj) return "?";
    void* klass = *reinterpret_cast<void**>(obj);
    if (!klass) return "?";
    const char* name = api.class_get_name ? api.class_get_name(klass) : nullptr;
    const char* ns = api.class_get_namespace ? api.class_get_namespace(klass) : nullptr;
    std::string out = ns && *ns ? std::string(ns) + "." : "";
    out += name ? name : "?";
    return out;
}

// Field-by-field dump of one live object: names, offsets and values, with strings decoded. Reads each field at the
// offset the guest's own metadata reports, so it stays layout-correct across builds.
static void il2cpp_inspect_fields(Il2CppApi& api, void* obj, const char* label) {
    if (!obj || !api.class_get_fields || !api.field_get_name || !api.field_get_offset) return;
    void* klass = api.object_get_class(obj);
    std::fprintf(stderr, "[MANAGED]   %s fields:\n", label);
    void* iter = nullptr;
    while (void* field = api.class_get_fields(klass, &iter)) {
        const char* name = api.field_get_name(field);
        const size_t offset = api.field_get_offset(field);
        void* type = api.field_get_type(field);
        const int type_code = (type && api.type_get_type) ? api.type_get_type(type) : -1;
        const char* value = reinterpret_cast<const char*>(obj) + offset;
        if (type_code == 0x0e && api.type_get_name) {                 // IL2CPP_TYPE_STRING
            const uint64_t str = *reinterpret_cast<const uint64_t*>(value);
            if (str) {
                const int32_t len = *reinterpret_cast<const int32_t*>(str + 16);
                std::string text;
                const char16_t* chars = reinterpret_cast<const char16_t*>(str + 20);
                for (int32_t c = 0; c < len && c < 96; ++c) text += chars[c] < 0x80 ? static_cast<char>(chars[c]) : '?';
                std::fprintf(stderr, "[MANAGED]     %-28s \"%s\"\n", name, text.c_str());
            } else {
                std::fprintf(stderr, "[MANAGED]     %-28s (null)\n", name);
            }
        } else if (type_code == 0x08 || type_code == 0x09 || type_code == 0x0c) {   // I4, U4, R4
            std::fprintf(stderr, "[MANAGED]     %-28s %d / %g\n", name,
                         *reinterpret_cast<const int32_t*>(value), static_cast<double>(*reinterpret_cast<const float*>(value)));
        } else if (type_code == 0x02) {                               // BOOLEAN
            std::fprintf(stderr, "[MANAGED]     %-28s %d\n", name,
                         *reinterpret_cast<const uint8_t*>(value) ? 1 : 0);
        } else if (type_code == 0x0a || type_code == 0x0b) {          // I8, U8
            std::fprintf(stderr, "[MANAGED]     %-28s %lld\n", name,
                         static_cast<long long>(*reinterpret_cast<const int64_t*>(value)));
        } else {
            std::fprintf(stderr, "[MANAGED]     %-28s ref %p (type 0x%x)\n", name,
                         *reinterpret_cast<void* const*>(value), type_code);
        }
    }
}

// LOADER_TRACE_HIT="<Type>::<Method>[,<Type>::<Method>...]" overwrites the first instruction of each method with
// `udf #0`. The first instruction fetched from a patched method raises SIGILL, whose dump carries the faulting PC, so
// one run shows whether (and which) of a set of methods is ever reached. Diagnostic only: it stops the process.
static void il2cpp_trace_hits(uint64_t image_base, const char* binary_path, const char* spec) {
    if (!guest_symtab_load(binary_path)) return;
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok || !spec) return;
    const std::string list = spec;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t sep = item.find("::");
        if (sep != std::string::npos) {
            const std::string type_name = item.substr(0, sep);
            const std::string method = item.substr(sep + 2);
            const size_t dot = type_name.find_last_of('.');
            const std::string ns = dot == std::string::npos ? std::string() : type_name.substr(0, dot);
            const std::string name = dot == std::string::npos ? type_name : type_name.substr(dot + 1);
            void* klass = il2cpp_class(api, ns.c_str(), name.c_str());
            void* mi = klass ? api.class_get_method_from_name(klass, method.c_str(), 0) : nullptr;
            void* code = mi ? *reinterpret_cast<void**>(mi) : nullptr;
            if (!code && mi) code = *reinterpret_cast<void**>(reinterpret_cast<char*>(mi) + 8);
            if (!code) {
                std::fprintf(stderr, "[TRACE-HIT] %s not found (klass=%p mi=%p slots=%p,%p,%p)\n", item.c_str(), klass,
                             mi, mi ? *reinterpret_cast<void**>(mi) : nullptr,
                             mi ? *reinterpret_cast<void**>(reinterpret_cast<char*>(mi) + 8) : nullptr,
                             mi ? *reinterpret_cast<void**>(reinterpret_cast<char*>(mi) + 16) : nullptr);
            } else {
                const uintptr_t page = reinterpret_cast<uintptr_t>(code) & ~static_cast<uintptr_t>(4095);
                mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
                *reinterpret_cast<uint32_t*>(code) = 0x00000000u;   // udf #0
                __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code) + 4);
                std::fprintf(stderr, "[TRACE-HIT] patched %s at %p\n", item.c_str(), code);
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    std::fflush(stderr);
}

// Value of a parameterless static method returning a managed string, e.g. Application.get_persistentDataPath.
static bool il2cpp_static_string0(Il2CppApi& api, void* klass, const char* method, std::string& out) {
    void* mi = klass && api.class_get_method_from_name ? api.class_get_method_from_name(klass, method, 0) : nullptr;
    if (!mi) return false;
    void* exc = nullptr;
    void* ret = api.runtime_invoke(mi, nullptr, nullptr, &exc);
    if (exc || !ret) return false;
    const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(ret) + 16);
    const char16_t* chars = reinterpret_cast<const char16_t*>(reinterpret_cast<char*>(ret) + 20);
    for (int32_t i = 0; i < len && i < 512; ++i) out += chars[i] < 0x80 ? static_cast<char>(chars[i]) : '?';
    return true;
}

// LOADER_MANAGED_STATIC="<Namespace.Type>::<StaticMethod>" prints one parameterless static result.
static void il2cpp_static_report(uint64_t image_base, const char* binary_path, const char* spec) {
    if (!guest_symtab_load(binary_path)) return;
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) return;
    const std::string full = spec ? spec : "";
    const size_t sep = full.find("::");
    if (sep == std::string::npos) return;
    const std::string type_name = full.substr(0, sep);
    const std::string method = full.substr(sep + 2);
    const size_t dot = type_name.find_last_of('.');
    const std::string ns = dot == std::string::npos ? std::string() : type_name.substr(0, dot);
    const std::string name = dot == std::string::npos ? type_name : type_name.substr(dot + 1);
    void* klass = il2cpp_class(api, ns.c_str(), name.c_str());
    std::string value;
    if (klass && il2cpp_static_string0(api, klass, method.c_str(), value))
        std::fprintf(stderr, "[MANAGED] %s::%s() = \"%s\"\n", type_name.c_str(), method.c_str(), value.c_str());
    else
        std::fprintf(stderr, "[MANAGED] %s::%s() unavailable\n", type_name.c_str(), method.c_str());
    std::fflush(stderr);
}

// LOADER_MANAGED_STATIC="<Namespace.Type>::<StaticMethod>" prints one parameterless static result.

// Active scene name through the guest's own SceneManager::GetSceneAt + Scene::GetNameInternal.
static std::string il2cpp_active_scene(Il2CppApi& api) {
    void* scene_manager = il2cpp_class(api, "UnityEngine.SceneManagement", "SceneManager");
    void* scene_class = il2cpp_class(api, "UnityEngine.SceneManagement", "Scene");
    if (!scene_manager || !scene_class) return std::string();
    int count = -1;
    if (!il2cpp_static_int(api, scene_manager, "get_sceneCount", &count) || count <= 0) return std::string();
    void* get_at = api.class_get_method_from_name(scene_manager, "GetSceneAt", 1);
    if (!get_at) return std::string();
    int index = 0;
    void* args[1] = {&index};
    void* exc = nullptr;
    void* boxed = api.runtime_invoke(get_at, nullptr, args, &exc);
    if (exc || !boxed) return std::string();
    const int handle = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(boxed) + 16);
    std::string name;
    il2cpp_static_string(api, scene_class, "GetNameInternal", handle, name);
    return name;
}

// 1 = a VideoPlayer exists and is neither prepared nor playing (the host has no video backend, so it never will be),
// 0 = nothing to do, -1 = introspection unavailable.
static int il2cpp_video_state(Il2CppApi& api) {
    void* video_class = il2cpp_class(api, "UnityEngine.Video", "VideoPlayer");
    void* object_class = il2cpp_class(api, "UnityEngine", "Object");
    if (!video_class || !object_class) return -1;
    void* mi = api.class_get_method_from_name(object_class, "FindObjectsOfType", 1);
    void* type_obj = api.type_get_object(api.class_get_type(video_class));
    if (!mi || !type_obj) return -1;
    void* args[1] = {type_obj};
    void* exc = nullptr;
    void* arr = api.runtime_invoke(mi, nullptr, args, &exc);
    if (exc || !arr) return -1;
    if (api.array_length(arr) <= 0) return 0;
    void* player = il2cpp_array_at(arr, 0);
    long long prepared = -1, playing = -1;
    il2cpp_get_int(api, player, video_class, "get_isPrepared", &prepared);
    il2cpp_get_int(api, player, video_class, "get_isPlaying", &playing);
    return (prepared == 1 || playing == 1) ? 0 : 1;
}

static void* il2cpp_find_load_scene(Il2CppApi& api, void* scene_manager, int first_param_type);

// Invoke a parameterless method on the first live instance of a managed type. Used for the splash video completion
// callback, which is what the player's own startup sequence waits on when the host cannot play the video.
static bool il2cpp_invoke_first(uint64_t image_base, const char* binary_path, const char* full_type, const char* method) {
    if (!guest_symtab_load(binary_path)) return false;
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) return false;
    const std::string qualified = full_type ? full_type : "";
    const size_t dot = qualified.find_last_of('.');
    const std::string ns = dot == std::string::npos ? std::string() : qualified.substr(0, dot);
    const std::string name = dot == std::string::npos ? qualified : qualified.substr(dot + 1);
    void* klass = il2cpp_class(api, ns.c_str(), name.c_str());
    void* object_class = il2cpp_class(api, "UnityEngine", "Object");
    if (!klass || !object_class) return false;
    void* find = api.class_get_method_from_name(object_class, "FindObjectsOfType", 1);
    void* type_obj = api.type_get_object(api.class_get_type(klass));
    void* exc = nullptr;
    void* arr = (find && type_obj) ? api.runtime_invoke(find, nullptr, (void*[]){type_obj}, &exc) : nullptr;
    if (!arr || exc || api.array_length(arr) == 0) return false;
    void* target = il2cpp_array_at(arr, 0);
    void* mi = api.class_get_method_from_name(klass, method, 0);
    void* args[1] = {nullptr};
    void** argv = nullptr;
    if (!mi) {
        // Video callbacks arrive as `OnVideoCompleted(VideoPlayer source)`, so pass the scene's player when the
        // parameterless overload does not exist.
        mi = api.class_get_method_from_name(klass, method, 1);
        if (!mi) {
            std::fprintf(stderr, "[MANAGED] %s has no %s overload\n", qualified.c_str(), method);
            return false;
        }
        void* video_class = il2cpp_class(api, "UnityEngine.Video", "VideoPlayer");
        if (video_class) {
            void* video_type = api.type_get_object(api.class_get_type(video_class));
            void* exc2 = nullptr;
            void* videos = video_type ? api.runtime_invoke(find, nullptr, (void*[]){video_type}, &exc2) : nullptr;
            if (videos && !exc2 && api.array_length(videos) > 0) args[0] = il2cpp_array_at(videos, 0);
        }
        argv = args;
    }
    void* domain = api.domain_get();
    if (api.thread_attach && domain) api.thread_attach(domain);
    void* ret = api.runtime_invoke(mi, target, argv, &exc);
    std::fprintf(stderr, "[MANAGED] %s::%s() on %p -> %p exc=%p\n", qualified.c_str(), method, target, ret, exc);
    std::fflush(stderr);
    return !exc;
}

// Deliver the video-completed callback to whichever live component declares it. Splash scenes on iOS end their video
// through such a callback; with no video backend the clip never ends, so the host reports completion instead and the
// player's own startup flow continues unchanged.
static bool il2cpp_video_complete_callback(uint64_t image_base, const char* binary_path) {
    if (!guest_symtab_load(binary_path)) return false;
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) return false;
    if (il2cpp_video_state(api) != 1) return false;   // only when a video really is stuck unprepared
    void* mono = il2cpp_class(api, "UnityEngine", "MonoBehaviour");
    void* object_class = il2cpp_class(api, "UnityEngine", "Object");
    void* video_class = il2cpp_class(api, "UnityEngine.Video", "VideoPlayer");
    if (!mono || !object_class) return false;
    void* find = api.class_get_method_from_name(object_class, "FindObjectsOfType", 1);
    void* mono_type = api.type_get_object(api.class_get_type(mono));
    void* exc = nullptr;
    void* components = (find && mono_type) ? api.runtime_invoke(find, nullptr, (void*[]){mono_type}, &exc) : nullptr;
    if (!components || exc) return false;
    void* player = nullptr;
    if (video_class) {
        void* video_type = api.type_get_object(api.class_get_type(video_class));
        void* exc_v = nullptr;
        void* videos = video_type ? api.runtime_invoke(find, nullptr, (void*[]){video_type}, &exc_v) : nullptr;
        if (videos && !exc_v && api.array_length(videos) > 0) player = il2cpp_array_at(videos, 0);
    }
    const int count = api.array_length(components);
    for (int i = 0; i < count; ++i) {
        void* obj = il2cpp_array_at(components, i);
        void* klass = api.object_get_class(obj);
        if (!klass) continue;
        void* mi = api.class_get_method_from_name(klass, "OnVideoCompleted", 0);
        void* args[1] = {player};
        void** argv = nullptr;
        if (!mi) {
            mi = api.class_get_method_from_name(klass, "OnVideoCompleted", 1);
            if (!mi) continue;
            argv = args;
        }
        void* domain = api.domain_get();
        if (api.thread_attach && domain) api.thread_attach(domain);
        void* call_exc = nullptr;
        void* ret = api.runtime_invoke(mi, obj, argv, &call_exc);
        std::fprintf(stderr, "[MANAGED] %s::OnVideoCompleted(player=%p) -> %p exc=%p\n",
                     il2cpp_object_class_name(api, obj).c_str(), player, ret, call_exc);
        std::fflush(stderr);
        return !call_exc;
    }
    std::fprintf(stderr, "[MANAGED] no live OnVideoCompleted component (%d behaviours)\n", count);
    std::fflush(stderr);
    return false;
}

// LOADER_MANAGED_INSPECT="<Namespace.Type>" support: dump every live instance of one managed type.
static void il2cpp_inspect_type(uint64_t image_base, const char* binary_path, const char* full_name) {
    if (!guest_symtab_load(binary_path)) return;
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) {
        std::fprintf(stderr, "[MANAGED] il2cpp API not fully exported\n");
        return;
    }
    const std::string qualified = full_name ? full_name : "";
    const size_t dot = qualified.find_last_of('.');
    const std::string ns = dot == std::string::npos ? std::string() : qualified.substr(0, dot);
    const std::string name = dot == std::string::npos ? qualified : qualified.substr(dot + 1);
    void* klass = il2cpp_class(api, ns.c_str(), name.c_str());
    void* object_class = il2cpp_class(api, "UnityEngine", "Object");
    if (!klass || !object_class) {
        std::fprintf(stderr, "[MANAGED] type %s not found\n", qualified.c_str());
        return;
    }
    void* mi = api.class_get_method_from_name(object_class, "FindObjectsOfType", 1);
    void* type_obj = api.type_get_object(api.class_get_type(klass));
    void* exc = nullptr;
    void* arr = (mi && type_obj) ? api.runtime_invoke(mi, nullptr, (void*[]){type_obj}, &exc) : nullptr;
    const int count = (arr && !exc) ? api.array_length(arr) : 0;
    std::fprintf(stderr, "[MANAGED] %s instances = %d\n", qualified.c_str(), count);
    for (int i = 0; i < count && i < 4; ++i) il2cpp_inspect_fields(api, il2cpp_array_at(arr, i), qualified.c_str());
    std::fflush(stderr);
}

// A splash scene that waits for a video can never finish on a host without a video backend. Load the scene the build
// order puts after the current one through the guest's own SceneManager, so the player continues its normal flow.
static bool il2cpp_skip_stuck_video(uint64_t image_base, const char* binary_path, const char* game_dir,
                                   const std::vector<std::string>& scenes) {
    if (!guest_symtab_load(binary_path)) return false;
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) return false;
    if (il2cpp_video_state(api) != 1) return false;
    const std::string current = il2cpp_active_scene(api);
    std::string next;
    for (size_t i = 0; i + 1 < scenes.size(); ++i)
        if (scenes[i] == current) {
            next = scenes[i + 1];
            break;
        }
    if (!current.empty() && !next.empty()) {
        std::fprintf(stderr, "[MANAGED] video in scene \"%s\" cannot prepare; loading \"%s\"\n", current.c_str(),
                     next.c_str());
    } else {
        std::fprintf(stderr, "[MANAGED] video in scene \"%s\" cannot prepare and no next scene follows it\n",
                     current.c_str());
        return false;
    }
    void* domain = api.domain_get();
    if (api.thread_attach && domain) api.thread_attach(domain);
    void* scene_manager = il2cpp_class(api, "UnityEngine.SceneManagement", "SceneManager");
    void* mi = scene_manager ? il2cpp_find_load_scene(api, scene_manager, 0x0e) : nullptr;
    void* str = mi && api.string_new ? api.string_new(next.c_str()) : nullptr;
    if (!str) return false;
    int mode = 0;
    void* args[2] = {str, &mode};
    void* exc = nullptr;
    api.runtime_invoke(mi, nullptr, args, &exc);
    std::fprintf(stderr, "[MANAGED] LoadSceneAsync(\"%s\") exc=%p\n", next.c_str(), exc);
    std::fflush(stderr);
    return !exc;
}

// Advance one build scene when a startup screen has been shown for `stall_frames` without the player loading anything
// itself. Some titles gate their startup on platform services (Apple splash/iCloud/Game Center) that a host without
// those services cannot complete; this keeps such a title from sitting on a loading screen forever.
// Returns the scene name it loaded, or an empty string when it did nothing.
static std::string il2cpp_advance_stalled_scene(uint64_t image_base, const char* binary_path,
                                                const std::vector<std::string>& scenes, long stall_frames) {
    if (!guest_symtab_load(binary_path) || scenes.empty()) return std::string();
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) return std::string();
    const std::string current = il2cpp_active_scene(api);
    std::string next;
    for (size_t i = 0; i + 1 < scenes.size(); ++i)
        if (scenes[i] == current) {
            next = scenes[i + 1];
            break;
        }
    if (next.empty()) return std::string();
    if (stall_frames <= 0) return std::string();

    void* domain = api.domain_get();
    if (api.thread_attach && domain) api.thread_attach(domain);
    void* scene_manager = il2cpp_class(api, "UnityEngine.SceneManagement", "SceneManager");
    void* mi = scene_manager ? il2cpp_find_load_scene(api, scene_manager, 0x0e) : nullptr;
    void* str = mi && api.string_new ? api.string_new(next.c_str()) : nullptr;
    if (!str) return std::string();
    int mode = 0;
    void* args[2] = {str, &mode};
    void* exc = nullptr;
    api.runtime_invoke(mi, nullptr, args, &exc);
    std::fprintf(stderr, "[MANAGED] scene \"%s\" made no progress for %ld frames; loading \"%s\"\n", current.c_str(),
                 stall_frames, next.c_str());
    std::fflush(stderr);
    return exc ? std::string() : next;
}

// LoadSceneAsync has (string, LoadSceneMode) and (int, LoadSceneMode) overloads, so pick by parameter type:
// IL2CPP_TYPE_STRING is 0x0e, IL2CPP_TYPE_I4 is 0x08.
static void* il2cpp_find_load_scene(Il2CppApi& api, void* scene_manager, int first_param_type) {
    if (!api.class_get_methods || !api.method_get_name || !api.method_get_param) return nullptr;
    void* iter = nullptr;
    while (void* m = api.class_get_methods(scene_manager, &iter)) {
        const char* name = api.method_get_name(m);
        if (!name || std::strcmp(name, "LoadSceneAsync") != 0) continue;
        if (api.method_get_param_count(m) != 2) continue;
        void* p0 = api.method_get_param(m, 0);
        if (p0 && api.type_get_type(p0) == first_param_type) return m;
    }
    return nullptr;
}

// Host-driven scene command, "scene:<name>" or "sceneindex:<n>". Uses the game's own SceneManager so the target
// scene is loaded exactly as the game would load it.
static void il2cpp_managed_cmd(uint64_t image_base, const char* binary_path, const char* cmd) {
    if (!guest_symtab_load(binary_path)) {
        std::fprintf(stderr, "[MANAGED] cannot read the guest symbol table at %s\n", binary_path);
        return;
    }
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) {
        std::fprintf(stderr, "[MANAGED] il2cpp API not fully exported\n");
        return;
    }
    void* domain = api.domain_get();
    if (api.thread_attach && domain) api.thread_attach(domain);
    void* scene_manager = il2cpp_class(api, "UnityEngine.SceneManagement", "SceneManager");
    if (!scene_manager) {
        std::fprintf(stderr, "[MANAGED] SceneManager not found\n");
        return;
    }
    int mode = 0;   // LoadSceneMode.Single
    void* exc = nullptr;
    void* ret = nullptr;
    if (std::strncmp(cmd, "scene:", 6) == 0 && api.string_new) {
        const char* name = cmd + 6;
        void* mi = il2cpp_find_load_scene(api, scene_manager, 0x0e);
        void* str = api.string_new(name);
        if (!mi || !str) {
            std::fprintf(stderr, "[MANAGED] no LoadSceneAsync(string) overload (mi=%p str=%p)\n", mi, str);
            return;
        }
        void* args[2] = {str, &mode};
        ret = api.runtime_invoke(mi, nullptr, args, &exc);
        std::fprintf(stderr, "[MANAGED] LoadSceneAsync(\"%s\") -> %p exc=%p\n", name, ret, exc);
    } else if (std::strncmp(cmd, "sceneindex:", 11) == 0) {
        int index = atoi(cmd + 11);
        void* mi = il2cpp_find_load_scene(api, scene_manager, 0x08);
        if (!mi) {
            std::fprintf(stderr, "[MANAGED] no LoadSceneAsync(int) overload\n");
            return;
        }
        void* args[2] = {&index, &mode};
        ret = api.runtime_invoke(mi, nullptr, args, &exc);
        std::fprintf(stderr, "[MANAGED] LoadSceneAsync(index %d) -> %p exc=%p\n", index, ret, exc);
    } else {
        std::fprintf(stderr, "[MANAGED] unknown command '%s'\n", cmd);
        return;
    }
    std::fflush(stderr);
}

static void il2cpp_query_report(uint64_t image_base, const char* binary_path, int frame) {
    if (!guest_symtab_load(binary_path)) {
        std::fprintf(stderr, "[MANAGED] cannot read the guest symbol table at %s\n", binary_path);
        return;
    }
    Il2CppApi api = il2cpp_api(image_base);
    if (!api.ok) {
        std::fprintf(stderr, "[MANAGED] il2cpp API not fully exported (ok=%d)\n", api.ok ? 1 : 0);
        return;
    }
    void* domain = api.domain_get();
    if (api.thread_attach && domain) api.thread_attach(domain);

    std::fprintf(stderr, "[MANAGED] frame %d probe:\n", frame);
    int frames = -1, playing = -1;
    if (il2cpp_static_int(api, il2cpp_class(api, "UnityEngine", "Time"), "get_frameCount", &frames))
        std::fprintf(stderr, "[MANAGED]   Time.frameCount = %d\n", frames);
    if (il2cpp_static_int(api, il2cpp_class(api, "UnityEngine", "Application"), "get_isPlaying", &playing))
        std::fprintf(stderr, "[MANAGED]   Application.isPlaying = %d\n", playing);

    // Frame timing: a splash scene that waits on WaitForSeconds or Invoke never advances if deltaTime is 0.
    void* time_class = il2cpp_class(api, "UnityEngine", "Time");
    const struct { const char* name; const char* label; } clocks[] = {
        {"get_time", "Time.time"}, {"get_deltaTime", "Time.deltaTime"},
        {"get_unscaledTime", "Time.unscaledTime"}, {"get_unscaledDeltaTime", "Time.unscaledDeltaTime"},
        {"get_realtimeSinceStartup", "Time.realtimeSinceStartup"}, {"get_timeSinceLevelLoad", "Time.timeSinceLevelLoad"},
        {"get_smoothDeltaTime", "Time.smoothDeltaTime"},
    };
    for (const auto& c : clocks) {
        float v = -1;
        if (il2cpp_static_float(api, time_class, c.name, &v))
            std::fprintf(stderr, "[MANAGED]   %s = %.6f\n", c.label, v);
        else
            std::fprintf(stderr, "[MANAGED]   %s unavailable\n", c.label);
    }

    void* scene_manager = il2cpp_class(api, "UnityEngine.SceneManagement", "SceneManager");
    void* scene_class = il2cpp_class(api, "UnityEngine.SceneManagement", "Scene");
    int scene_count = -1;
    if (il2cpp_static_int(api, scene_manager, "get_sceneCount", &scene_count))
        std::fprintf(stderr, "[MANAGED]   SceneManager.sceneCount = %d\n", scene_count);

    // GetSceneAt returns a Scene struct by value; il2cpp_runtime_invoke hands back a boxed copy whose only field is
    // the int handle at +16, which the static GetNameInternal/GetRootCountInternal accessors take.
    if (scene_count > 0 && scene_count < 32) {
        void* get_at = api.class_get_method_from_name(scene_manager, "GetSceneAt", 1);
        for (int i = 0; i < scene_count && get_at; ++i) {
            void* args[1] = {&i};
            void* exc = nullptr;
            void* boxed = api.runtime_invoke(get_at, nullptr, args, &exc);
            if (exc || !boxed) {
                std::fprintf(stderr, "[MANAGED]   scene %d: GetSceneAt threw\n", i);
                continue;
            }
            const int handle = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(boxed) + 16);
            std::string name;
            int roots = -1;
            const bool named = il2cpp_static_string(api, scene_class, "GetNameInternal", handle, name);
            const bool counted = il2cpp_static_int_arg(api, scene_class, "GetRootCountInternal", handle, &roots);
            std::fprintf(stderr, "[MANAGED]   scene %d handle=%d name=\"%s\" roots=%d\n", i, handle,
                         named ? name.c_str() : "?", roots);
        }
    }

    void* object_class = il2cpp_class(api, "UnityEngine", "Object");
    int objects = -1, renderers = -1, cameras = -1, canvases = -1;
    if (il2cpp_find_count(api, object_class, "UnityEngine", "Object", &objects))
        std::fprintf(stderr, "[MANAGED]   FindObjectsOfType(Object) = %d\n", objects);
    if (il2cpp_find_count(api, object_class, "UnityEngine", "Renderer", &renderers))
        std::fprintf(stderr, "[MANAGED]   FindObjectsOfType(Renderer) = %d\n", renderers);
    if (il2cpp_find_count(api, object_class, "UnityEngine", "Camera", &cameras))
        std::fprintf(stderr, "[MANAGED]   FindObjectsOfType(Camera) = %d\n", cameras);
    if (il2cpp_find_count(api, object_class, "UnityEngine", "Canvas", &canvases))
        std::fprintf(stderr, "[MANAGED]   FindObjectsOfType(Canvas) = %d\n", canvases);

    // ECS bootstrap: DefaultWorldInitializationProxy creates the default world in Awake, so a null world after many
    // frames means managed scripts are not executing at all.
    void* world_class = il2cpp_class(api, "Unity.Entities", "World");
    if (world_class) {
        void* mi = api.class_get_method_from_name(world_class, "get_DefaultGameObjectInjectionWorld", 0);
        void* exc = nullptr;
        void* world = mi ? api.runtime_invoke(mi, nullptr, nullptr, &exc) : nullptr;
        if (exc) world = nullptr;
        std::fprintf(stderr, "[MANAGED]   World.DefaultGameObjectInjectionWorld = %p\n", world);
        if (world) {
            void* systems_mi = api.class_get_method_from_name(world_class, "get_Systems", 0);
            void* systems = systems_mi ? api.runtime_invoke(systems_mi, world, nullptr, &exc) : nullptr;
            std::fprintf(stderr, "[MANAGED]   World systems = %d\n",
                         (systems && !exc) ? api.array_length(systems) : -1);
        }
    }

    // VideoPlayer state: a splash scene that waits for a video to prepare or finish never advances when the host has
    // no video backend.
    void* video_player_class = il2cpp_class(api, "UnityEngine.Video", "VideoPlayer");
    if (video_player_class && object_class) {
        void* mi = api.class_get_method_from_name(object_class, "FindObjectsOfType", 1);
        void* type_obj = api.type_get_object(api.class_get_type(video_player_class));
        void* args[1] = {type_obj};
        void* exc = nullptr;
        void* arr = (mi && type_obj) ? api.runtime_invoke(mi, nullptr, args, &exc) : nullptr;
        const int count = (arr && !exc) ? api.array_length(arr) : 0;
        std::fprintf(stderr, "[MANAGED]   VideoPlayer instances = %d\n", count);
        for (int i = 0; i < count && i < 3; ++i) {
            void* vp = il2cpp_array_at(arr, i);
            std::string url;
            void* url_mi = il2cpp_get_method(api, video_player_class, "get_url");
            void* url_exc = nullptr;
            void* url_ret = (url_mi && vp) ? api.runtime_invoke(url_mi, vp, nullptr, &url_exc) : nullptr;
            if (url_ret && !url_exc) {
                const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(url_ret) + 16);
                const char16_t* chars = reinterpret_cast<const char16_t*>(reinterpret_cast<char*>(url_ret) + 20);
                if (len > 0 && len < 1024)
                    for (int32_t c = 0; c < len; ++c) url += chars[c] < 0x80 ? static_cast<char>(chars[c]) : '?';
            }
            double length = -1, time = -1;
            long long prepared = -1, playing = -1, frame_no = -1, wait_first = -1, loop = -1;
            il2cpp_get_number(api, vp, video_player_class, "get_length", &length);
            il2cpp_get_number(api, vp, video_player_class, "get_time", &time);
            il2cpp_get_int(api, vp, video_player_class, "get_isPrepared", &prepared);
            il2cpp_get_int(api, vp, video_player_class, "get_isPlaying", &playing);
            il2cpp_get_int(api, vp, video_player_class, "get_frame", &frame_no);
            il2cpp_get_int(api, vp, video_player_class, "get_waitForFirstFrame", &wait_first);
            il2cpp_get_int(api, vp, video_player_class, "get_isLooping", &loop);
            std::fprintf(stderr,
                         "[MANAGED]   video[%d] url=\"%s\" prepared=%lld playing=%lld length=%.3f time=%.3f "
                         "frame=%lld waitForFirstFrame=%lld looping=%lld\n",
                         i, url.c_str(), prepared, playing, length, time, frame_no, wait_first, loop);
        }
    }

    // What the scene actually holds: every live Object's concrete managed type, grouped and counted.
    if (object_class && api.class_get_name && api.class_get_namespace) {
        void* mi = api.class_get_method_from_name(object_class, "FindObjectsOfType", 1);
        void* type_obj = api.type_get_object(api.class_get_type(object_class));
        if (mi && type_obj) {
            void* args[1] = {type_obj};
            void* exc = nullptr;
            if (void* arr = api.runtime_invoke(mi, nullptr, args, &exc); arr && !exc) {
                const int len = api.array_length(arr);
                std::map<std::string, int> by_type;
                for (int i = 0; i < len; ++i) ++by_type[il2cpp_object_class_name(api, il2cpp_array_at(arr, i))];
                std::vector<std::pair<int, std::string>> top;
                for (auto& kv : by_type) top.emplace_back(kv.second, kv.first);
                std::sort(top.rbegin(), top.rend());
                std::fprintf(stderr, "[MANAGED]   live object types (%d):\n", len);
                for (size_t i = 0; i < top.size() && i < 120; ++i)
                    std::fprintf(stderr, "[MANAGED]     %4d  %s\n", top[i].first, top[i].second.c_str());
            }
        }
    }
    std::fflush(stderr);
}
