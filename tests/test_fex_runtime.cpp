/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x86-64 guest calls into the real Tempest NGS2 runtime on an ARM64 host. */
#undef NDEBUG
#include "core/guest_cpu/fex_guest_cpu.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "fex_c_adapter.h"
#include <FEXCore/Core/X86Enums.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include "runtime.h"
extern const TempestFexExport tempest_fex_runtime_ngs2[];
extern const size_t tempest_fex_runtime_ngs2_count;
extern const TempestFexExport tempest_fex_fex_abi_fixture[];
extern const size_t tempest_fex_fex_abi_fixture_count;
void *runtime_low_alloc(size_t size) {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *p = mmap(nullptr, (size + page - 1) & ~(page - 1), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (reinterpret_cast<uintptr_t>(p) >= (UINT64_C(1) << 40)) {
        munmap(p, (size + page - 1) & ~(page - 1));
        return nullptr;
    }
    return p;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!std::strcmp(table[i].name, name)) return reinterpret_cast<uintptr_t>(table[i].function);
    return 0;
}
}

namespace {
using Ranges = std::vector<Core::GuestExecutionRange>;
thread_local const Ranges *active_ranges;
struct Stack {
    size_t size;
    void *address;
    explicit Stack(size_t bytes) : size(bytes), address(mmap(nullptr, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) { assert(address != MAP_FAILED); }
    ~Stack() { assert(munmap(address, size) == 0); }
};
bool validate(void *context, uintptr_t address, size_t size, bool writable) {
    if (!address || !size || address > UINTPTR_MAX - size) return false;
    for (const auto &r : active_ranges ? *active_ranges : *static_cast<Ranges *>(context))
        if (address >= r.Begin && address - r.Begin <= r.Size &&
            size <= r.Size - (address - r.Begin) && (!writable || r.Writable)) return true;
    return false;
}
void fault(int signal, siginfo_t *info, void *context) {
    if (!Core::Fex::HandleGuestSignal(signal, info, context)) _exit(128 + signal);
}
uintptr_t bind(Core::GuestCpu::HleCallRegistry &registry, Core::GuestCpu::HleVeneerAllocator &veneers,
               const char *name) {
    auto address = runtime_ngs2_resolve(name);
    std::shared_ptr<Core::GuestCpu::HleCallAdapter> adapter;
    for (size_t i = 0; i < tempest_fex_runtime_ngs2_count; ++i)
        if (address && reinterpret_cast<uintptr_t>(tempest_fex_runtime_ngs2[i].function) == address)
            adapter = registry.Register(std::make_shared<Tempest::CAdapter>(tempest_fex_runtime_ngs2[i]), name);
    for (size_t i = 0; i < tempest_fex_fex_abi_fixture_count; ++i)
        if (!std::strcmp(tempest_fex_fex_abi_fixture[i].name, name))
            adapter = registry.Register(std::make_shared<Tempest::CAdapter>(tempest_fex_fex_abi_fixture[i]), name);
    assert(adapter);
    auto result = veneers.Allocate(*adapter);
    assert(std::holds_alternative<u64>(result));
    return std::get<u64>(result);
}
}

int main() {
    struct sigaction action {};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGBUS, &action, nullptr) == 0);

    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto *memory = static_cast<unsigned char *>(mmap(nullptr, page * 3, PROT_READ | PROT_WRITE,
                                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    auto *guest_code = static_cast<unsigned char *>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(guest_code != MAP_FAILED);
    const unsigned char code[] = {0x65,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3, /* mov rax,gs:[0]; ret */
                                   0x48,0x8b,0x07,0xc3}; /* mov rax,[rdi]; ret */
    std::memcpy(guest_code, code, sizeof(code));
    assert(mprotect(guest_code, page, PROT_READ | PROT_EXEC) == 0);
    Core::GuestCpu::HleCallRegistry registry;
    Core::GuestCpu::HleVeneerAllocator veneers;
    auto frame_info = bind(registry, veneers, "sceNgs2GetWaveformFrameInfo");
    auto create = bind(registry, veneers, "sceNgs2SystemCreateWithAllocator");
    auto destroy = bind(registry, veneers, "sceNgs2SystemDestroy");
    auto negative = bind(registry, veneers, "fixture_negative");
    auto store = bind(registry, veneers, "fixture_store");
    auto mixed = bind(registry, veneers, "fixture_mixed");
    Ranges ranges = veneers.GetExecutableRanges();
    ranges.push_back({reinterpret_cast<uintptr_t>(memory), page * 3, false, true});
    ranges.push_back({reinterpret_cast<uintptr_t>(guest_code), page, true, false});
    Core::GuestCpu::HleGuestBridge bridge(registry, validate, &ranges);
    auto created = Core::FexGuestCpuBackend::Create(bridge);
    assert(std::holds_alternative<std::unique_ptr<Core::FexGuestCpuBackend>>(created));
    auto backend = std::move(std::get<std::unique_ptr<Core::FexGuestCpuBackend>>(created));
    ranges.push_back(backend->ReturnRange());
    const uintptr_t return_address = backend->ReturnAddress();
    const auto execute = [&](uintptr_t entry, std::initializer_list<uint64_t> arguments,
                             std::initializer_list<double> floating, uintptr_t gs_base = 0) {
        Core::GuestExecutionRequest request;
        request.Rip = entry;
        /* Each caller needs its own stack when two guest threads execute concurrently. */
        thread_local Stack thread_stack(page);
        request.Rsp = reinterpret_cast<uintptr_t>(thread_stack.address) + page - 24;
        request.GsBase = gs_base;
        std::memcpy(reinterpret_cast<void *>(request.Rsp), &return_address, sizeof(uint64_t));
        constexpr size_t registers[] = {FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI,
            FEXCore::X86State::REG_RDX, FEXCore::X86State::REG_RCX,
            FEXCore::X86State::REG_R8, FEXCore::X86State::REG_R9};
        size_t index = 0;
        for (uint64_t argument : arguments) {
            if (index < std::size(registers)) request.Gpr[registers[index]] = argument;
            else std::memcpy(reinterpret_cast<void *>(request.Rsp + 8 * (index - 5)), &argument, 8);
            ++index;
        }
        index = 0;
        for (double argument : floating) {
            assert(index < 8);
            std::memcpy(&request.Xmm[index++][0], &argument, sizeof(argument));
        }
        request.MappedRanges = ranges;
        request.MappedRanges.push_back({reinterpret_cast<uintptr_t>(thread_stack.address), page, false, true});
        active_ranges = &request.MappedRanges;
        auto result = backend->Run(request);
        active_ranges = nullptr;
        if (auto *failure = std::get_if<Core::GuestExecutionFailure>(&result)) {
            std::fprintf(stderr, "Tempest FEX check failed at entry 0x%lx, stage %d: %d\n",
                         entry, static_cast<int>(failure->Stage), failure->Error);
            std::abort();
        }
        auto &state = std::get<Core::GuestExecutionState>(result);
        assert(state.StopReason == Core::GuestStopReason::Halted);
        if (state.Rip != backend->ReturnAddress() && state.Rip != backend->ReturnAddress() + 1) {
            std::fprintf(stderr, "Unexpected guest stop RIP 0x%lx (return veneer 0x%lx)\n",
                         state.Rip, backend->ReturnAddress());
            std::abort();
        }
        return state;
    };
    const auto call = [&](uintptr_t entry, std::initializer_list<uint64_t> arguments) {
        return static_cast<int32_t>(execute(entry, arguments, {}).Gpr[FEXCore::X86State::REG_RAX]);
    };
    uint32_t format[] = {0x40, 2, 48000, 0xfe740bf0, 0, 0};
    std::memcpy(memory, format, sizeof(format));
    const auto at = [&](size_t offset) { return reinterpret_cast<uintptr_t>(memory + offset); };
    assert(call(frame_info, {at(0), at(32), at(36), at(40), at(44)}) == 0);
    uint32_t geometry[4];
    std::memcpy(geometry, memory + 32, sizeof(geometry));
    assert(geometry[0] == 384 && geometry[1] == 1024 && geometry[2] == 4 && geometry[3] == 256);
    assert(call(create, {0, 0, at(64)}) == 0);
    uintptr_t system;
    std::memcpy(&system, memory + 64, sizeof(system));
    assert(system && system < (UINT64_C(1) << 40));
    assert(call(destroy, {system, 0}) == 0);
    assert(munmap(reinterpret_cast<void *>(system), page) == 0);
    assert(execute(negative, {}, {}).Gpr[FEXCore::X86State::REG_RAX] == UINT64_C(0xffffffdb));
    execute(store, {at(80), UINT64_C(0x123456789abcdef0)}, {});
    uint64_t stored;
    std::memcpy(&stored, memory + 80, sizeof(stored));
    assert(stored == UINT64_C(0x123456789abcdef0));
    auto mixed_result = execute(mixed, {1, 2, 3, 4, 5, 6, 7}, {1.5, 2.25});
    double value;
    std::memcpy(&value, &mixed_result.Xmm[0][0], sizeof(value));
    assert(value == 31.375);
    const uint64_t tls[2] = {UINT64_C(0x1234567890abcdef), UINT64_C(0xfedcba0987654321)};
    std::memcpy(memory + 96, tls, sizeof(tls));
    std::thread first([&] {
        assert(execute(reinterpret_cast<uintptr_t>(guest_code), {}, {}, at(96)).Gpr[0] == tls[0]);
    });
    std::thread second([&] {
        assert(execute(reinterpret_cast<uintptr_t>(guest_code), {}, {}, at(104)).Gpr[0] == tls[1]);
    });
    first.join(); second.join();
    std::memcpy(memory + 113, &tls[0], sizeof(tls[0]));
    assert(execute(reinterpret_cast<uintptr_t>(guest_code + 10), {at(113)}, {}).Gpr[0] == tls[0]);
    backend.reset();
    assert(munmap(guest_code, page) == 0);
    assert(munmap(memory, page * 3) == 0);
    std::puts("Tempest FEX: x86 guest -> ARM64 NGS2 geometry and system lifecycle passed");
    std::puts("Tempest FEX: generated adapters preserve pointers, void, signed returns, FP and stack arguments");
    std::puts("Tempest FEX: concurrent x86 threads, GS TLS and unaligned loads passed");
    return 0;
}
