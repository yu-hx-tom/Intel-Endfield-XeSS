#include "fsr_local_fi_off.hpp"
#include <windows.h>
#include <MinHook.h>
#include "../third_party/minhook/src/hde/hde64.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

struct Snapshot { std::uint64_t gpr[16]; std::uint8_t xmm[16][16]; std::uint64_t flags; };
static_assert(sizeof(Snapshot) == 392);
extern "C" {
Snapshot fst_before{}, fst_after{};
std::uint32_t fst_taken{};
void fst_invoke0(void*); void fst_invoke1(void*);
extern const std::uint8_t fst_cmp0[], fst_cmp1[], fst_resume0[], fst_resume1[];
}
namespace {
namespace local = xess_vk::fsr_local_fi_off;
std::size_t checks{};
void require(bool v, const char* message) { ++checks; if (!v) throw std::runtime_error(message); }
void mh(MH_STATUS s) { require(s == MH_OK, MH_StatusToString(s)); }
std::uint64_t cmpFlags(std::uint8_t v) {
    return (!v ? 0x40 : 0) | ((std::popcount(v) & 1) ? 0 : 4) | (v & 0x80);
}
void execute(bool enabled) {
    for (std::size_t site = 0; site < 2; ++site) {
        for (std::uint8_t value : {0, 1, 0x80, 0xff}) {
            std::array<std::uint8_t, 64> view{}; view[0x3b] = value;
            SetLastError(0x19283746);
            (site ? fst_invoke1 : fst_invoke0)(view.data());
            require(GetLastError() == 0x19283746, "stub changed LastError");
            require(std::memcmp(fst_before.gpr, fst_after.gpr, sizeof(fst_before.gpr)) == 0, "GPR/RSP changed");
            require(std::memcmp(fst_before.xmm, fst_after.xmm, sizeof(fst_before.xmm)) == 0, "XMM changed");
            require((fst_after.flags & 0x8d5) == cmpFlags(enabled ? 1 : value), "CMP arithmetic flags differ");
            require((fst_after.flags & 0x400) == (fst_before.flags & 0x400), "DF changed");
            require(fst_taken == (enabled || value != 0), "original remaining JNE took wrong branch");
            require(view[0x3b] == value, "shared view changed");
        }
    }
}
void compareUnwind(const CONTEXT& a, const CONTEXT& b) {
    require(a.Rsp == b.Rsp && a.Rip == b.Rip, "unwind RSP/RIP mismatch");
    require(a.Rbx == b.Rbx && a.Rbp == b.Rbp && a.Rsi == b.Rsi && a.Rdi == b.Rdi &&
        a.R12 == b.R12 && a.R13 == b.R13 && a.R14 == b.R14 && a.R15 == b.R15, "unwind nonvolatile GPR mismatch");
    require(std::memcmp(&a.Xmm6, &b.Xmm6, 10 * sizeof(M128A)) == 0, "unwind XMM6..15 mismatch");
}
std::size_t unwind() {
    // Actual Unity chained xdata, rebased into an inert private reference image.
    // The reference PC is a NOP BODY, never our synthetic stub's own metadata.
    std::array<std::uint8_t, 4096> image{};
    const auto guards = local::bodyGuards();
    std::memcpy(image.data() + 0x800, guards[5].bytes, guards[5].size);
    std::memcpy(image.data() + 0x840, guards[4].bytes, guards[4].size);
    std::memcpy(image.data() + 0x880, guards[3].bytes, guards[3].size);
    const RUNTIME_FUNCTION root{0x100, 0x180, 0x800}, middle{0x180, 0x200, 0x840}, body{0x200, 0x280, 0x880};
    std::memcpy(image.data() + 0x840 + 28, &root, sizeof(root));
    std::memcpy(image.data() + 0x880 + 4, &middle, sizeof(middle));
    std::memset(image.data() + 0x200, 0x90, 0x80);
    std::vector<std::uint64_t> stack(0xb500 / 8);
    const auto rsp = (reinterpret_cast<std::uintptr_t>(stack.data()) + 15) & ~std::uintptr_t(15);
    const auto store = [&](std::size_t offset, std::uint64_t v) { std::memcpy(reinterpret_cast<void*>(rsp + offset), &v, 8); };
    for (std::size_t offset : {0xb350, 0xb358, 0xb360, 0xb368, 0xb370, 0xb378, 0xb380, 0xb3a0}) store(offset, 0xa0000000 + offset);
    for (std::size_t offset : {0xb2e0, 0xb2f0, 0xb300, 0xb310, 0xb320, 0xb330, 0xb340}) {
        store(offset, 0xbb000000 + offset); store(offset + 8, 0xcc000000 + offset);
    }
    store(0xb388, reinterpret_cast<std::uintptr_t>(&unwind));
    CONTEXT initial{}; initial.ContextFlags = CONTEXT_ALL; initial.Rsp = rsp;
    std::memset(&initial.Xmm6, 0x59, 10 * sizeof(M128A));
    const auto run = [](CONTEXT c, DWORD64 base, DWORD64 pc, const RUNTIME_FUNCTION* fn) {
        c.Rip = pc; PVOID handler{}; DWORD64 establisher{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, const_cast<RUNTIME_FUNCTION*>(fn), &c, &handler, &establisher, nullptr);
        return c;
    };
    const auto reference = run(initial, reinterpret_cast<DWORD64>(image.data()), reinterpret_cast<DWORD64>(image.data() + 0x220), &body);
    require(reference.Rsp == rsp + 0xb390 && reference.Rip == reinterpret_cast<DWORD64>(&unwind), "original Unity reference unwind invalid");
    require(reference.Rbx == 0xa000b3a0 && reference.R15 == 0xa000b350 && reference.Xmm6.Low == 0xbb00b340, "original reference did not restore fixed BODY");
    std::size_t points{};
    for (std::size_t site = 0; site < 2; ++site) {
        const auto start = reinterpret_cast<std::uintptr_t>(local::entry(site));
        const auto end = start + 40;
        const auto original = reinterpret_cast<std::uintptr_t>(*local::originalSlot(site));
        for (const auto [begin, limit] : {std::pair{start, end}, std::pair{original, original + 33}}) {
            // Decode the actual MinHook object after conversion, including both
            // branches of every complementary pair. Immediates are not PCs.
            for (auto pc = begin; pc < limit;) {
                hde64s instruction{};
                const auto length = hde64_disasm(reinterpret_cast<void*>(pc), &instruction);
                require(length && !(instruction.flags & F_ERROR) && length <= limit - pc, "invalid island instruction");
                require(instruction.opcode != 0xff && instruction.opcode != 0xe9 && instruction.opcode != 0xeb,
                    "island retains epilogue tail-JMP");
                DWORD64 base{}; auto* fn = RtlLookupFunctionEntry(pc, &base, nullptr);
                require(fn != nullptr, "missing island RUNTIME_FUNCTION");
                const auto observed = run(initial, base, pc, fn);
                if (observed.Rsp != reference.Rsp || observed.Rip != reference.Rip) {
                    std::cerr << "unwind site=" << site << " pcOffset=" << pc-begin << " code=" << std::hex << begin
                              << " rsp=" << observed.Rsp << "/" << reference.Rsp << " rip=" << observed.Rip << "/" << reference.Rip
                              << std::dec << '\n';
                }
                compareUnwind(observed, reference); ++points;
                pc += length;
            }
        }
    }
    return points;
}
}
int main(int argc, char** argv) {
    try {
        require(!GetModuleHandleW(L"UnityPlayer.dll"), "CPU fixture must not load game");
        require(!local::active() && !local::activate(), "certificate published before sites ready");
        require(!local::configureSite(2, nullptr, nullptr), "invalid site accepted");
        require(local::entry(0) == nullptr, "unprepared code published");
        if (argc > 1 && std::string_view(argv[1]) == "--stop-race") {
            std::atomic<bool> start{};
            std::thread stopper([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                local::stop();
            });
            start.store(true, std::memory_order_release);
            const bool prepared = local::prepare(reinterpret_cast<std::uintptr_t>(fst_cmp0) - local::siteRvas[0]);
            stopper.join();
            require(!local::active() && !local::activate(), "prepare raced past sticky stop");
            require(!local::prepare(reinterpret_cast<std::uintptr_t>(fst_cmp0) - local::siteRvas[0]), "prepare resurrected stopped phase");
            std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"stopRace\":true,\"preparedBeforeStop\":"
                      << (prepared ? "true" : "false") << "}\n";
            return 0;
        }
        if (argc > 1) {
            local::stop();
            require(!local::prepare(reinterpret_cast<std::uintptr_t>(fst_cmp0) - local::siteRvas[0]), "prepare undid early stop");
            require(!local::active() && !local::activate(), "early stop was not sticky");
            std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"earlyStop\":true}\n";
            return 0;
        }
        require(local::bodyGuards().size() == 8, "fixed body guards missing");
        std::size_t guardBytes{}; for (const auto& g : local::bodyGuards()) guardBytes += g.size;
        require(guardBytes == 247, "fixed body guard byte count");
        const std::uint8_t* targets[]{fst_cmp0, fst_cmp1};
        const std::uint8_t* resumes[]{fst_resume0, fst_resume1};
        constexpr std::uint8_t branches[][6]{{0x75,0x14},{0x0f,0x85,0x4b,0x01,0x00,0x00}};
        mh(MH_Initialize());
        require(!local::prepare(UINTPTR_MAX), "overflow module base accepted");
        require(local::prepare(reinterpret_cast<std::uintptr_t>(fst_cmp0) - local::siteRvas[0]), "near island preparation failed");
        require(!local::prepare(reinterpret_cast<std::uintptr_t>(fst_cmp0) - local::siteRvas[0]), "duplicate prepare accepted");
        for (std::size_t i = 0; i < 2; ++i) {
            require(std::memcmp(targets[i], local::cmpBytes.data(), 5) == 0, "fixture original CMP bytes differ");
            require(std::memcmp(resumes[i], branches[i], i ? 6 : 2) == 0, "fixture remaining JNE bytes differ");
            mh(MH_CreateHook(const_cast<std::uint8_t*>(targets[i]), local::entry(i), local::originalSlot(i)));
            if (!local::configureSite(i, *local::originalSlot(i), const_cast<std::uint8_t*>(resumes[i]))) {
                const auto error = GetLastError(); DWORD64 base{};
                const auto* fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(local::entry(i)), &base, nullptr);
                std::cerr << "configure site=" << i << " lastError=" << error << " staticFunction=" << fn;
                if (fn) {
                    const auto* u = reinterpret_cast<const std::uint8_t*>(base + fn->UnwindData);
                    std::cerr << " length=" << fn->EndAddress-fn->BeginAddress << " unwind=" << unsigned(u[0]) << ',' << unsigned(u[1]) << ',' << unsigned(u[2]);
                }
                std::cerr << '\n';
                throw std::runtime_error("configure/unwind registration failed");
            }
            require(local::siteReady(i), "site precheck failed");
            require(!local::configureSite(i, *local::originalSlot(i), const_cast<std::uint8_t*>(resumes[i])), "duplicate site accepted");
        }
        for (auto* target : targets) mh(MH_EnableHook(const_cast<std::uint8_t*>(target)));
        std::cerr << "CPU stage: inactive execution\n";
        execute(false);
        std::cerr << "CPU stage: original BODY unwind comparison\n";
        const auto points = unwind();
        require(local::activate() && local::active(), "complete certificate not published");
        require(!local::activate(), "certificate published twice");
        std::cerr << "CPU stage: active execution\n";
        execute(true);
        local::stop(); local::stop();
        require(!local::active() && !local::activate(), "stop was not sticky");
        execute(false);
        require(local::siteReady(0) && local::siteReady(1), "stopped forwarders lost lifetime");
        std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"unwindInstructionPoints\":" << points
                  << ",\"actualMinHookSites\":2,\"gameLoaded\":false,\"gpuExecuted\":false}\n";
        return 0;
    } catch (const std::exception& e) {
        local::stop(); std::cerr << e.what() << '\n'; return 1;
    }
}
