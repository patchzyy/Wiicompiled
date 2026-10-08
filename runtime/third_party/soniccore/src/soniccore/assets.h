#pragma once
#include "render_types.h"
#include "formats/adx.h"
#include "formats/pe_image.h"
#include <map>

namespace sonic {

// Everything Sonic needs from the original game, and nothing else:
//   CHRMODELS_orig.dll (or CHRMODELS.dll)  Sonic's models, animations, spin ball, effect models
//   SONIC.PVM, SON_EFF.PVM                  his textures and the streak/afterimage textures
//   COMMON_BANK00.dat, P_SONICTAILS_BANK03.dat   his sound effects
// load() accepts either a flat folder holding those files (what the sonic_extract
// tool writes) or a Sonic Adventure DX install folder (system\, SoundData\SE\).
// Physics constants, the animation table and the limb weld table were recovered
// from sonic.exe and are built into SonicCore, so sonic.exe itself is not needed.
class Assets {
public:
    bool load(const std::string& folder);
    const std::string& error() const { return error_; }

    const PeImage* chrModels() const { return chr_.loaded() ? &chr_ : nullptr; }
    const std::vector<TextureImage>& textures(int set) const { return tex_[set < 0 || set >= TEXSET_COUNT ? 0 : set]; }
    // Sound by "BANK:ENTRY" reference, e.g. "COMMON_BANK00:B00_00_17". nullptr if missing.
    std::shared_ptr<PcmSound> sound(const std::string& ref) const;
    std::vector<std::string> soundNames() const;

    // The file names load() looks for (used by the extractor).
    static std::vector<std::string> requiredFiles();

private:
    std::string find(const std::string& name) const;
    bool loadPvm(const std::string& name, std::vector<TextureImage>& out);
    void loadBank(const std::string& stem);
    std::string root_, error_;
    PeImage chr_;
    std::vector<TextureImage> tex_[TEXSET_COUNT];
    std::map<std::string, std::shared_ptr<PcmSound>> sounds_;
};

}  // namespace sonic
