// sonic_disc_patch.cpp - disc-side half of the Sonic integration (see sonic_mkw.h).
//
// Runs once inside DVDInit, after the disc and overlay files are known:
//   1. loads SonicCore from the player's Sonic Adventure DX files;
//   2. fingerprints every driver model of the replaced roster slot so the draw
//      hook can recognise them in guest memory;
//   3. patches the UI archives: the slot's roster icons become portraits
//      rendered from Sonic's model, its name message becomes "Sonic";
// Results are cached under <app data>/Cache/sonic and keyed by the source file's
// path, size and modification time, so later boots only stat the files.
#include "sonic/sonic_mkw.h"

#include "runtime_config.h"
#include "runtime_log.h"
#include "sonic/sonic_formats.h"
#include "sonic/sonic_render.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>

#ifndef RT_TAG_SONIC
#define RT_TAG_SONIC "sonic"
#endif

namespace sonic_mkw {

namespace disc_detail {

namespace fs = std::filesystem;
namespace cfg_detail {
inline fs::path Utf8(const std::string& text) { return RuntimeConfigFile::PathFromUtf8(text); }
}  // namespace cfg_detail

// Bump when the generated content changes so stale cache entries are rebuilt.
constexpr const char* kPatchVersion = "sonic-mkw-1";

const CharacterSlot kSlots[] = {
    {0, "Mario", "mr", "mario"},          {1, "Baby Peach", "bpc", "baby_peach"},
    {2, "Waluigi", "wl", "waluigi"},      {3, "Bowser", "kp", "koopa"},
    {4, "Baby Daisy", "bds", "baby_daisy"}, {5, "Dry Bones", "ka", "karon"},
    {6, "Baby Mario", "bmr", "baby_mario"}, {7, "Luigi", "lg", "luigi"},
    {8, "Toad", "ko", "kinopio"},         {9, "Donkey Kong", "dk", "donky"},
    {10, "Yoshi", "ys", "yoshi"},         {11, "Wario", "wr", "wario"},
    {12, "Baby Luigi", "blg", "baby_luigi"}, {13, "Toadette", "kk", "kinopico"},
    {14, "Koopa Troopa", "nk", "noko"},   {15, "Daisy", "ds", "daisy"},
    {16, "Peach", "pc", "peach"},         {17, "Birdo", "ca", "catherine"},
    {18, "Diddy Kong", "dd", "didy"},     {19, "King Boo", "kt", "teresa"},
    {20, "Bowser Jr.", "jr", "koopa_jr"}, {21, "Dry Bowser", "bk", "hone_koopa"},
    {22, "Funky Kong", "fk", "funky"},    {23, "Rosalina", "rs", "roseta"},
};

std::string Squash(const std::string& text) {
    std::string out;
    for (char c : ToLowerAscii(text)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
    }
    return out;
}

std::mutex g_stateMutex;
std::unordered_set<uint64_t> g_fingerprints;
std::atomic<bool> g_modelSwap{false};
std::atomic<bool> g_debug{false};

bool ReadBytes(const fs::path& path, Bytes& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool WriteBytesAtomically(const fs::path& path, const Bytes& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!file) return false;
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(temp, path, ec);
    }
    return !ec;
}

// Identity of a source file for the cache: path, size and modification time.
uint64_t FileKey(const fs::path& path, const std::string& salt) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    const auto time = fs::last_write_time(path, ec).time_since_epoch().count();
    std::ostringstream text;
    text << kPatchVersion << '|' << salt << '|' << RuntimeConfigFile::PathToUtf8(path) << '|' << size << '|'
         << time;
    const std::string s = text.str();
    return Fnv1a64(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string Hex(uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

// Cache index: one "<kind>\t<key>\t<value>" line per entry.
class CacheIndex {
public:
    explicit CacheIndex(fs::path file) : file_(std::move(file)) {
        std::ifstream in(file_);
        std::string line;
        while (std::getline(in, line)) {
            const size_t a = line.find('\t');
            const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            entries_[line.substr(0, b)] = line.substr(b + 1);
        }
    }
    bool Find(const std::string& kind, uint64_t key, std::string& value) const {
        const auto it = entries_.find(kind + '\t' + Hex(key));
        if (it == entries_.end()) return false;
        value = it->second;
        return true;
    }
    void Set(const std::string& kind, uint64_t key, const std::string& value) {
        entries_[kind + '\t' + Hex(key)] = value;
        dirty_ = true;
    }
    void Save() {
        if (!dirty_) return;
        std::ostringstream out;
        for (const auto& [k, v] : entries_) out << k << '\t' << v << '\n';
        const std::string text = out.str();
        WriteBytesAtomically(file_, Bytes(text.begin(), text.end()));
        dirty_ = false;
    }

private:
    fs::path file_;
    std::map<std::string, std::string> entries_;
    bool dirty_ = false;
};

// Marks which roster slots a file or path names. Longer icon names are matched
// first so "koopa_jr" or "hone_koopa" never count as Bowser's "koopa", and
// "baby_luigi" never as "luigi".
std::set<int> SlotsNamedBy(const std::string& path) {
    std::string joined = "_";
    for (const std::string& token : PathTokens(path)) joined += token + "_";
    std::vector<const CharacterSlot*> byLength;
    for (const auto& slot : kSlots) byLength.push_back(&slot);
    std::sort(byLength.begin(), byLength.end(), [](const CharacterSlot* a, const CharacterSlot* b) {
        return std::strlen(a->icon) > std::strlen(b->icon);
    });
    std::set<int> found;
    for (const CharacterSlot* slot : byLength) {
        const std::string needle = std::string("_") + slot->icon + "_";
        size_t pos;
        while ((pos = joined.find(needle)) != std::string::npos) {
            found.insert(slot->id);
            joined.replace(pos, needle.size(), "_#_");
        }
    }
    for (const auto& slot : kSlots) {
        if (joined.find(std::string("_") + slot.abbr + "_") != std::string::npos) found.insert(slot.id);
    }
    return found;
}

bool LowerStartsWith(const std::string& text, const char* prefix) {
    const std::string lower = ToLowerAscii(text);
    return lower.rfind(prefix, 0) == 0;
}

bool LowerEndsWith(const std::string& text, const char* suffix) {
    const std::string lower = ToLowerAscii(text);
    const size_t n = std::strlen(suffix);
    return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
}

bool OpenArchive(const Bytes& file, bool& compressed, Bytes& raw) {
    compressed = IsYaz0(file.data(), file.size());
    if (compressed) return Yaz0Decode(file.data(), file.size(), raw);
    raw = file;
    return raw.size() >= 4 && BeRead32(raw.data()) == 0x55AA382Du;
}

// ---- driver model fingerprints -----------------------------------------------------

bool IsModelArchive(const std::string& dvdPath) {
    if (!LowerEndsWith(dvdPath, ".szs")) return false;
    return LowerStartsWith(dvdPath, "/race/kart/") || LowerStartsWith(dvdPath, "/scene/model/") ||
           LowerStartsWith(dvdPath, "/demo/");
}

std::vector<uint64_t> FingerprintArchive(const Bytes& file, const std::string& dvdPath, const CharacterSlot& slot,
                                         std::vector<std::string>& log) {
    std::vector<uint64_t> prints;
    bool compressed = false;
    Bytes raw;
    U8Archive archive;
    if (!OpenArchive(file, compressed, raw) || !archive.Parse(raw.data(), raw.size())) return prints;
    const bool archiveIsSlot = SlotsNamedBy(dvdPath).count(slot.id) != 0;
    for (size_t index : archive.Files()) {
        const std::string inner = archive.PathOf(index);
        if (!LowerEndsWith(inner, ".brres")) continue;
        const auto named = SlotsNamedBy(inner);
        const auto tokens = PathTokens(inner);
        const bool isDriver = std::find(tokens.begin(), tokens.end(), "driver") != tokens.end();
        // Inside the slot's own archives take the driver models; elsewhere (shared
        // archives such as Scene/Model/Driver.szs) only files named after the slot.
        const bool take = named.count(slot.id) != 0 || (archiveIsSlot && isDriver && named.empty());
        if (!take) continue;
        const Bytes& brres = archive.At(index).data;
        for (const BrresModel& model : BrresListModels(brres.data(), brres.size())) {
            const uint64_t print = Mdl0Fingerprint(brres.data() + model.offset, brres.size() - model.offset);
            if (!print) continue;
            prints.push_back(print);
            log.push_back(dvdPath + ":" + inner + ":" + model.name + " " + Hex(print));
        }
    }
    return prints;
}

// ---- UI archives ------------------------------------------------------------------

bool IsUiArchive(const std::string& dvdPath) {
    if (!LowerEndsWith(dvdPath, ".szs")) return false;
    return LowerStartsWith(dvdPath, "/scene/ui/") || ToLowerAscii(dvdPath) == "/race/common.szs";
}

// "tt_luigi_64x64.tpl" style names: a short lowercase prefix, the icon name, a size.
bool IsSlotIconTexture(const std::string& fileName, const CharacterSlot& slot, int& width, int& height) {
    const std::string name = ToLowerAscii(fileName);
    if (!LowerEndsWith(name, ".tpl")) return false;
    const std::string stem = name.substr(0, name.size() - 4);
    const size_t underscore = stem.find('_');
    if (underscore == std::string::npos || underscore == 0 || underscore > 4) return false;
    for (size_t i = 0; i < underscore; ++i) {
        if (stem[i] < 'a' || stem[i] > 'z') return false;
    }
    const std::string icon = std::string("_") + slot.icon + "_";
    if (stem.compare(underscore, icon.size(), icon) != 0) return false;
    const std::string size = stem.substr(underscore + icon.size());
    const size_t x = size.find('x');
    if (x == std::string::npos || x == 0 || x + 1 >= size.size()) return false;
    for (size_t i = 0; i < size.size(); ++i) {
        if (i != x && (size[i] < '0' || size[i] > '9')) return false;
    }
    width = std::atoi(size.substr(0, x).c_str());
    height = std::atoi(size.substr(x + 1).c_str());
    return width > 0 && height > 0 && width <= 1024 && height <= 1024;
}

std::u16string SonicNameFor(const std::string& dvdPath) {
    // Language archives end in _<letter>.szs (E, F, G, I, J, K, M, N, Q, S, U).
    const auto tokens = PathTokens(dvdPath);
    const std::string lang = tokens.size() >= 2 ? tokens[tokens.size() - 2] : std::string();
    if (lang == "j") return u"ソニック";  // ソニック
    if (lang == "k") return u"소닉";              // 소닉
    return u"Sonic";
}

class IconCache {
public:
    const RgbaImage* Get(int width, int height) {
        const auto key = std::make_pair(width, height);
        auto it = images_.find(key);
        if (it == images_.end()) {
            RgbaImage image;
            IconStyle style;
            if (!RenderSonicIcon(width, height, style, image)) return nullptr;
            it = images_.emplace(key, std::move(image)).first;
        }
        return &it->second;
    }

private:
    std::map<std::pair<int, int>, RgbaImage> images_;
};

bool PatchUiArchive(const Bytes& file, const std::string& dvdPath, const CharacterSlot& slot, bool icons,
                    bool names, IconCache& iconCache, Bytes& out, std::vector<std::string>& log) {
    bool compressed = false;
    Bytes raw;
    U8Archive archive;
    if (!OpenArchive(file, compressed, raw) || !archive.Parse(raw.data(), raw.size())) return false;
    bool changed = false;
    for (size_t index : archive.Files()) {
        U8Archive::Node& node = archive.At(index);
        int width = 0, height = 0;
        if (icons && IsSlotIconTexture(node.name, slot, width, height)) {
            TplInfo info;
            if (TplReadInfo(node.data.data(), node.data.size(), info) && info.width <= 1024 && info.height <= 1024) {
                width = info.width;
                height = info.height;
            }
            if (const RgbaImage* image = iconCache.Get(width, height)) {
                // Keep full RGBA8 only where the original was RGBA8: the menus load
                // these archives into fixed heaps, so the files should not grow much.
                node.data = info.format == 6
                                ? TplMakeRgba8(image->rgba.data(), image->width, image->height, info.wrapS, info.wrapT)
                                : TplMakeRgb5a3(image->rgba.data(), image->width, image->height, info.wrapS, info.wrapT);
                changed = true;
                log.push_back(dvdPath + ":" + archive.PathOf(index) + " -> Sonic icon " + std::to_string(width) +
                              "x" + std::to_string(height));
            }
        } else if (names && ToLowerAscii(node.name) == "common.bmg") {
            if (BmgReplaceMessage(node.data, 9000u + uint32_t(slot.id), SonicNameFor(dvdPath))) {
                changed = true;
                log.push_back(dvdPath + ":" + archive.PathOf(index) + " -> name message " +
                              std::to_string(9000 + slot.id));
            }
        }
    }
    if (!changed) return false;
    const Bytes rebuilt = archive.Build();
    out = compressed ? Yaz0Encode(rebuilt.data(), rebuilt.size()) : rebuilt;
    return true;
}

std::string SanitizedName(const std::string& dvdPath) {
    std::string out;
    for (char c : dvdPath) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '-';
        out.push_back(keep ? c : '_');
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    return out;
}

// Identity of the SADX files the icons are rendered from, so switching installs
// (or texture mods) rebuilds the patched archives.
std::string SonicAssetsStamp(const std::string& folder) {
    std::ostringstream stamp;
    stamp << folder;
    for (const std::string& name : sonic::Assets::requiredFiles()) {
        for (const char* sub : {"", "system"}) {
            const std::string found = sonic::findFileNoCase(sub[0] ? sonic::joinPath(folder, sub) : folder, name);
            if (found.empty()) continue;
            std::error_code ec;
            const fs::path path = cfg_detail::Utf8(found);
            stamp << '|' << name << ':' << fs::file_size(path, ec) << ':'
                  << fs::last_write_time(path, ec).time_since_epoch().count();
            break;
        }
    }
    return stamp.str();
}

std::vector<std::string> AssetFallbacks() {
    std::vector<std::string> out;
    auto add = [&](const fs::path& p) { out.push_back(RuntimeConfigFile::PathToUtf8(p)); };
    const fs::path configDir = RuntimeConfigFile::ResolveConfigPath().parent_path();
    add(configDir / "SonicAssets");
    if (const auto exe = RuntimeConfigFile::ExecutableDirectory()) add(*exe / "SonicAssets");
    add(RuntimeConfigFile::ApplicationDataDirectory() / "SonicAssets");
    return out;
}

}  // namespace disc_detail

const CharacterSlot* FindCharacterSlot(const std::string& text) {
    const std::string wanted = disc_detail::Squash(text);
    if (wanted.empty()) return nullptr;
    for (const auto& slot : disc_detail::kSlots) {
        if (wanted == disc_detail::Squash(slot.name) || wanted == disc_detail::Squash(slot.abbr) ||
            wanted == disc_detail::Squash(slot.icon)) {
            return &slot;
        }
    }
    // a few common spellings
    if (wanted == "dk") return &disc_detail::kSlots[9];
    if (wanted == "bowserjr" || wanted == "jr") return &disc_detail::kSlots[20];
    if (wanted == "rosaline") return &disc_detail::kSlots[23];
    return nullptr;
}

bool ModelSwapActive() { return disc_detail::g_modelSwap.load(std::memory_order_acquire); }

bool IsSonicFingerprint(uint64_t fingerprint) {
    std::lock_guard<std::mutex> lock(disc_detail::g_stateMutex);
    return disc_detail::g_fingerprints.count(fingerprint) != 0;
}

bool DebugLogging() { return disc_detail::g_debug.load(std::memory_order_relaxed); }

void PatchDisc(const std::vector<DiscFile>& files, const RegisterDiscFile& reg) {
    using namespace disc_detail;
    namespace cfg = RuntimeConfigFile;
    if (!cfg::SonicEnabled()) return;
    g_debug = cfg::SonicDebug();
    const auto started = std::chrono::steady_clock::now();

    // Still the earlier Luigi-slot build here; the new-character version replaces this.
    const CharacterSlot* slot = FindCharacterSlot("luigi");
    if (!slot) {
        RT_LOG(RT_TAG_SONIC) << "replaces = \"" << "luigi"
                             << "\" is not a Mario Kart Wii character; Sonic is disabled." << std::endl;
        return;
    }

    std::string configured = cfg::SonicAssets();
    if (!configured.empty()) configured = cfg::PathToUtf8(cfg::ResolveRelativeToConfig(configured));
    auto& resources = SonicResources::Get();
    if (!resources.Load(configured, AssetFallbacks())) {
        RT_LOG(RT_TAG_SONIC) << "Sonic Adventure DX files not found, Sonic is disabled. Set [sonic] assets in "
                             << "Config.toml to your SADX folder (the one with system\\CHRMODELS_orig.dll) or a "
                             << "SonicAssets folder.\n"
                             << resources.Error() << std::endl;
        return;
    }
    RT_LOG(RT_TAG_SONIC) << "Sonic loaded from " << resources.Folder() << "; replacing " << slot->name << std::endl;

    const fs::path cacheDir = cfg::ApplicationDataDirectory() / "Cache" / "sonic";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    CacheIndex index(cacheDir / "index.txt");
    std::vector<std::string> log;

    // 1. driver model fingerprints
    std::unordered_set<uint64_t> prints;
    size_t modelFiles = 0;
    const bool modelSwap = cfg::SonicModel();
    if (modelSwap) {
        for (const DiscFile& file : files) {
            if (!IsModelArchive(file.dvdPath)) continue;
            const bool named = SlotsNamedBy(file.dvdPath).count(slot->id) != 0;
            const auto tokens = PathTokens(file.dvdPath);
            const bool shared = std::find(tokens.begin(), tokens.end(), "driver") != tokens.end() ||
                                LowerStartsWith(file.dvdPath, "/demo/");
            if (!named && !shared) continue;
            const uint64_t key = FileKey(file.hostPath, std::string("fp:") + slot->abbr);
            std::string cached;
            std::vector<uint64_t> filePrints;
            if (index.Find("fp", key, cached)) {
                std::istringstream in(cached);
                std::string item;
                while (std::getline(in, item, ',')) {
                    const uint64_t print = std::strtoull(item.c_str(), nullptr, 16);
                    if (print) filePrints.push_back(print);
                }
            } else {
                Bytes data;
                if (!ReadBytes(file.hostPath, data)) continue;
                filePrints = FingerprintArchive(data, file.dvdPath, *slot, log);
                std::string value = "-";
                if (!filePrints.empty()) {
                    value.clear();
                    for (uint64_t p : filePrints) value += Hex(p) + ",";
                }
                index.Set("fp", key, value);
            }
            if (!filePrints.empty()) {
                ++modelFiles;
                if (g_debug) log.push_back(file.dvdPath + ": " + std::to_string(filePrints.size()) + " model(s)");
            }
            prints.insert(filePrints.begin(), filePrints.end());
        }
    }

    // 2. UI archives
    const bool icons = cfg::SonicIcons();
    const bool names = cfg::SonicName();
    size_t patched = 0;
    if (icons || names) {
        IconCache iconCache;
        const std::string salt = std::string("ui:") + slot->icon + (icons ? ":i" : "") + (names ? ":n" : "") +
                                 (icons ? ":" + SonicAssetsStamp(resources.Folder()) : std::string());
        for (const DiscFile& file : files) {
            if (!IsUiArchive(file.dvdPath)) continue;
            const uint64_t key = FileKey(file.hostPath, salt);
            std::string cached;
            fs::path output;
            if (index.Find("ui", key, cached)) {
                if (cached == "-") continue;
                output = cacheDir / cfg::PathFromUtf8(cached);
                if (!fs::is_regular_file(output, ec)) output.clear();
            }
            if (output.empty()) {
                Bytes data, result;
                if (!ReadBytes(file.hostPath, data)) continue;
                if (!PatchUiArchive(data, file.dvdPath, *slot, icons, names, iconCache, result, log)) {
                    index.Set("ui", key, "-");
                    continue;
                }
                const std::string name = SanitizedName(file.dvdPath) + "." + Hex(key) + ".szs";
                output = cacheDir / name;
                if (!WriteBytesAtomically(output, result)) {
                    RT_LOG(RT_TAG_SONIC) << "could not write " << cfg::PathToUtf8(output) << std::endl;
                    continue;
                }
                index.Set("ui", key, name);
            }
            const auto size = fs::file_size(output, ec);
            if (ec) continue;
            reg(file.dvdPath, output, uint32_t(size));
            ++patched;
        }
    }
    index.Save();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_fingerprints = prints;
    }
    g_modelSwap = modelSwap && !prints.empty();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    RT_LOG(RT_TAG_SONIC) << "Sonic replaces " << slot->name << ": " << prints.size() << " driver model(s) in "
                         << modelFiles << " file(s), " << patched << " UI archive(s) patched (" << ms.count()
                         << " ms)" << std::endl;
    if (modelSwap && prints.empty()) {
        RT_LOG(RT_TAG_SONIC) << "no driver models found for " << slot->name
                             << "; Sonic's 3D model will not appear (icons and name still do)" << std::endl;
    }
    if (g_debug) {
        for (const std::string& line : log) RT_LOG(RT_TAG_SONIC) << "  " << line << std::endl;
    }
}

}  // namespace sonic_mkw
