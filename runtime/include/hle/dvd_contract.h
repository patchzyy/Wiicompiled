#pragma once

#include "isa/big_endian.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace DvdFstContract {

struct RegisteredFile {
    std::filesystem::path hostPath;
    std::string dvdPath;
    uint32_t size = 0;
    uint32_t discOffsetWords = 0;
};

struct IndexedEntry {
    std::filesystem::path hostPath;
    std::string dvdPath;
    uint32_t size = 0;
    uint32_t discOffsetWords = 0;
    uint32_t parentIndex = 0;
    uint32_t subtreeEnd = 0;
    bool isDirectory = false;
};

struct Image {
    std::vector<IndexedEntry> entries;
    std::map<std::string, int32_t> pathToEntry;
    std::vector<uint8_t> bytes;
};

struct GuestPlacement {
    uint32_t address = 0;
    uint32_t reservedArenaHi = 0;
};

inline std::string CanonicalizePath(const std::string& input) {
    std::string path = input;
    std::replace(path.begin(), path.end(), '\\', '/');

    std::vector<std::string> components;
    size_t cursor = 0;
    while (cursor < path.size()) {
        while (cursor < path.size() && path[cursor] == '/') {
            ++cursor;
        }
        const size_t start = cursor;
        while (cursor < path.size() && path[cursor] != '/') {
            ++cursor;
        }
        if (start == cursor) {
            continue;
        }

        std::string component = path.substr(start, cursor - start);
        if (component == ".") {
            continue;
        }
        if (component == "..") {
            if (!components.empty()) {
                components.pop_back();
            }
            continue;
        }
        components.push_back(std::move(component));
    }

    std::string canonical = "/";
    for (size_t i = 0; i < components.size(); ++i) {
        if (i != 0) {
            canonical.push_back('/');
        }
        canonical += components[i];
    }
    return canonical;
}

inline std::string NormalizeLookupPath(const std::string& input) {
    std::string path = CanonicalizePath(input);
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return path;
}

namespace Detail {

struct TreeNode {
    std::string name;
    std::map<std::string, std::unique_ptr<TreeNode>> children;
    std::optional<RegisteredFile> file;
};

inline std::vector<std::string> Components(const std::string& canonicalPath) {
    std::vector<std::string> result;
    size_t cursor = canonicalPath == "/" ? canonicalPath.size() : 1;
    while (cursor < canonicalPath.size()) {
        const size_t slash = canonicalPath.find('/', cursor);
        const size_t end = slash == std::string::npos ? canonicalPath.size() : slash;
        result.push_back(canonicalPath.substr(cursor, end - cursor));
        cursor = end + 1;
    }
    return result;
}

inline std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline uint32_t EmitTree(const TreeNode& directory,
                         uint32_t directoryIndex,
                         const std::string& directoryPath,
                         Image& image,
                         std::vector<std::string>& names) {
    for (const auto& [lookupName, childPointer] : directory.children) {
        (void)lookupName;
        const TreeNode& child = *childPointer;
        const std::string childPath = directoryPath == "/"
            ? "/" + child.name
            : directoryPath + "/" + child.name;
        const uint32_t index = static_cast<uint32_t>(image.entries.size());

        if (!child.children.empty()) {
            if (child.file.has_value()) {
                throw std::runtime_error("DVD FST path is both a file and a directory: " + childPath);
            }
            image.entries.push_back({{}, childPath, 0, 0, directoryIndex, 0, true});
            names.push_back(child.name);
            image.pathToEntry.emplace(NormalizeLookupPath(childPath), static_cast<int32_t>(index));
            image.entries[index].subtreeEnd = EmitTree(child, index, childPath, image, names);
            continue;
        }

        if (!child.file.has_value()) {
            throw std::runtime_error("DVD FST contains an empty implicit node: " + childPath);
        }
        const RegisteredFile& file = *child.file;
        image.entries.push_back({file.hostPath, childPath, file.size, file.discOffsetWords,
                                 directoryIndex, index + 1, false});
        names.push_back(child.name);
        image.pathToEntry.emplace(NormalizeLookupPath(childPath), static_cast<int32_t>(index));
    }
    return static_cast<uint32_t>(image.entries.size());
}

} // namespace Detail

