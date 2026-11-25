#pragma once

#include "gx_internal.h"

#include <xxhash.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace GxDisplayListScanCache {

template <typename Value>
class BoundedCache {
    struct Entry {
        uint64_t key;
        Value value;
        size_t bytes;
        Entry* newerBytes = nullptr;
        Entry* olderBytes = nullptr;
    };
    using Entries = std::list<Entry>;
    using Iterator = typename Entries::iterator;

public:
    static constexpr size_t kMaxEvictionsPerStore = 64;

    BoundedCache(size_t maxEntries, size_t maxBytes)
        : m_maxEntries(maxEntries), m_maxBytes(maxBytes) {}
    BoundedCache(const BoundedCache&) = delete;
    BoundedCache& operator=(const BoundedCache&) = delete;

    Value* Find(uint64_t key) {
        const auto it = m_index.find(key);
        return it == m_index.end() ? nullptr : &it->second->value;
    }

    // Only validated hits refresh recency.
    void Touch(uint64_t key) {
        const auto it = m_index.find(key);
        if (it != m_index.end() && it->second != m_entries.begin()) {
            const auto entry = it->second;
            UnlinkByteEntry(*entry);
            LinkByteEntry(*entry);
            m_entries.splice(m_entries.begin(), m_entries, entry);
        }
    }

    bool Insert(uint64_t key, Value&& value, size_t bytes) {
        static_assert(std::is_nothrow_move_assignable_v<Value>);
        if (m_maxEntries == 0 || bytes > m_maxBytes) {
            return false;
        }
        const auto found = m_index.find(key);
        Entry* const old = found == m_index.end() ? nullptr : &*found->second;
        size_t retainedBytes = m_storedBytes - (old ? old->bytes : 0u);
        const size_t availableBytes = m_maxBytes - bytes;

        std::array<Entry*, kMaxEvictionsPerStore> victims{};
        size_t victimCount = 0;
        for (auto* victim = m_oldestByteEntry;
             victim && retainedBytes > availableBytes && victimCount < victims.size();
             victim = victim->newerBytes) {
            if (victim == old) {
                continue;
            }
            victims[victimCount++] = victim;
            retainedBytes -= victim->bytes;
        }
        // Retire at most one batch so later attempts can make progress too.
        if (retainedBytes > availableBytes) {
            for (size_t i = 0; i < victimCount; ++i) {
                Erase(*victims[i]);
            }
            return false;
        }
        // Any byte victim also frees the one slot a new entry can need.
        if (victimCount == 0 && !old && m_entries.size() == m_maxEntries) {
            victims[victimCount++] = &m_entries.back();
        }

        if (!old) {
            m_entries.push_front({key, std::move(value), bytes});
            try {
                m_index.emplace(key, m_entries.begin());
            } catch (...) {
                m_entries.pop_front();
                throw;
            }
        } else {
            UnlinkByteEntry(*old);
            old->value = std::move(value);
            m_storedBytes -= old->bytes;
            old->bytes = bytes;
            m_entries.splice(m_entries.begin(), m_entries, found->second);
        }
        LinkByteEntry(m_entries.front());
        for (size_t i = 0; i < victimCount; ++i) {
            Erase(*victims[i]);
        }
        m_storedBytes += bytes;
        return true;
    }

    void Clear() {
        m_index.clear();
        m_entries.clear();
        m_newestByteEntry = nullptr;
        m_oldestByteEntry = nullptr;
        m_storedBytes = 0;
    }

    size_t Size() const { return m_entries.size(); }
    size_t StoredBytes() const { return m_storedBytes; }

private:
    void LinkByteEntry(Entry& entry) {
        if (entry.bytes == 0) return;
        entry.newerBytes = nullptr;
        entry.olderBytes = m_newestByteEntry;
        if (m_newestByteEntry) {
            m_newestByteEntry->newerBytes = &entry;
        } else {
            m_oldestByteEntry = &entry;
        }
        m_newestByteEntry = &entry;
    }

    void UnlinkByteEntry(Entry& entry) {
        if (entry.bytes == 0) return;
        if (entry.newerBytes) {
            entry.newerBytes->olderBytes = entry.olderBytes;
        } else {
            m_newestByteEntry = entry.olderBytes;
        }
        if (entry.olderBytes) {
            entry.olderBytes->newerBytes = entry.newerBytes;
        } else {
            m_oldestByteEntry = entry.newerBytes;
        }
        entry.newerBytes = nullptr;
        entry.olderBytes = nullptr;
    }

    void Erase(Entry& entry) {
        UnlinkByteEntry(entry);
        m_storedBytes -= entry.bytes;
        const auto found = m_index.find(entry.key);
        const auto position = found->second;
        m_index.erase(found);
        m_entries.erase(position);
    }

    size_t m_maxEntries;
    size_t m_maxBytes;
    Entries m_entries;
    std::unordered_map<uint64_t, Iterator> m_index;
    Entry* m_newestByteEntry = nullptr;
    Entry* m_oldestByteEntry = nullptr;
    size_t m_storedBytes = 0;
};

inline constexpr uint32_t kMaxSourceBytes = 64u * 1024u;

class DisplayListBytes {
public:
    DisplayListBytes(const uint8_t* source, uint32_t size) : m_data(source), m_size(size) {}

    bool CanCache() const { return m_size <= kMaxSourceBytes; }

    const uint8_t* Data() {
        if (!m_captured && CanCache()) {
            // Each thread consumes its list synchronously before reusing the scratch.
            static thread_local std::array<uint8_t, kMaxSourceBytes> scratch;
            std::memcpy(scratch.data(), m_data, m_size);
            m_data = scratch.data();
            m_captured = true;
        }
        return m_data;
    }

    uint64_t SourceDigest() const { return XXH3_64bits(m_data, m_size); }

    uint64_t Digest() {
        if (!m_digest) {
            m_digest = XXH3_64bits(Data(), m_size);
        }
        return *m_digest;
    }

private:
    const uint8_t* m_data;
    uint32_t m_size;
    bool m_captured = false;
    std::optional<uint64_t> m_digest;
};

template <typename Record>
const Record* Probe(BoundedCache<Record>& cache, DisplayListBytes& list, uint32_t listAddr,
                    uint32_t nbytes, uint64_t layoutHash, uint64_t writeGeneration) {
    const uint64_t key = MakeIdentityKey(listAddr, nbytes, layoutHash);
    auto* record = cache.Find(key);
    if (!record) return nullptr;

    if (record->listAddr != CanonicalizeGxMainRamAddress(listAddr) || record->nbytes != nbytes ||
        record->layoutHash != layoutHash) {
        return nullptr;
    }
    if (record->streamIsSource) {
        // Submit the owned copy after comparison, even if the guest rewrites the list again.
        if (list.SourceDigest() != record->contentDigest) return nullptr;
    } else if (!GxGuestWrite::CanSkipDigest(record->writeGeneration, writeGeneration) &&
               list.Digest() != record->contentDigest) {
        return nullptr;
    }
    record->writeGeneration = writeGeneration;
    cache.Touch(key);
    return record;
}

} // namespace GxDisplayListScanCache
