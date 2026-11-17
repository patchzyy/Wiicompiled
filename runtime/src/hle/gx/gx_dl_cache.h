#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
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
        if (it != m_index.end()) {
            m_entries.splice(m_entries.begin(), m_entries, it->second);
        }
    }

    bool Insert(uint64_t key, Value&& value, size_t bytes) {
        static_assert(std::is_nothrow_move_assignable_v<Value>);
        if (m_maxEntries == 0 || bytes > m_maxBytes) {
            return false;
        }
        const auto found = m_index.find(key);
        const auto old = found == m_index.end() ? m_entries.end() : found->second;
        size_t retainedEntries = m_entries.size() - (old != m_entries.end() ? 1u : 0u);
        size_t retainedBytes = m_storedBytes - (old != m_entries.end() ? old->bytes : 0u);
        const auto fits = [&] {
            return retainedEntries < m_maxEntries && retainedBytes <= m_maxBytes - bytes;
        };

        std::array<Iterator, kMaxEvictionsPerStore> victims{};
        size_t victimCount = 0;
        auto next = m_entries.end();
        while (!fits() && next != m_entries.begin() && victimCount < victims.size()) {
            const auto victim = --next;
            if (victim == old) {
                continue;
            }
            victims[victimCount++] = victim;
            retainedBytes -= victim->bytes;
            --retainedEntries;
        }
        // Retire at most one batch so later attempts can make progress too.
        if (!fits()) {
            for (size_t i = 0; i < victimCount; ++i) {
                Erase(victims[i]);
            }
            return false;
        }

        if (old == m_entries.end()) {
            m_entries.push_front({key, std::move(value), bytes});
            try {
                m_index.emplace(key, m_entries.begin());
            } catch (...) {
                m_entries.pop_front();
                throw;
            }
        } else {
            old->value = std::move(value);
            m_storedBytes -= old->bytes;
            old->bytes = bytes;
            m_entries.splice(m_entries.begin(), m_entries, old);
        }
        for (size_t i = 0; i < victimCount; ++i) {
            Erase(victims[i]);
        }
        m_storedBytes += bytes;
        return true;
    }

    void Clear() {
        m_index.clear();
        m_entries.clear();
        m_storedBytes = 0;
    }

    size_t Size() const { return m_entries.size(); }
    size_t StoredBytes() const { return m_storedBytes; }

private:
    void Erase(Iterator entry) {
        m_storedBytes -= entry->bytes;
        m_index.erase(entry->key);
        m_entries.erase(entry);
    }

    size_t m_maxEntries;
    size_t m_maxBytes;
    Entries m_entries;
    std::unordered_map<uint64_t, Iterator> m_index;
    size_t m_storedBytes = 0;
};

} // namespace GxDisplayListScanCache
