#include "pe_image.h"
#include "../core/common.h"

namespace sonic {

bool PeImage::load(const std::string& path) {
    path_ = path;
    if (!readFile(path, data_) || data_.size() < 0x200) return false;
    if (data_[0] != 'M' || data_[1] != 'Z') return false;
    u32 pe = rd<u32>(&data_[0x3c]);
    if (pe + 0xF8 > data_.size() || memcmp(&data_[pe], "PE\0\0", 4) != 0) return false;
    u16 nsec = rd<u16>(&data_[pe + 6]);
    u16 optSize = rd<u16>(&data_[pe + 20]);
    u32 opt = pe + 24;
    base_ = rd<u32>(&data_[opt + 28]);
    u32 sec = opt + optSize;
    for (u16 i = 0; i < nsec; i++) {
        const u8* s = &data_[sec + i * 40];
        Section S;
        char nm[9] = {0};
        memcpy(nm, s, 8);
        S.name = nm;
        S.vsize = rd<u32>(s + 8);
        S.va = rd<u32>(s + 12);
        S.rawSize = rd<u32>(s + 16);
        S.raw = rd<u32>(s + 20);
        sections_.push_back(S);
    }
    // exports
    u32 expRva = rd<u32>(&data_[opt + 96]);
    if (expRva) {
        auto rvaOff = [&](u32 rva) -> const u8* { return ptr(base_ + rva); };
        const u8* e = rvaOff(expRva);
        if (e) {
            u32 nNames = rd<u32>(e + 24);
            u32 addrFn = rd<u32>(e + 28), addrNames = rd<u32>(e + 32), addrOrd = rd<u32>(e + 36);
            for (u32 i = 0; i < nNames; i++) {
                const u8* np = rvaOff(addrNames + i * 4);
                const u8* op = rvaOff(addrOrd + i * 2);
                if (!np || !op) break;
                std::string name = cstr(base_ + rd<u32>(np));
                u16 ord = rd<u16>(op);
                const u8* fp = rvaOff(addrFn + ord * 4);
                if (!fp) continue;
                exports_[name] = base_ + rd<u32>(fp);
            }
        }
    }
    return true;
}

bool PeImage::valid(u32 va, u32 size) const {
    if (va < base_) return false;
    u32 rva = va - base_;
    for (auto& s : sections_) {
        if (rva >= s.va && rva + size <= s.va + std::max(s.vsize, s.rawSize)) {
            u32 o = rva - s.va;
            return o + size <= s.rawSize && size_t(s.raw) + o + size <= data_.size();
        }
    }
    return false;
}

const u8* PeImage::ptr(u32 va) const {
    if (va < base_) return nullptr;
    u32 rva = va - base_;
    for (auto& s : sections_) {
        if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rawSize)) {
            u32 o = rva - s.va;
            if (o >= s.rawSize || size_t(s.raw) + o >= data_.size()) return nullptr;
            return &data_[s.raw + o];
        }
    }
    return nullptr;
}

std::string PeImage::cstr(u32 va, size_t maxLen) const {
    std::string r;
    for (size_t i = 0; i < maxLen; i++) {
        const u8* p = ptr(va + u32(i));
        if (!p || !*p) break;
        r.push_back(char(*p));
    }
    return r;
}

u32 PeImage::exportVA(const std::string& name) const {
    auto it = exports_.find(name);
    return it == exports_.end() ? 0 : it->second;
}

u32 PeImage::offsetToVA(size_t off) const {
    for (auto& s : sections_)
        if (off >= s.raw && off < size_t(s.raw) + s.rawSize) return base_ + s.va + u32(off - s.raw);
    return 0;
}

}  // namespace sonic
