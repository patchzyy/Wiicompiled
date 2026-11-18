#pragma once

#include <cstdint>
#include <cstring>

namespace GxTextureBindingContract {

// Independent of the metadata shadow, which getters and setters may refresh.
struct ValidatedLoad {
    uint8_t bytes[32]{};
    bool valid = false;

    void Invalidate() noexcept { valid = false; }

    void Record(const uint8_t* snapshot) noexcept {
        valid = snapshot != nullptr;
        if (valid) {
            std::memcpy(bytes, snapshot, sizeof(bytes));
        }
    }

    bool CanReuse(const uint8_t* snapshot, bool needsUpload, bool constructed,
                  bool isPalette) const noexcept {
        return valid && snapshot != nullptr && !needsUpload && constructed && !isPalette &&
               std::memcmp(bytes, snapshot, sizeof(bytes)) == 0;
    }
};

// Sampler fields shared by the binding record and HLE's TexObjMeta.
struct SamplerState {
    uint32_t dataAddr = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t format = 0;
    uint32_t wrapS = 0;
    uint32_t wrapT = 0;
    uint32_t tlut = 0;
    uint32_t minFilter = 0;
    uint32_t magFilter = 0;
    float minLod = 0.0f;
    float maxLod = 0.0f;
    float lodBias = 0.0f;
    uint32_t maxAniso = 0;
    bool biasClamp = false;
    bool edgeLod = false;
    bool mipmap = false;
};

// Guest code routinely reuses a GXTexObj at the same stack address, so that
// address is not an object identity by itself - the sampler state above has to
// be part of the snapshot.
struct State : SamplerState {
    uint32_t objAddr = 0;
    uint32_t texObjId = 0;
    uint32_t texDataVersion = 0;
};

inline bool SameSamplerState(const SamplerState& lhs, const SamplerState& rhs) noexcept {
    return lhs.dataAddr == rhs.dataAddr &&
           lhs.width == rhs.width &&
           lhs.height == rhs.height &&
           lhs.format == rhs.format &&
           lhs.wrapS == rhs.wrapS &&
           lhs.wrapT == rhs.wrapT &&
           lhs.tlut == rhs.tlut &&
           lhs.minFilter == rhs.minFilter &&
           lhs.magFilter == rhs.magFilter &&
           lhs.minLod == rhs.minLod &&
           lhs.maxLod == rhs.maxLod &&
           lhs.lodBias == rhs.lodBias &&
           lhs.maxAniso == rhs.maxAniso &&
           lhs.biasClamp == rhs.biasClamp &&
           lhs.edgeLod == rhs.edgeLod &&
           lhs.mipmap == rhs.mipmap;
}

inline bool SameBinding(const State& lhs, const State& rhs) noexcept {
    return lhs.objAddr == rhs.objAddr && lhs.texObjId == rhs.texObjId && lhs.texDataVersion == rhs.texDataVersion &&
           SameSamplerState(lhs, rhs);
}

} // namespace GxTextureBindingContract
