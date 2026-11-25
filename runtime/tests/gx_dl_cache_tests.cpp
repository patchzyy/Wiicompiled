#include "../src/hle/gx/gx_dl_cache.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

extern "C" const uint8_t kMkwGuestRegion = 0;

namespace {
thread_local int g_allocationsBeforeFailure = -1;
}

void* operator new(size_t size) {
    if (g_allocationsBeforeFailure == 0) {
        g_allocationsBeforeFailure = -1;
        throw std::bad_alloc{};
    }
    if (g_allocationsBeforeFailure > 0) --g_allocationsBeforeFailure;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Record {
    uint32_t listAddr = 0;
    uint32_t nbytes = 0;
    uint64_t layoutHash = 0;
    uint64_t contentDigest = 0;
    uint64_t writeGeneration = GxGuestWrite::kUntracked;
    std::vector<uint64_t> cpWrites;
    bool streamIsSource = false;
    std::vector<uint8_t> stream;

    size_t Bytes() const { return stream.size() + cpWrites.size() * sizeof(uint64_t); }
};

using Cache = GxDisplayListScanCache::BoundedCache<Record>;
using GxDisplayListScanCache::DisplayListBytes;
using GxDisplayListScanCache::MakeIdentityKey;
using GxDisplayListScanCache::Probe;

bool Store(Cache& cache, uint64_t key, size_t flattenedBytes, size_t cpWrites = 0) {
    Record record{};
    record.cpWrites.resize(cpWrites, key);
    record.stream.resize(flattenedBytes, static_cast<uint8_t>(key));
    const size_t bytes = record.Bytes();
    return cache.Insert(key, std::move(record), bytes);
}

void EntryBudgetAndRecency() {
    Cache cache{3, 1024};
    for (uint64_t key = 1; key <= 3; ++key) Require(Store(cache, key, 0), "initial insert failed");
    auto* hot = cache.Find(1);
    cache.Touch(1);
    Require(Store(cache, 4, 0), "entry-budget insert failed");
    Require(cache.Size() == 3 && cache.StoredBytes() == 0, "entry budget exceeded");
    Require(cache.Find(1) == hot && cache.Find(2) == nullptr && cache.Find(3) && cache.Find(4),
            "entry-budget eviction discarded the hot working set");
    cache.Find(3);
    cache.Touch(999);
    Require(Store(cache, 5, 0), "second entry-budget insert failed");
    Require(!cache.Find(3) && cache.Find(1), "unvalidated probe refreshed recency");
}

void ByteBudgetAndReplacement() {
    Cache cache{3, 12};
    Require(Store(cache, 1, 4) && Store(cache, 2, 4) && Store(cache, 3, 4), "byte-budget setup failed");
    auto* retained = cache.Find(3);
    Require(Store(cache, 1, 8), "protected replacement failed");
    Require(cache.Size() == 2 && cache.StoredBytes() == 12, "replacement byte accounting failed");
    Require(cache.Find(1) && !cache.Find(2) && cache.Find(3) == retained,
            "oldest replacement was evicted or unrelated record moved");
    Require(cache.Find(1)->stream == std::vector<uint8_t>(8, 1), "replacement payload was lost");
    Require(Store(cache, 4, 4), "byte-budget eviction failed");
    Require(cache.Size() == 2 && cache.StoredBytes() == 12 && !cache.Find(3) && cache.Find(1),
            "replacement was not promoted to most recent");
    Require(Store(cache, 1, 0), "shrinking replacement failed");
    Require(cache.Size() == 2 && cache.StoredBytes() == 4, "shrinking replacement leaked bytes");
    Require(Store(cache, 5, 0, 1), "CP-write insertion failed");
    Require(cache.Size() == 3 && cache.StoredBytes() == 12, "CP-write bytes were not counted");
    Require(Store(cache, 5, 4), "CP-write replacement failed");
    Require(cache.StoredBytes() == 8, "CP-write replacement counted old bytes twice");
}

void BytePressureSkipsEmptyRecords() {
    Cache cache{514, 8};
    for (uint64_t key = 1; key <= 512; ++key) Require(Store(cache, key, 0), "empty record setup failed");
    Require(Store(cache, 513, 4) && Store(cache, 514, 4), "byte record setup failed");
    cache.Touch(513);
    Require(Store(cache, 515, 4), "byte pressure starved behind empty records");
    Require(cache.Size() == 514 && cache.StoredBytes() == 8 && cache.Find(513) && !cache.Find(514),
            "combined pressure failed to pick the oldest byte-bearing entry");
    for (uint64_t key = 1; key <= 512; ++key) Require(cache.Find(key), "byte pressure evicted an empty record");

    const auto* promoted = cache.Find(512);
    Require(Store(cache, 512, 4) && cache.Find(512) == promoted && !cache.Find(513),
            "zero-to-positive replacement failed to join the byte order");
    Require(Store(cache, 515, 0) && Store(cache, 516, 8) && !cache.Find(512) && cache.Find(515),
            "byte pressure evicted a positive-to-zero replacement");
    Require(Store(cache, 517, 0) && Store(cache, 518, 0) && !cache.Find(1) && cache.Find(516),
            "entry pressure failed to pick the overall oldest entry");
}

void AllocationFailurePreservesVictims() {
    for (const int failAfter : {0, 1}) {
        Cache cache{2, 8};
        Require(Store(cache, 1, 4) && Store(cache, 2, 4), "allocation failure setup failed");
        auto* first = cache.Find(1);
        auto* second = cache.Find(2);
        Record record{};
        record.stream.resize(4);
        bool failed = false;
        g_allocationsBeforeFailure = failAfter;
        try {
            cache.Insert(3, std::move(record), 4);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        g_allocationsBeforeFailure = -1;
        Require(failed, "cache insertion did not hit the allocation failure");
        Require(cache.Size() == 2 && cache.StoredBytes() == 8 && cache.Find(1) == first &&
                cache.Find(2) == second && !cache.Find(3), "allocation failure changed the working set");
        Require(Store(cache, 4, 4) && !cache.Find(1) && cache.Find(2) == second,
                "allocation failure changed byte recency");
    }
}

void RejectedInsertsPreserveRecords() {
    Cache cache{2, 12};
    Require(Store(cache, 1, 8) && Store(cache, 2, 4), "oversize setup failed");
    const auto* old = cache.Find(1);
    Require(!Store(cache, 3, 13) && !Store(cache, 1, 13), "oversized record was admitted");
    Require(!cache.Insert(1, Record{}, std::numeric_limits<size_t>::max()), "overflowing size was admitted");
    Require(cache.Size() == 2 && cache.StoredBytes() == 12 && cache.Find(1) == old && cache.Find(2),
            "rejected insertion changed the working set");
    Require(Store(cache, 3, 4) && !cache.Find(1) && cache.Find(2), "rejected insertion changed recency");

    Cache disabled{0, 12};
    Require(!Store(disabled, 1, 0) && disabled.Size() == 0, "disabled entry budget accepted a record");
    Cache noBytes{1, 0};
    Require(Store(noBytes, 1, 0) && !Store(noBytes, 1, 1) && noBytes.Find(1),
            "zero byte budget mishandled an empty record");
}

void EvictionLimitAndProtection() {
    Cache cache{256, 128};
    for (uint64_t key = 1; key <= 128; ++key) Require(Store(cache, key, 1), "bounded setup failed");
    const auto* protectedRecord = cache.Find(1);
    Require(Store(cache, 1, 65), "replacement needing exactly 64 victims failed");
    Require(cache.Size() == 64 && cache.StoredBytes() == 128 && cache.Find(1) == protectedRecord,
            "protected replacement was counted as a victim");
    for (uint64_t key = 2; key <= 65; ++key) Require(!cache.Find(key), "wrong LRU victim selected");
    for (uint64_t key = 66; key <= 128; ++key) Require(cache.Find(key), "too many records evicted");

    cache.Clear();
    for (uint64_t key = 1; key <= 128; ++key) Require(Store(cache, key, 1), "bounded refill failed");
    Require(Store(cache, 129, 64), "new insertion needing exactly 64 victims failed");
    Require(cache.Size() == 65 && cache.StoredBytes() == 128 && !cache.Find(64) && cache.Find(65),
            "new insertion evicted the wrong count");
}

void BoundedRejectionsMakeProgress() {
    Cache cache{256, 128};
    for (uint64_t key = 1; key <= 128; ++key) Require(Store(cache, key, 1), "rejection setup failed");
    Require(!Store(cache, 129, 65), "new insertion exceeded 64 victims");
    Require(cache.Size() == 64 && cache.StoredBytes() == 64 && !cache.Find(64) && cache.Find(65),
            "rejected insertion did not stop at 64 victims");
    Require(Store(cache, 129, 65) && cache.Size() == 64 && cache.StoredBytes() == 128,
            "rejected insertion did not make progress for the next attempt");

    cache.Clear();
    for (uint64_t key = 1; key <= 128; ++key) Require(Store(cache, key, 1), "protected rejection setup failed");
    const auto* protectedRecord = cache.Find(1);
    Require(!Store(cache, 1, 66), "replacement exceeded 64 victims");
    Require(cache.Size() == 64 && cache.StoredBytes() == 64 && cache.Find(1) == protectedRecord &&
            protectedRecord->stream.size() == 1, "bounded rejection lost the protected replacement");
    Require(Store(cache, 1, 66) && cache.Size() == 63 && cache.StoredBytes() == 128,
            "replacement did not make progress for the next attempt");
}

void ClearAndPointerLifetime() {
    Cache cache{8192, 8192};
    Require(Store(cache, 0, 1), "pointer setup failed");
    const auto* first = cache.Find(0);
    const auto* payload = first->stream.data();
    for (uint64_t key = 1; key < 8192; ++key) Require(Store(cache, key, 1), "growth insert failed");
    cache.Touch(0);
    for (uint64_t key = 8192; key < 16384; ++key) {
        Require(Store(cache, key, 1), "churn insert failed");
        Require(cache.Find(0) == first && first->stream.data() == payload,
                "rehash or unrelated eviction invalidated a retained stream");
        cache.Touch(0);
    }
    Require(cache.Size() == 8192 && cache.StoredBytes() == 8192, "churn exceeded a budget");
    cache.Clear();
    cache.Clear();
    Require(cache.Size() == 0 && cache.StoredBytes() == 0 && !cache.Find(0), "clear left stale accounting");
    Require(Store(cache, 0, 0), "insertion after clear failed");
}

void LiveBytesStillInvalidateRetainedRecords() {
    constexpr uint32_t address = 0x80010000;
    constexpr uint32_t alias = 0xC0010000;
    constexpr uint64_t layout = 0x12345678;
    std::array<uint8_t, 8> bytes{0x08, 0x50, 0, 0, 0, 1, 0, 0};
    const auto key = MakeIdentityKey(address, bytes.size(), layout);
    Cache cache{2, 32};
    Record record{};
    record.listAddr = CanonicalizeGxMainRamAddress(address);
    record.nbytes = bytes.size();
    record.layoutHash = layout;
    record.contentDigest = XXH3_64bits(bytes.data(), bytes.size());
    record.writeGeneration = GxGuestWrite::GenerationForRange(address, bytes.size());
    record.stream = {0};
    Require(cache.Insert(key, std::move(record), 1), "validation setup failed");
    auto* retained = cache.Find(key);
    const auto probe = [&](uint32_t addr, uint32_t count, uint64_t currentLayout) {
        const auto generation = GxGuestWrite::GenerationForRange(addr, count);
        DisplayListBytes current{bytes.data(), count};
        return Probe(cache, current, addr, count, currentLayout, generation);
    };
    Require(probe(alias, bytes.size(), layout) == retained, "unchanged alias was rejected");
    Require(!probe(address, bytes.size() - 1, layout) && !probe(address, bytes.size(), layout + 1) &&
            !probe(address + 32, bytes.size(), layout), "identity mismatch was reused");
    GxGuestWrite::NotifyWrite(alias, bytes.size());
    Require(Store(cache, 1, 16) && probe(address, bytes.size(), layout) == retained,
            "unchanged bytes after a write were rejected");
    const auto validatedGeneration = GxGuestWrite::GenerationForRange(address, bytes.size());
    Require(retained->writeGeneration == validatedGeneration, "validated hit did not refresh the generation");
    Require(Store(cache, 2, 16) && cache.Find(key) == retained && !cache.Find(1),
            "unrelated eviction discarded the validated record");
    bytes[5] ^= 0x80;
    GxGuestWrite::NotifyWrite(alias, bytes.size());
    Require(!probe(address, bytes.size(), layout) && retained->writeGeneration == validatedGeneration,
            "changed live commands reused a retained record");

    constexpr uint32_t untrackedAddress = 0xF0000000;
    Record untracked = *retained;
    untracked.listAddr = untrackedAddress;
    untracked.writeGeneration = GxGuestWrite::kUntracked;
    Require(cache.Insert(MakeIdentityKey(untrackedAddress, bytes.size(), layout), std::move(untracked), 1),
            "untracked validation setup failed");
    Require(!probe(untrackedAddress, bytes.size(), layout), "untracked writes bypassed digest validation");
}

void RawProbeAndScanUseTheSameCapture() {
    constexpr uint32_t address = 0x80020000;
    constexpr uint64_t layout = 0x12345678;
    constexpr std::array<uint8_t, 4> original{0x90, 0, 1, 7};
    constexpr uint32_t size = original.size();
    const auto key = MakeIdentityKey(address, size, layout);
    const auto generation = GxGuestWrite::GenerationForRange(address, size);
    auto guest = original;
    Cache cache{2, 32};
    {
        DisplayListBytes bytes{guest.data(), size};
        Require(!Probe(cache, bytes, address, size, layout, generation), "empty cache hit");
        const auto* scanned = bytes.Data();
        guest.back() = 42;
        Record record{};
        record.listAddr = CanonicalizeGxMainRamAddress(address);
        record.nbytes = size;
        record.layoutHash = layout;
        record.writeGeneration = generation;
        record.contentDigest = bytes.Digest();
        Require(scanned[3] == 7 && record.contentDigest == XXH3_64bits(original.data(), size),
                "miss digest did not describe the captured bytes");
        record.streamIsSource = true;
        record.stream.assign(scanned, scanned + size);
        Require(cache.Insert(key, std::move(record), size), "raw validation setup failed");
    }
    {
        DisplayListBytes bytes{guest.data(), size};
        Require(!Probe(cache, bytes, address, size, layout, generation),
                "unchanged generation bypassed raw-byte validation");
        const auto* rescanned = bytes.Data();
        guest.back() = 7;
        Require(rescanned[3] == 42 && bytes.Digest() != cache.Find(key)->contentDigest,
                "stale-hit scan did not retain the probed bytes");
    }
    {
        DisplayListBytes bytes{guest.data(), size};
        const auto* hit = Probe(cache, bytes, address, size, layout, generation);
        Require(hit != nullptr, "unchanged raw bytes missed");
        guest.back() = 42;
        Require(hit->stream[3] == 7, "raw hit submitted bytes different from the probe");
    }
}

struct LegacyCache {
    std::unordered_map<uint64_t, Record> entries;
    size_t storedBytes = 0;
    size_t maxEntries;
    size_t maxBytes;

    Record* Find(uint64_t key) {
        const auto it = entries.find(key);
        return it == entries.end() ? nullptr : &it->second;
    }
    void Touch(uint64_t) {}
    size_t Size() const { return entries.size(); }
    bool Insert(uint64_t key, Record&& value, size_t bytes) {
        const auto it = entries.find(key);
        const bool replacing = it != entries.end();
        size_t replacedBytes = replacing ? it->second.Bytes() : 0;
        if ((!replacing && entries.size() >= maxEntries) || storedBytes - replacedBytes + bytes > maxBytes) {
            entries.clear();
            storedBytes = 0;
            replacedBytes = 0;
        }
        entries.insert_or_assign(key, std::move(value));
        storedBytes = storedBytes - replacedBytes + bytes;
        return true;
    }
};

template <typename Storage>
void Benchmark(Storage& cache, std::string_view policy, std::string_view scenario,
               size_t hotCount, size_t coldCount, size_t bytesPerEntry) {
    using Clock = std::chrono::steady_clock;
    uint64_t hits = 0, misses = 0, evictions = 0, skips = 0;
    std::vector<double> batches;
    const auto access = [&](uint64_t key) {
        if (cache.Find(key)) {
            ++hits;
            cache.Touch(key);
        } else {
            ++misses;
            Record record{};
            record.stream.resize(bytesPerEntry);
            const auto before = cache.Size();
            const bool inserted = cache.Insert(key, std::move(record), bytesPerEntry);
            skips += !inserted;
            evictions += before + (inserted ? 1u : 0u) - cache.Size();
        }
    };
    for (uint64_t key = 0; key < hotCount; ++key) access(key);
    for (uint64_t round = 0; round < 256; ++round) {
        const auto begin = Clock::now();
        for (uint64_t key = 0; key < hotCount; ++key) access(key);
        for (uint64_t key = 0; key < coldCount; ++key) access(hotCount + round * coldCount + key);
        batches.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
    }
    std::sort(batches.begin(), batches.end());
    const auto percentile = [&](size_t percent) { return batches[(batches.size() - 1) * percent / 100]; };
    std::cout << policy << ',' << scenario << ',' << hits << ',' << misses << ',' << evictions << ',' << skips
              << ',' << percentile(50) << ',' << percentile(95) << ',' << percentile(99) << '\n';
}

void Benchmarks() {
    constexpr size_t maxBytes = 8u * 1024u * 1024u;
    std::cout << "policy,scenario,hits,misses,evictions,skips,batch_p50_us,batch_p95_us,batch_p99_us\n";
    for (const bool bytePressure : {false, true}) {
        const size_t hotCount = bytePressure ? 256 : 4096;
        const size_t coldCount = bytePressure ? 64 : 1024;
        const size_t bytesPerEntry = bytePressure ? 16384 : 64;
        const auto scenario = bytePressure ? "bytes" : "entries";
        Cache cache{8192, maxBytes};
        Benchmark(cache, "incremental", scenario, hotCount, coldCount, bytesPerEntry);
        LegacyCache legacy{{}, 0, 8192, maxBytes};
        Benchmark(legacy, "whole-cache", scenario, hotCount, coldCount, bytesPerEntry);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
            Benchmarks();
        } else {
            EntryBudgetAndRecency();
            ByteBudgetAndReplacement();
            BytePressureSkipsEmptyRecords();
            AllocationFailurePreservesVictims();
            RejectedInsertsPreserveRecords();
            EvictionLimitAndProtection();
            BoundedRejectionsMakeProgress();
            ClearAndPointerLifetime();
            LiveBytesStillInvalidateRetainedRecords();
            RawProbeAndScanUseTheSameCapture();
            std::cout << "display-list scan cache tests passed\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
