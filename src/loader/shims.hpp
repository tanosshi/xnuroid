#pragma once

#include "shim_types.hpp"
#include <string>
#include <unordered_map>

namespace loader {

class ShimRegistry {
public:
    static ShimRegistry& instance();

    void register_shim(const std::string& symbol, ShimHandler handler);
    ShimHandler find_shim(const std::string& symbol) const;
    bool dispatch(const std::string& symbol, CallFrame& frame, void* context = nullptr);

    void init_standard_shims();

private:
    std::unordered_map<std::string, ShimHandler> handlers_;
};

} // namespace loader
