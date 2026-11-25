#pragma once

#include "isa/big_endian.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeU8 {

inline bool Extract(const std::vector<uint8_t>& data, std::string_view targetName,
                    std::vector<uint8_t>& outData) {
    constexpr uint64_t nodeSize = 12;
    if (data.size() < 0x20 || BigEndian::Read32(data.data()) != 0x55AA382Du) return false;
    const uint64_t rootOffset = BigEndian::Read32(data.data() + 4);
    const uint64_t headerSize = BigEndian::Read32(data.data() + 8);
    if (rootOffset > data.size() || nodeSize > data.size() - rootOffset ||
        headerSize > data.size() - rootOffset) return false;
    const uint8_t* root = data.data() + rootOffset;
    const uint32_t nodeCount = BigEndian::Read32(root + 8);
    const uint64_t nodeTableSize = static_cast<uint64_t>(nodeCount) * nodeSize;
    const uint64_t metadataEnd = rootOffset + headerSize;
    if ((BigEndian::Read32(root) >> 24) != 1 || BigEndian::Read32(root + 4) != 0 ||
        nodeCount == 0 || nodeTableSize >= headerSize) return false;
    const uint64_t stringTableOffset = rootOffset + nodeTableSize;

    struct DirFrame {
        std::string path;
        uint32_t index;
        uint32_t endIndex;
    };
    std::vector<DirFrame> stack;
    struct Payload { uint64_t offset; uint64_t size; };
    std::optional<Payload> selected;

    for (uint32_t index = 0; index < nodeCount; ++index) {
        while (!stack.empty() && index >= stack.back().endIndex) stack.pop_back();
        if (index != 0 && stack.empty()) return false;
        const uint8_t* node = root + static_cast<uint64_t>(index) * nodeSize;
        const uint32_t typeAndName = BigEndian::Read32(node);
        const uint32_t type = typeAndName >> 24;
        const uint64_t nameOffset = typeAndName & 0x00FFFFFFu;
        if (type > 1 || nameOffset >= metadataEnd - stringTableOffset) return false;
        const char* name = reinterpret_cast<const char*>(data.data() + stringTableOffset + nameOffset);
        const auto* end = static_cast<const char*>(std::memchr(name, '\0',
            static_cast<size_t>(metadataEnd - stringTableOffset - nameOffset)));
        if (!end) return false;
        const std::string_view nameText(name, static_cast<size_t>(end - name));
        const std::string fullPath = index == 0 ? std::string{} : stack.back().path + std::string(nameText);
        const uint32_t offset = BigEndian::Read32(node + 4);
        const uint32_t size = BigEndian::Read32(node + 8);
        if (type == 1) {
            if (size <= index || (index != 0 &&
                (size > stack.back().endIndex || offset != stack.back().index))) return false;
            stack.push_back({index == 0 ? std::string{} : fullPath + "/", index, size});
        } else {
            const uint64_t fileOffset = offset;
            if (fileOffset > data.size() || size > data.size() - fileOffset) return false;
            if (!selected && (fullPath == targetName || nameText == targetName)) selected = Payload{fileOffset, size};
        }
    }
    if (!selected) return false;
    outData.assign(data.begin() + selected->offset, data.begin() + selected->offset + selected->size);
    return true;
}

} // namespace RuntimeU8
