/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "fex_runtime.h"
#include "fex_c_adapter.h"
#include "core/guest_cpu/fex_guest_cpu.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include <FEXCore/Core/X86Enums.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <unordered_map>
#include <unwind.h>
extern "C" {
#include "runtime.h"
#include "guest_call.h"
#include "gpu/tempest_gpu.h"
#define FEX_MODULE(stem) extern const TempestFexExport tempest_fex_##stem[]; \
    extern const size_t tempest_fex_##stem##_count;
#include "runtime_registry.inc"
#undef FEX_MODULE
}

namespace {
using Range = Core::GuestExecutionRange;
using Ranges = std::vector<Range>;
thread_local const Ranges *active_ranges;
struct Runtime {
    std::mutex lock;
    Ranges ranges;
    Core::GuestCpu::HleCallRegistry registry;
    Core::GuestCpu::HleVeneerAllocator veneers;
    std::unordered_map<uintptr_t, uintptr_t> bindings;
    std::unordered_map<uintptr_t, std::shared_ptr<Core::GuestCpu::HleCallAdapter>> adapters;
    std::unique_ptr<Core::GuestCpu::HleGuestBridge> bridge;
    std::unique_ptr<Core::FexGuestCpuBackend> backend;
};
Runtime &runtime() { static Runtime value; return value; }

bool contains(const Range &r, uintptr_t address, size_t size, bool writable) {
    return address >= r.Begin && address - r.Begin <= r.Size && size <= r.Size - (address - r.Begin)
        && (!writable || r.Writable);
}
bool validate(void *, uintptr_t address, size_t size, bool writable) {
    if (!address || !size || address > UINTPTR_MAX - size) return false;
    if (active_ranges)
        for (const auto &r : *active_ranges) if (contains(r, address, size, writable)) return true;
    return runtime_memory_is_mapped(address, size) != 0;
}
std::optional<Range> executable(void *, uintptr_t address) {
    auto &s = runtime();
    if (auto veneer = s.veneers.QueryExecutableRange(address)) return veneer;
    uintptr_t begin, end;
    if (runtime_memory_executable_range(address, &begin, &end))
        return Range{begin, end - begin, true, false};
    return std::nullopt;
}
[[noreturn]] void failed(const char *what, int stage, int error) {
    std::fprintf(stderr, "STOP: FEX %s failed (stage %d, error %d)\n", what, stage, error);
    std::fflush(nullptr);
    _exit(21);
}
void initialize() {
    auto &s = runtime();
    std::lock_guard guard(s.lock);
    if (s.backend) return;
    s.bridge = std::make_unique<Core::GuestCpu::HleGuestBridge>(s.registry, validate, nullptr,
        [](void *, const Core::GuestCpu::HleCallFailure &failure) {
            std::fprintf(stderr, "STOP: Android API adapter failed: %.*s (error %d)\n",
                static_cast<int>(failure.name.size()), failure.name.data(), failure.error);
            std::fflush(nullptr); _exit(21);
        }, nullptr, executable, nullptr);
    auto result = Core::FexGuestCpuBackend::Create(*s.bridge);
    if (auto *failure = std::get_if<Core::GuestExecutionFailure>(&result))
        failed("initialization", static_cast<int>(failure->Stage), failure->Error);
    s.backend = std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend>>(result));
}
class Missing final : public Core::GuestCpu::HleCallAdapter {
public:
    Core::GuestCpu::HleCallResult Invoke(Core::GuestCpu::HleCallFrame &) const override {
        std::fprintf(stderr, "STOP: unsupported PS4 import in Android bootstrap: %.*s\n",
                     static_cast<int>(Name().size()), Name().data());
        std::fflush(nullptr);
        _exit(20);
    }
};
uintptr_t veneer(std::shared_ptr<Core::GuestCpu::HleCallAdapter> adapter, const char *name) {
    auto &s = runtime();
    auto registered = s.registry.Register(std::move(adapter), name);
    auto result = s.veneers.Allocate(*registered);
    if (auto *failure = std::get_if<Core::GuestCpu::HleVeneerFailure>(&result))
        failed("veneer", 0, failure->error);
    return std::get<u64>(result);
}
const TempestFexExport *find(uintptr_t function) {
#define FEX_MODULE(stem) \
    for (size_t i = 0; i < tempest_fex_##stem##_count; ++i) \
        if (reinterpret_cast<uintptr_t>(tempest_fex_##stem[i].function) == function) \
            return &tempest_fex_##stem[i];
#include "runtime_registry.inc"
#undef FEX_MODULE
    return nullptr;
}
void fault(int signal, siginfo_t *info, void *context) {
    if (signal == SIGSEGV && tempest_gpu_handle_fault(context, info->si_addr)) return;
    if (Core::Fex::HandleGuestSignal(signal, info, context)) return;
    if (runtime_fault_recover) {
        auto *recover = runtime_fault_recover;
        runtime_fault_recover = nullptr;
        siglongjmp(*recover, 1);
    }
    uint64_t rip = 0, operation = 0;
    Core::Fex::BachataQueryGuestRipSyscall(&rip, &operation);
    std::fprintf(stderr, "Fault: Android signal %d at %p, host PC 0x%llx, guest RIP 0x%llx, operation %llu\n",
        signal, info->si_addr, static_cast<unsigned long long>(static_cast<ucontext_t *>(context)->uc_mcontext.pc),
        static_cast<unsigned long long>(rip), static_cast<unsigned long long>(operation));
    Dl_info location{};
    uintptr_t pc = static_cast<ucontext_t *>(context)->uc_mcontext.pc;
    if (dladdr(reinterpret_cast<void *>(pc), &location))
        std::fprintf(stderr, "  PC: %s+0x%lx (%s)\n", location.dli_fname,
            static_cast<unsigned long>(pc - reinterpret_cast<uintptr_t>(location.dli_fbase)),
            location.dli_sname ? location.dli_sname : "?");
    int depth = 0;
    _Unwind_Backtrace([](_Unwind_Context *frame, void *data) {
        auto &depth = *static_cast<int *>(data);
        uintptr_t address = _Unwind_GetIP(frame);
        Dl_info entry{};
        if (address && dladdr(reinterpret_cast<void *>(address), &entry))
            std::fprintf(stderr, "  #%d %s+0x%lx (%s)\n", depth, entry.dli_fname,
                static_cast<unsigned long>(address - reinterpret_cast<uintptr_t>(entry.dli_fbase)),
                entry.dli_sname ? entry.dli_sname : "?");
        return ++depth < 16 ? _URC_NO_REASON : _URC_END_OF_STACK;
    }, &depth);
    std::fflush(nullptr);
    _exit(128 + signal);
}
struct GuestStack {
    static constexpr size_t size = 16 * 1024 * 1024;
    void *address = runtime_low_map(size, PROT_READ | PROT_WRITE);
    GuestStack() {
        if (!address) failed("stack allocation", 0, ENOMEM);
        runtime_thread_set_stack(address, size);
    }
    ~GuestStack() { munmap(address, size); }
};
}

void tempest_fex_add_range(uintptr_t address, size_t size, bool execute, bool writable) {
    auto &s = runtime();
    std::lock_guard guard(s.lock);
    s.ranges.push_back({address, size, execute, writable});
}
uintptr_t tempest_fex_bind(const char *name, uintptr_t native_function) {
    auto &s = runtime();
    std::lock_guard guard(s.lock);
    if (auto found = s.bindings.find(native_function); found != s.bindings.end()) return found->second;
    auto *definition = find(native_function);
    auto adapter = s.adapters.find(native_function);
    if (!definition && adapter == s.adapters.end()) {
        std::fprintf(stderr, "STOP: native PS4 function has no typed ARM adapter: %s (%p)\n",
                     name, reinterpret_cast<void *>(native_function));
        _exit(21);
    }
    uintptr_t address = veneer(definition ? std::make_shared<Tempest::CAdapter>(*definition)
                                          : adapter->second, name);
    s.bindings.emplace(native_function, address);
    return address;
}
void tempest_fex_register_adapter(uintptr_t function, std::shared_ptr<Core::GuestCpu::HleCallAdapter> adapter) {
    auto &s = runtime();
    std::lock_guard guard(s.lock);
    s.adapters[function] = std::move(adapter);
}
uintptr_t tempest_fex_missing(const char *name) { return veneer(std::make_shared<Missing>(), name); }
void tempest_fex_signals() {
    struct sigaction action {};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGBUS, SIGSEGV, SIGILL, SIGSYS}) sigaction(signal, &action, nullptr);
}

