#pragma once
#include "../core/common.h"

namespace sonic {

// Decoded texture, always RGBA8.
struct Image {
    int width = 0, height = 0;
    std::vector<u8> rgba;
    bool hasAlpha = false;  // any texel with alpha < 255
};

// Decode one PowerVR texture (PVRT chunk, optional GBIX prefix).
// Palettised formats use `palette` (ARGB8888 entries) when supplied, otherwise
// a greyscale ramp.
bool decodePvr(const u8* data, size_t len, Image& out, const std::vector<u32>* palette = nullptr);

// PVM archive (PVMH header + sequence of PVR textures)
struct PvmEntry {
    std::string name;
    u32 gbix = 0;
    size_t offset = 0;  // offset of GBIX/PVRT data in archive
    size_t size = 0;
};

class PvmArchive {
public:
    bool open(const std::string& path);           // reads whole file
    bool parse(std::vector<u8>&& data);
    bool readHeaderOnly(const std::string& path);  // names only (fast index)
    const std::vector<PvmEntry>& entries() const { return entries_; }
    bool decode(size_t index, Image& out) const;
    const std::string& path() const { return path_; }

private:
    bool parseHeader(const u8* d, size_t len, bool needData);
    std::string path_;
    std::vector<u8> data_;
    std::vector<PvmEntry> entries_;
};

}  // namespace sonic
