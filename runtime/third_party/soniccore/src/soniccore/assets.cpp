#include "assets.h"

namespace sonic {

static std::string fileStem(const std::string& n) {
    size_t s = n.find_last_of("/\\");
    std::string b = s == std::string::npos ? n : n.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

std::vector<std::string> Assets::requiredFiles() {
    return {"CHRMODELS_orig.dll", "SONIC.PVM", "SON_EFF.PVM", "COMMON_BANK00.dat", "P_SONICTAILS_BANK03.dat"};
}

std::string Assets::find(const std::string& name) const {
    // flat asset folder first, then the places a SADX install keeps them
    for (const char* sub : {"", "system", "SoundData/SE", "SYSTEM", "sounddata/se"}) {
        std::string dir = *sub ? joinPath(root_, sub) : root_;
        std::string p = findFileNoCase(dir, name);
        if (!p.empty()) return p;
    }
    return "";
}

bool Assets::loadPvm(const std::string& name, std::vector<TextureImage>& out) {
    out.clear();
    std::string p = find(name);
    PvmArchive arc;
    if (p.empty() || !arc.open(p)) return false;
    for (size_t i = 0; i < arc.entries().size(); i++) {
        TextureImage t;
        t.name = arc.entries()[i].name;
        if (!arc.decode(i, t.image)) {
            // keep indices stable: a 1x1 white texel stands in for anything undecodable
            t.image.width = t.image.height = 1;
            t.image.rgba = {255, 255, 255, 255};
        }
        out.push_back(std::move(t));
    }
    return !out.empty();
}

void Assets::loadBank(const std::string& stem) {
    std::string p = find(stem + ".dat");
    std::vector<u8> d;
    if (p.empty() || !readFile(p, d)) {
        SONIC_LOGW("Sound bank %s not found (sounds disabled)", stem.c_str());
        return;
    }
    std::vector<SoundBankEntry> entries;
    if (!parseSoundBank(d, entries)) return;
    int ok = 0;
    for (auto& e : entries) {
        auto s = std::make_shared<PcmSound>();
        if (decodeAdx(d.data() + e.offset, e.size, *s)) {
            sounds_[toUpper(stem) + ":" + toUpper(fileStem(e.name))] = s;
            ok++;
        }
    }
    SONIC_LOGI("Sound bank %s: %d sounds", stem.c_str(), ok);
}

bool Assets::load(const std::string& folder) {
    root_ = folder;
    error_.clear();
    // The SADX Mod Loader renames the real CHRMODELS.dll to CHRMODELS_orig.dll and
    // installs a stub, so accept whichever one really exports Sonic's data.
    for (const char* n : {"CHRMODELS_orig.dll", "CHRMODELS.dll"}) {
        std::string p = find(n);
        if (p.empty()) continue;
        PeImage pe;
        if (pe.load(p) && pe.exportVA("___SONIC_OBJECTS")) {
            chr_ = std::move(pe);
            break;
        }
    }
    if (!chr_.loaded()) {
        error_ = "CHRMODELS_orig.dll / CHRMODELS.dll with Sonic's models not found in " + folder;
        SONIC_LOGE("%s", error_.c_str());
        return false;
    }
    if (!loadPvm("SONIC.PVM", tex_[TEXSET_SONIC])) {
        error_ = "SONIC.PVM not found in " + folder;
        SONIC_LOGE("%s", error_.c_str());
        return false;
    }
    if (!loadPvm("SON_EFF.PVM", tex_[TEXSET_EFFECTS])) SONIC_LOGW("SON_EFF.PVM not found: streak/afterimage effects disabled");
    loadBank("COMMON_BANK00");
    loadBank("P_SONICTAILS_BANK03");
    SONIC_LOGI("Assets loaded from %s (%zu + %zu textures, %zu sounds)", folder.c_str(), tex_[0].size(), tex_[1].size(), sounds_.size());
    return true;
}

std::shared_ptr<PcmSound> Assets::sound(const std::string& ref) const {
    auto it = sounds_.find(toUpper(ref));
    return it == sounds_.end() ? nullptr : it->second;
}

std::vector<std::string> Assets::soundNames() const {
    std::vector<std::string> r;
    for (auto& kv : sounds_) r.push_back(kv.first);
    return r;
}

}  // namespace sonic