extern "C" uintptr_t runtime_fex_call(uintptr_t entry, const uintptr_t *arguments, size_t count) {
    initialize();
    auto &s = runtime();
    auto nested = s.backend->CallGuest(entry, std::span<const uint64_t>(arguments, count));
    if (auto *state = std::get_if<Core::GuestExecutionState>(&nested)) return state->Gpr[0];
    auto failure = std::get<Core::GuestExecutionFailure>(nested);
    if (failure.Stage != Core::GuestExecutionStage::Thread || failure.Error != ENXIO)
        failed("callback", static_cast<int>(failure.Stage), failure.Error);
    thread_local GuestStack stack;
    Core::GuestExecutionRequest request;
    request.Rip = entry;
    request.Rsp = reinterpret_cast<uintptr_t>(stack.address) + stack.size - 128 - 8;
    request.Rflags = 2;
    request.GsBase = runtime_thread_tcb();
    uint64_t ret = s.backend->ReturnAddress();
    std::memcpy(reinterpret_cast<void *>(request.Rsp), &ret, sizeof(ret));
    constexpr size_t registers[] = {7, 6, 2, 1, 8, 9};
    if (count > 7) failed("argument count", 0, E2BIG);
    for (size_t i = 0; i < count; ++i) {
        if (i < 6) request.Gpr[registers[i]] = arguments[i];
        else std::memcpy(reinterpret_cast<void *>(request.Rsp + 8), &arguments[i], 8);
    }
    { std::lock_guard guard(s.lock); request.MappedRanges = s.ranges; }
    auto veneers = s.veneers.GetExecutableRanges();
    request.MappedRanges.insert(request.MappedRanges.end(), veneers.begin(), veneers.end());
    request.MappedRanges.push_back(s.backend->ReturnRange());
    request.MappedRanges.push_back(s.backend->CallbackReturnRange());
    request.MappedRanges.push_back({reinterpret_cast<uintptr_t>(stack.address), stack.size, false, true});
    active_ranges = &request.MappedRanges;
    auto result = s.backend->Run(request);
    active_ranges = nullptr;
    if (auto *error = std::get_if<Core::GuestExecutionFailure>(&result))
        failed("guest execution", static_cast<int>(error->Stage), error->Error);
    const auto &state = std::get<Core::GuestExecutionState>(result);
    if (state.Rip != ret && state.Rip != ret + 1) failed("unexpected guest halt", 0, EPROTO);
    return state.Gpr[0];
}
