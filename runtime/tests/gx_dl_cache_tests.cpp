#include "../src/hle/gx/gx_dl_cache.h"
#include "../src/hle/gx/gx_internal.h"

#include <xxhash.h>

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

extern "C" const uint8_t kMkwGuestRegion = 0;

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
    std::vector<uint8_t> flattened;

    size_t Bytes() const { return flattened.size() + cpWrites.size() * sizeof(uint64_t); }
};

using Cache = GxDisplayListScanCache::BoundedCache<Record>;

bool Store(Cache& cache, uint64_t key, size_t flattenedBytes, size_t cpWrites = 0) {
    Record record{};
    record.cpWrites.resize(cpWrites, key);
    record.flattened.resize(flattenedBytes, static_cast<uint8_t>(key));
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
    Require(cache.Find(1)->flattened == std::vector<uint8_t>(8, 1), "replacement payload was lost");
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
            protectedRecord->flattened.size() == 1, "bounded rejection lost the protected replacement");
    Require(Store(cache, 1, 66) && cache.Size() == 63 && cache.StoredBytes() == 128,
            "replacement did not make progress for the next attempt");

    cache.Clear();
    for (uint64_t key = 1; key <= 64; ++key) Require(Store(cache, key, 0), "empty record setup failed");
    Require(Store(cache, 65, 128), "large record setup failed");
    Require(!Store(cache, 66, 1) && cache.Size() == 1 && cache.StoredBytes() == 128,
            "zero-byte victims bypassed the eviction work limit");
    Require(Store(cache, 66, 1) && cache.Size() == 1 && cache.StoredBytes() == 1,
            "zero-byte victims caused repeated insertion starvation");
}

void ClearAndPointerLifetime() {
    Cache cache{8192, 8192};
    Require(Store(cache, 0, 1), "pointer setup failed");
    const auto* first = cache.Find(0);
    const auto* payload = first->flattened.data();
    for (uint64_t key = 1; key < 8192; ++key) Require(Store(cache, key, 1), "growth insert failed");
    cache.Touch(0);
    for (uint64_t key = 8192; key < 16384; ++key) {
        Require(Store(cache, key, 1), "churn insert failed");
        Require(cache.Find(0) == first && first->flattened.data() == payload,
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
    constexpr uint64_t layout = 0x12345678;
    std::array<uint8_t, 8> bytes{0x08, 0x50, 0, 0, 0, 1, 0, 0};
    const auto digest = [&] { return static_cast<uint64_t>(XXH3_64bits(bytes.data(), bytes.size())); };
    const auto key = GxDisplayListScanCache::MakeIdentityKey(address, bytes.size(), layout);
    Require(key == GxDisplayListScanCache::MakeIdentityKey(0xC0010000, bytes.size(), layout),
            "cached and uncached aliases use different keys");
    Cache cache{2, 32};
    Record record{};
    record.listAddr = CanonicalizeGxMainRamAddress(address);
    record.nbytes = bytes.size();
    record.layoutHash = layout;
    record.contentDigest = digest();
    record.writeGeneration = GxGuestWrite::GenerationForRange(address, bytes.size());
    Require(cache.Insert(key, std::move(record), 0), "validation setup failed");
    auto* retained = cache.Find(key);
    const auto reusable = [&](uint32_t addr, uint32_t count, uint64_t currentLayout, uint64_t currentDigest) {
        return GxDisplayListScanCache::CanReuse(retained->listAddr, retained->nbytes, retained->layoutHash,
                                               retained->contentDigest, addr, count, currentLayout,
                                               currentDigest);
    };
    Require(reusable(0xC0010000, bytes.size(), layout, digest()), "unchanged alias was rejected");
    Require(!reusable(address, bytes.size() - 1, layout, digest()) &&
            !reusable(address, bytes.size(), layout + 1, digest()) &&
            !reusable(address + 32, bytes.size(), layout, digest()), "identity mismatch was reused");
    GxGuestWrite::NotifyWrite(0xC0010000, bytes.size());
    const auto generation = GxGuestWrite::GenerationForRange(address, bytes.size());
    Require(!GxGuestWrite::CanSkipDigest(retained->writeGeneration, generation), "write failed to invalidate digest");
    Require(reusable(address, bytes.size(), layout, digest()), "unchanged bytes after a write were rejected");
    bytes[5] ^= 0x80;
    Require(!reusable(address, bytes.size(), layout, digest()), "changed command bytes reused a retained record");
    Require(!GxGuestWrite::CanSkipDigest(GxGuestWrite::kUntracked, GxGuestWrite::kUntracked),
            "untracked writes bypassed digest validation");
    Require(Store(cache, 1, 16), "validation churn setup failed");
    cache.Touch(key);
    Require(Store(cache, 2, 16) && cache.Find(key) == retained && !cache.Find(1),
            "unrelated eviction discarded the retained record");
    Require(!reusable(address, bytes.size(), layout, digest()), "churn made a stale digest reusable");
    Record replacement = *retained;
    replacement.contentDigest = digest();
    replacement.writeGeneration = generation;
    Require(cache.Insert(key, std::move(replacement), 0) && cache.StoredBytes() == 16,
            "invalidated replacement changed unrelated byte accounting");
    Require(reusable(address, bytes.size(), layout, digest()), "replacement did not refresh the digest");
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
            record.flattened.resize(bytesPerEntry);
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
            RejectedInsertsPreserveRecords();
            EvictionLimitAndProtection();
            BoundedRejectionsMakeProgress();
            ClearAndPointerLifetime();
            LiveBytesStillInvalidateRetainedRecords();
            std::cout << "display-list scan cache tests passed\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
