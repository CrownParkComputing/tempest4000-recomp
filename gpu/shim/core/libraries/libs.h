// tempest: shadPS4 registers HLE functions with LIB_FUNCTION(nid, library,
// version, module, fn). Here the registrations fill a table that the C
// loader queries by NID (see tempest_gpu.cpp: tempest_gpu_resolve).
#pragma once
#include <string>
#include "common/types.h"

namespace Core::Loader {
enum class SymbolType { Function, Object };
class SymbolsResolver {
public:
    void AddSymbol(const char* nid, const char* library, const char* module, SymbolType type, u64 address);
};
} // namespace Core::Loader

#ifdef TEMPEST_FEX
#include "fex_runtime.h"
#include "core/guest_cpu/hle_call_adapter.h"
// tempest: retain the actual C++ function type when registering an ARM64 graphics API.
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    do {                                                                                       \
        tempest_fex_register_adapter(reinterpret_cast<uintptr_t>(function),                     \
            Core::GuestCpu::MakeHleCallAdapter(function));                                       \
        sym->AddSymbol(nid, lib, mod, Core::Loader::SymbolType::Function,                         \
                       reinterpret_cast<u64>(function));                                       \
    } while (false)
#else
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    sym->AddSymbol(nid, lib, mod, Core::Loader::SymbolType::Function,                             \
                   reinterpret_cast<u64>(function))
#endif
#define LIB_OBJ(nid, lib, libversion, mod, obj)                                                    \
    sym->AddSymbol(nid, lib, mod, Core::Loader::SymbolType::Object, reinterpret_cast<u64>(obj))
