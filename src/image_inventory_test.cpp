#include "image_inventory.hpp"
#include <atomic>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace xess_vk::image_inventory;
namespace {
unsigned checks{};
void require(bool okay, const char* message) { ++checks; if (!okay) throw std::runtime_error(message); }
Description description(std::uint32_t width) { Description d; d.width = width; d.height = 73; d.format = 83; return d; }
}
int main() {
    try {
        Inventory<8> small;
        require(small.lookup(0).status == Status::NotObserved, "null image matched");
        small.created(3, description(127));
        const auto original = small.lookup(3);
        require(original.status == Status::LiveHandleObserved && original.record.description.width == 127, "created metadata missing");
        small.destroying(3);
        require(small.lookup(3).status == Status::NotObserved, "destroyed image retained");
        small.created(3, description(256));
        require(small.lookup(3).record.generation > original.record.generation && small.lookup(3).record.description.width == 256, "reuse kept old generation");
        small.created(3, description(512));
        require(small.lookup(3).status == Status::DuplicateCreate, "duplicate did not fail closed");
        small.destroying(3); small.created(4, description(512));
        require(small.lookup(4).status == Status::DuplicateCreate, "fault recovered implicitly");
        Inventory<8> capped;
        for (std::uint64_t n = 1; n <= 4; ++n) capped.created(n, description(7));
        require(capped.lookup(4).active == 4, "capacity count wrong");
        capped.created(5, description(7));
        require(capped.lookup(1).status == Status::CapacityExceeded, "overflow left partial verification active");
        capped.stop(); require(capped.lookup(1).status == Status::Stopped, "stop ineffective");

        // Compare a long create/destroy/reuse sequence against an independent
        // container. The small table forces wrapped collision/deletion chains.
        Inventory<64> inventory;
        std::unordered_map<std::uint64_t, std::uint32_t> expected;
        std::mt19937_64 random(0x78213);
        for (unsigned n = 0; n < 100000; ++n) {
            const auto id = random() % 100 + 1;
            if (expected.contains(id)) { inventory.destroying(id); expected.erase(id); }
            else if (expected.size() < 30) { const auto width = static_cast<std::uint32_t>(random()); inventory.created(id, description(width)); expected[id] = width; }
            for (std::uint64_t query = 1; query <= 100; ++query) {
                const auto actual = inventory.lookup(query);
                const auto found = expected.find(query);
                require(actual.active == expected.size(), "active count diverged");
                require(actual.status == (found == expected.end() ? Status::NotObserved : Status::LiveHandleObserved), "collision deletion corrupted presence");
                if (found != expected.end()) require(actual.record.description.width == found->second, "collision deletion changed record");
            }
        }
        // Each owner uses a separate numeric image but all operations contend
        // on the same inventory. Lookup snapshots must stay internally coherent.
        auto concurrent = std::make_unique<Inventory<1024>>();
        std::atomic<unsigned> failures{};
        std::vector<std::jthread> workers;
        for (unsigned t = 0; t < 8; ++t) workers.emplace_back([&, t] {
            for (unsigned n = 1; n <= 10000; ++n) {
                const std::uint64_t id = (t + 1) * 0x100000000ull;
                concurrent->created(id, description(n));
                const auto snapshot = concurrent->lookup(id);
                if (snapshot.status != Status::LiveHandleObserved || snapshot.record.image != id || snapshot.record.description.width != n) ++failures;
                concurrent->destroying(id);
                if (concurrent->lookup(id).status != Status::NotObserved) ++failures;
            }
        });
        for (auto& worker : workers) worker.join();
        require(failures == 0 && concurrent->lookup(0).active == 0, "concurrent lifecycle mismatch");
        std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"randomLifecycleOperations\":100000,\"concurrentLifecycleOperations\":80000,\"boundedBytes\":" << sizeof(Inventory<>) << ",\"maximumActive\":" << Inventory<>::maximumActive << ",\"gameTested\":false}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