inline Image BuildImage(const std::vector<RegisteredFile>& registrations) {
    // Overlay scanning deliberately registers later mappings last. Collapse those
    // mappings before assigning FST indices so one guest path has one stable entry.
    std::map<std::string, RegisteredFile> filesByPath;
    for (RegisteredFile file : registrations) {
        file.dvdPath = CanonicalizePath(file.dvdPath);
        if (file.dvdPath == "/") {
            throw std::runtime_error("DVD FST cannot register the root as a file");
        }
        filesByPath[NormalizeLookupPath(file.dvdPath)] = std::move(file);
    }

    Detail::TreeNode root;
    for (const auto& [lookupPath, file] : filesByPath) {
        (void)lookupPath;
        Detail::TreeNode* node = &root;
        const std::vector<std::string> components = Detail::Components(file.dvdPath);
        for (const std::string& component : components) {
            const std::string key = Detail::Lowercase(component);
            auto& child = node->children[key];
            if (!child) {
                child = std::make_unique<Detail::TreeNode>();
                child->name = component;
            }
            node = child.get();
        }
        node->name = components.back();
        node->file = file;
    }

    Image image;
    image.entries.push_back({{}, "/", 0, 0, 0, 0, true});
    image.pathToEntry.emplace("/", 0);
    std::vector<std::string> names(1);
    image.entries[0].subtreeEnd = Detail::EmitTree(root, 0, "/", image, names);

    if (image.entries.size() > std::numeric_limits<uint32_t>::max() / 12u) {
        throw std::runtime_error("DVD FST contains too many entries");
    }

    const size_t entriesSize = image.entries.size() * 12u;
    std::vector<uint8_t> stringTable(1, 0);
    std::vector<uint32_t> nameOffsets(image.entries.size(), 0);
    for (size_t i = 1; i < names.size(); ++i) {
        if (stringTable.size() > 0x00FFFFFFu) {
            throw std::runtime_error("DVD FST name table exceeds the Wii 24-bit offset limit");
        }
        nameOffsets[i] = static_cast<uint32_t>(stringTable.size());
        stringTable.insert(stringTable.end(), names[i].begin(), names[i].end());
        stringTable.push_back(0);
    }

    image.bytes.assign(entriesSize + stringTable.size(), 0);
    for (size_t i = 0; i < image.entries.size(); ++i) {
        const IndexedEntry& entry = image.entries[i];
        const uint32_t typeAndName = (entry.isDirectory ? 0x01000000u : 0u) | nameOffsets[i];
        const uint32_t word1 = entry.isDirectory ? entry.parentIndex : entry.discOffsetWords;
        const uint32_t word2 = entry.isDirectory ? entry.subtreeEnd : entry.size;
        BigEndian::Write32(image.bytes.data(), i * 12u + 0u, typeAndName);
        BigEndian::Write32(image.bytes.data(), i * 12u + 4u, word1);
        BigEndian::Write32(image.bytes.data(), i * 12u + 8u, word2);
    }
    std::copy(stringTable.begin(), stringTable.end(), image.bytes.begin() + entriesSize);
    return image;
}

inline std::optional<GuestPlacement> ReserveBelowArena(uint32_t arenaLo,
                                                        uint32_t arenaHi,
                                                        size_t byteCount) {
    constexpr uint32_t kAlignment = 32;
    if (byteCount == 0 || byteCount > std::numeric_limits<uint32_t>::max() || arenaHi <= arenaLo) {
        return std::nullopt;
    }
    const uint32_t size = static_cast<uint32_t>(byteCount);
    if (size > arenaHi - arenaLo) {
        return std::nullopt;
    }
    const uint32_t unaligned = arenaHi - size;
    const uint32_t address = unaligned & ~(kAlignment - 1u);
    if (address < arenaLo || static_cast<uint64_t>(address) + size > arenaHi) {
        return std::nullopt;
    }
    return GuestPlacement{address, address};
}

} // namespace DvdFstContract

namespace DvdReadContract {

inline constexpr int32_t kInterruptTransferComplete = 1;
inline constexpr int32_t kInterruptDriveError = 2;

struct LowReadCompletion {
    int32_t returnValue;
    int32_t callbackResult;
};

inline constexpr LowReadCompletion CompletionFor(bool succeeded) noexcept {
    return succeeded ? LowReadCompletion{1, kInterruptTransferComplete}
                     : LowReadCompletion{0, kInterruptDriveError};
}

enum class HostReadFailure : uint8_t {
    None,
    MissingFile,
    BadOffset,
    ShortRead,
};

inline constexpr const char* Describe(HostReadFailure failure) noexcept {
    switch (failure) {
    case HostReadFailure::None:
        return "no error";
    case HostReadFailure::MissingFile:
        return "host file is missing or cannot be opened";
    case HostReadFailure::BadOffset:
        return "read offset is outside the host file";
    case HostReadFailure::ShortRead:
        return "host file did not contain the complete requested range";
    }
    return "unknown host read error";
}

// Read into private storage first and publish it only after the complete host
// range has been obtained. Callers can therefore leave a guest DMA destination
// untouched for every failure, including a host file truncated after indexing.
inline bool ReadExact(const std::filesystem::path& hostPath,
                      uint64_t offset,
                      uint32_t length,
                      std::vector<uint8_t>& destination,
                      HostReadFailure& failure) {
    failure = HostReadFailure::None;

    std::ifstream file(hostPath, std::ios::binary);
    if (!file.is_open()) {
        failure = HostReadFailure::MissingFile;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0 ||
        offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        offset >= static_cast<uint64_t>(fileSize)) {
        failure = HostReadFailure::BadOffset;
        return false;
    }

    const uint64_t remaining = static_cast<uint64_t>(fileSize) - offset;
    if (static_cast<uint64_t>(length) > remaining) {
        failure = HostReadFailure::ShortRead;
        return false;
    }

    std::vector<uint8_t> staged(length);
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        failure = HostReadFailure::BadOffset;
        return false;
    }

    if (length != 0) {
        file.read(reinterpret_cast<char*>(staged.data()),
                  static_cast<std::streamsize>(length));
        if (file.gcount() != static_cast<std::streamsize>(length)) {
            failure = HostReadFailure::ShortRead;
            return false;
        }
    }

    destination = std::move(staged);
    return true;
}

} // namespace DvdReadContract
