/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Headless ARM64 execution check for an RGIMAGE7 made by prepare.py/link.py. */
#include "fex_runtime.h"
extern "C" {
#include "runtime.h"
#include "guest_call.h"
#include "gpu/tempest_gpu.h"
uintptr_t tempest_boot_finalizer(void);
}
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#ifdef __ANDROID__
#include <malloc.h>
#endif

namespace {
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
struct Reader {
    FILE *file;
    explicit Reader(const char *path) : file(std::fopen(path, "rb")) {
        if (!file) fail("cannot open prepared image");
    }
    ~Reader() { std::fclose(file); }
    void read(void *out, size_t size) {
        if (std::fread(out, 1, size, file) != size) fail("truncated prepared image");
    }
    uint64_t word() {
        std::array<unsigned char, 8> bytes;
        read(bytes.data(), bytes.size());
        uint64_t value = 0;
        for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
        return value;
    }
};
struct Segment { uint64_t address, size, flags; };
struct Relocation { uint64_t target, kind, value, addend; };
bool within(uint64_t address, uint64_t length, uint64_t begin, uint64_t size) {
    return address >= begin && address - begin <= size && length <= size - (address - begin);
}
void timeout(int) {
    static constexpr char message[] = "STOP: Android boot check timed out\n";
    auto ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    _exit(124);
}
int boot(const char *path, const char *app0, const char *user, bool enter) {
    Reader reader(path);
    char magic[8]; reader.read(magic, sizeof(magic));
    if (std::memcmp(magic, "RGIMAGE7", 8)) fail("boot check requires RGIMAGE7");
    uint64_t size = reader.word(), entry = reader.word(), ns = reader.word(), nr = reader.word();
    uint64_t ni = reader.word(), capabilities = reader.word();
    if (!size || size > 512 * 1024 * 1024 || entry >= size || !ns || ns > 64 ||
        nr > 1000000 || ni > 100000 || (capabilities & ~UINT64_C(3))) fail("image limits exceeded");
    uint64_t procparam = reader.word();
    std::array<uint64_t, 4> tls;
    for (auto &value : tls) value = reader.word();
    if (tls[1] > tls[2] || tls[2] > 1024 * 1024 || !within(tls[0], tls[1], 0, size) ||
        (tls[3] & (tls[3] - 1)) || tls[3] > 4096 || !within(procparam, 64, 0, size))
        fail("invalid process/TLS metadata");
    uint64_t nm = reader.word();
    if (!nm || nm > 32) fail("invalid module count");
    std::vector<RuntimeModule> modules(nm);
    for (auto &m : modules) {
        m.base = reader.word(); m.size = reader.word(); m.init = reader.word();
        m.tls_address = reader.word(); m.tls_memsz = reader.word(); m.tls_filesz = reader.word();
        m.tls_module = reader.word(); m.flags = reader.word(); m.procparam = reader.word();
        reader.read(m.name, sizeof(m.name));
        if (!m.size || !within(m.base, m.size, 0, size) || !within(m.init, 1, m.base, m.size) ||
            !std::memchr(m.name, 0, sizeof(m.name)) || (m.flags & ~UINT64_C(1)) ||
            (m.procparam && !within(m.procparam, 24, m.base, m.size)) ||
            (m.tls_module && (m.tls_module < 2 || m.tls_module >= RUNTIME_TLS_MODULES ||
             m.tls_filesz > m.tls_memsz || m.tls_memsz > 1024 * 1024 ||
             !within(m.tls_address, m.tls_memsz, 0, size)))) fail("invalid linked module");
    }
    uint64_t nb = reader.word();
    if (nb > ni) fail("invalid binding count");
    std::vector<uint64_t> bindings(ni), kinds(ni);
    for (uint64_t i = 0; i < nb; ++i) {
        uint64_t index = reader.word(), address = reader.word(), kind = reader.word();
        bool inside = false;
        for (const auto &m : modules) inside |= within(address, 1, m.base, m.size);
        if (index >= ni || !inside || (kind != 1 && kind != 2) || bindings[index]) fail("invalid binding");
        bindings[index] = address; kinds[index] = kind;
    }
    uint64_t ne = reader.word();
    if (ne > 1000000) fail("invalid export count");
    std::vector<RuntimeModuleExport> exports(ne);
    uint64_t previous = 0;
    for (auto &e : exports) {
        uint64_t index = reader.word(); reader.read(e.nid, sizeof(e.nid));
        e.address = reader.word(); e.kind = reader.word();
        if (index >= nm || index < previous || !std::memchr(e.nid, 0, sizeof(e.nid)) ||
            (e.kind != 1 && e.kind != 2) || !within(e.address, 1, modules[index].base, modules[index].size))
            fail("invalid module export");
        auto &m = modules[index];
        if (!m.exports) m.exports = &e;
        ++m.export_count; previous = index;
    }
    RuntimeModule main_module{};
    main_module.size = modules.front().base;
    std::strcpy(main_module.name, "eboot.bin");
    if (reader.word() != nm + 1) fail("invalid unwind count");
    for (uint64_t i = 0; i <= nm; ++i) {
        auto &m = i < nm ? modules[i] : main_module;
        m.eh_frame_hdr = reader.word(); m.eh_frame = reader.word(); m.eh_frame_size = reader.word();
        m.seg0 = reader.word(); m.seg0_size = reader.word();
        if (m.eh_frame_hdr >= size || !within(m.eh_frame, m.eh_frame_size, 0, size) ||
            !within(m.seg0, m.seg0_size, 0, size)) fail("invalid unwind metadata");
    }
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    const auto rounded = [page](size_t n) { return (n + page - 1) & ~(page - 1); };
    std::vector<Segment> segments(ns);
    for (auto &s : segments) {
        s.address = reader.word(); s.size = reader.word(); s.flags = reader.word();
        if (!within(s.address, s.size, 0, size) || s.address % page || s.flags > 7) fail("invalid segment");
    }
    const auto mapped = [&segments](uint64_t address, uint64_t length, bool execute = false) {
        for (auto &s : segments)
            if ((!execute || (s.flags & 1)) && within(address, length, s.address, s.size)) return true;
        return false;
    };
    if (!mapped(entry, 1, true) || !mapped(procparam, 64) || !mapped(tls[0], tls[1]))
        fail("unmapped process metadata");
    for (const auto &m : modules)
        if (!mapped(m.init, 1, true) || (m.tls_module && !mapped(m.tls_address, m.tls_memsz)))
            fail("unmapped module metadata");
    for (uint64_t i = 0; i < ni; ++i)
        if (bindings[i] && !mapped(bindings[i], 1, kinds[i] == 1)) fail("unmapped binding");
    std::vector<std::array<char, 128>> names(ni);
    for (auto &name : names) {
        reader.read(name.data(), name.size());
        if (!std::memchr(name.data(), 0, name.size())) fail("unterminated import name");
    }
    std::vector<Relocation> relocations(nr);
    for (auto &r : relocations) {
        r.target = reader.word(); r.kind = reader.word(); r.value = reader.word(); r.addend = reader.word();
        if (!mapped(r.target, 8) || r.kind > 2 || (r.kind && r.value >= ni) ||
            (!r.kind && r.value >= size) || (r.kind != 2 && r.addend) || r.addend >= page)
            fail("invalid relocation");
    }
    auto *image = static_cast<unsigned char *>(runtime_low_map(rounded(size), PROT_READ | PROT_WRITE));
    if (!image) fail("cannot map low guest image");
    reader.read(image, size);
    if (std::fgetc(reader.file) != EOF) fail("incorrect image size");
    runtime_start(capabilities & 1);
    if (capabilities & 2) runtime_content_link_provider();
    runtime_file_configure(app0, user);
#ifdef TEMPEST_ANDROID_ACTIVITY
    uint64_t sdk = 0;
    std::memcpy(&sdk, image + procparam + 16, 8);
    tempest_gpu_register_kernel();
    TempestGpuConfig gpu{"Tempest 4000", "CUSA09856", user, static_cast<uint32_t>(sdk), 0, 1280, 720, nullptr};
    if (tempest_gpu_init(&gpu)) fail("Android renderer initialization failed");
    if (tempest_render_4k()) runtime_kernel_set_neo_mode(1);
#endif
    auto *data_traps = static_cast<unsigned char *>(runtime_low_map((ni + 1) * page, PROT_NONE));
    if (!data_traps) fail("cannot map unsupported data imports");
    size_t native_count = 0, guest_count = 0, missing_count = 0;
    std::vector<std::array<uintptr_t, 2>> resolved(ni);
    for (const auto &r : relocations) {
        uintptr_t value = reinterpret_cast<uintptr_t>(image) + r.value;
        if (r.kind) {
            auto &slot = resolved[r.value][r.kind - 1];
            if (!slot) {
                const char *name = names[r.value].data();
                uintptr_t host = runtime_resolve(name, r.kind == 2);
                if (host) {
                    slot = r.kind == 2 ? host : tempest_fex_bind(name, host); ++native_count;
                } else if ((capabilities & 1) && bindings[r.value]) {
                    if (kinds[r.value] != r.kind || !mapped(bindings[r.value], r.addend + 1))
                        fail("binding kind/range mismatch");
                    slot = reinterpret_cast<uintptr_t>(image) + bindings[r.value]; ++guest_count;
                } else {
                    slot = r.kind == 2 ? reinterpret_cast<uintptr_t>(data_traps + r.value * page)
                                      : tempest_fex_missing(name);
                    ++missing_count;
                }
            }
            value = slot + r.addend;
        }
        std::memcpy(image + r.target, &value, 8);
    }
    for (const auto &s : segments) {
        // FEX reads executable x86 bytes; ARM64 never executes the image itself.
        int prot = ((s.flags & 5) ? PROT_READ : 0) | ((s.flags & 2) ? PROT_WRITE : 0);
        if (mprotect(image + s.address, rounded(s.size), prot)) fail("cannot protect guest segment");
        tempest_fex_add_range(reinterpret_cast<uintptr_t>(image + s.address), rounded(s.size),
                             s.flags & 1, s.flags & 2);
    }
    for (const auto &m : modules)
        if (m.tls_module) runtime_set_module_tls(m.tls_module, image + m.tls_address, m.tls_filesz, m.tls_memsz);
    runtime_set_main_tls(image + tls[0], tls[1], tls[2], tls[3]);
    runtime_set_procparam(image + procparam);
    runtime_module_register(image, modules.data(), modules.size());
    runtime_module_set_main(&main_module);
    runtime_thread_attach_main();
    runtime_orbis_fpu();
    std::printf("Android boot: image %p, %zu ARM adapters, %zu guest bindings, %zu unavailable imports\n",
                image, native_count, guest_count, missing_count);
    const auto &libc = modules.front();
    if (std::strcmp(libc.name, "libc.prx")) fail("first linked module must be libc.prx");
    std::printf("Android boot: starting %s at image offset 0x%llx\n", libc.name,
                static_cast<unsigned long long>(libc.init));
    const uintptr_t arguments[] = {0, 0, 0};
    int32_t result = static_cast<int32_t>(runtime_fex_call(reinterpret_cast<uintptr_t>(image + libc.init), arguments, 3));
    std::printf("Android boot: real PS4 libc initializer returned %d\n", result);
    if (result) fail("libc initialization failed");
    modules.front().started = 1;
    if (enter) {
        for (size_t i = 1; i < modules.size(); ++i) {
            auto &m = modules[i];
            if (m.flags & RUNTIME_MODULE_DEFERRED) continue;
            std::printf("Android boot: starting %s at image offset 0x%llx\n", m.name,
                        static_cast<unsigned long long>(m.init));
            int32_t status = static_cast<int32_t>(runtime_fex_call(
                reinterpret_cast<uintptr_t>(image + m.init), arguments, 3));
            if (status) fail("linked module initialization failed");
            m.started = 1;
        }
        struct Parameters { uint64_t argc; const char *argv[2]; char program[16]; };
        auto *parameters = static_cast<Parameters *>(runtime_low_alloc(sizeof(Parameters)));
        if (!parameters) fail("cannot allocate guest arguments");
        parameters->argc = 1;
        std::strcpy(parameters->program, "eboot.bin");
        parameters->argv[0] = parameters->program;
        parameters->argv[1] = nullptr;
        const uintptr_t entry_args[] = {reinterpret_cast<uintptr_t>(parameters),
            tempest_fex_bind("process finalizer", tempest_boot_finalizer())};
        std::printf("Android boot: entering original game code at image offset 0x%llx\n",
                    static_cast<unsigned long long>(entry));
        runtime_fex_call(reinterpret_cast<uintptr_t>(image + entry), entry_args, 2);
        fail("game entry unexpectedly returned");
    }
    return 0;
}
}
#ifdef TEMPEST_ANDROID_ACTIVITY
int tempest_android_boot(const char *image, const char *app0, const char *user) {
    tempest_fex_signals();
    signal(SIGALRM, timeout);
    unsigned seconds=0;
    if (const char* value=std::getenv("TEMPEST_ANDROID_TIMEOUT")) {
        char* end=nullptr;
        unsigned long parsed=std::strtoul(value,&end,10);
        if (*value && end && !*end && parsed<=3600) seconds=static_cast<unsigned>(parsed);
    }
    alarm(seconds);
    try { return boot(image, app0, user, true); }
    catch (const std::exception &e) { std::fprintf(stderr, "STOP: Android boot: %s\n", e.what()); return 1; }
}
#else
int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc != 4 && (argc != 5 || std::strcmp(argv[4], "--entry"))) {
        std::fprintf(stderr, "Usage: tempest-boot-check boot-linked.bin APP0_DIR USER_DIR [--entry]\n");
        return 2;
    }
#if defined(__ANDROID__) && defined(M_BIONIC_SET_HEAP_TAGGING_LEVEL)
    // Guest x86 pointers cannot carry Android's top-byte native heap tags.
    if (!mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE)) {
        std::fputs("STOP: cannot disable native heap pointer tagging\n", stderr);
        return 1;
    }
#endif
    tempest_fex_signals();
    signal(SIGALRM, timeout);
    alarm(30);
    int result = 1;
    try { result = boot(argv[1], argv[2], argv[3], argc == 5); }
    catch (const std::exception &e) { std::fprintf(stderr, "STOP: Android boot: %s\n", e.what()); }
    std::fflush(nullptr);
    _exit(result); // Guest workers may still be live; do not tear down their CPU backend.
}
#endif
