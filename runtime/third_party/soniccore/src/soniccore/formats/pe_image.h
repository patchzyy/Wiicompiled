#pragma once
#include "../core/common.h"
#include <map>

namespace sonic {

// A Win32 PE image (sonic.exe or one of the *MODELS.DLL files) loaded as
// read-only data. The game stores almost all of its models, level geometry,
// animations and tables as initialised data, with absolute pointers relative
// to the image base. We never execute any of it; we only follow pointers.
class PeImage {
public:
    bool load(const std::string& path);
    bool loaded() const { return !data_.empty(); }
    const std::string& path() const { return path_; }
    u32 imageBase() const { return base_; }

    // Is the virtual address inside a section with file-backed data?
    bool valid(u32 va, u32 size = 1) const;
    const u8* ptr(u32 va) const;  // nullptr when invalid

    template <typename T>
    T read(u32 va, T def = T()) const {
        if (!valid(va, sizeof(T))) return def;
        return rd<T>(ptr(va));
    }
    u32 u32at(u32 va) const { return read<u32>(va, 0); }
    std::string cstr(u32 va, size_t maxLen = 256) const;

    // Named exports (DLL data exports such as ___SONIC_OBJECTS)
    u32 exportVA(const std::string& name) const;
    const std::map<std::string, u32>& exports() const { return exports_; }

    struct Section {
        std::string name;
        u32 va, vsize, raw, rawSize;
    };
    const std::vector<Section>& sections() const { return sections_; }
    const std::vector<u8>& data() const { return data_; }
    // file offset -> VA (only for offsets inside a section)
    u32 offsetToVA(size_t off) const;

private:
    std::string path_;
    std::vector<u8> data_;
    u32 base_ = 0;
    std::vector<Section> sections_;
    std::map<std::string, u32> exports_;
};

}  // namespace sonic
