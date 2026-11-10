#pragma once
// HD sprite packs: true-colour art drawn at an integer render scale.
//
// With Runtime HdScale=S (S > 1) the map is rasterised into an S-times larger
// back buffer. Every draw keeps working on game pixels (each one becomes an
// S x S block), except sprites that have a pack in <local data>/hd: for those,
// the GPU samples the pack, so one game pixel shows S x S real source pixels.
// The 1x SHP still decides silhouette, depth, selection and shadows.
//
// Pack file <name>.hdspr, where <name> is the SHP file name without extension
// (see tools/hd/make_hd_building.py): "CHD1", u32 version=1, u32 scale,
// u32 canvasW, u32 canvasH, u32 frameCount, then per frame u32 present and, if
// present, canvasW*S x canvasH*S BGRA pixels (straight alpha).
//
// Header-only on purpose: the renderer, presenter and runtime live in separate
// static libraries; inline functions share one state per executable.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace concord::render::hd {

struct HdSpritePack {
    int scale = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
    std::vector<std::vector<std::uint32_t>> frames;   // empty = no HD for that frame
};

struct HdSpriteState {
    int scale = 1;
    std::filesystem::path root;
    std::unordered_map<std::string, std::shared_ptr<HdSpritePack>> packs;   // null = no pack
    std::unordered_map<std::string, std::shared_ptr<std::vector<std::uint32_t>>> crops;
    std::string capturePath;   // pending back-buffer capture request (BMP)
};

inline HdSpriteState& hdSpriteState() {
    static HdSpriteState state;
    return state;
}

inline void configureHdSprites(const std::filesystem::path& root, int scale) {
    auto& s = hdSpriteState();
    s.scale = std::clamp(scale, 1, 4);
    s.root = root;
    s.packs.clear();
    s.crops.clear();
}

inline int hdRenderScale() { return hdSpriteState().scale; }

// CONCORD_HD_TRACE=1 logs every new HD lookup (name, frame, box) to stderr.
inline bool hdTraceEnabled() {
    static const bool enabled = [] {
#ifdef _MSC_VER
        char* value = nullptr;
        std::size_t length = 0;
        const bool on = _dupenv_s(&value, &length, "CONCORD_HD_TRACE") == 0 && value != nullptr;
        std::free(value);
        return on;
#else
        return std::getenv("CONCORD_HD_TRACE") != nullptr;
#endif
    }();
    return enabled;
}

// "x/y/GGPOWR.SHP", "a/generic.mix!ggpowr.shp" or "mix|ggpowr.shp" -> "ggpowr"
inline std::string hdSpriteName(const std::string& sourcePath) {
    const auto cut = sourcePath.find_last_of("/\\|!");
    std::string name = cut == std::string::npos ? sourcePath : sourcePath.substr(cut + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

inline std::shared_ptr<HdSpritePack> loadHdSpritePack(const std::string& name) {
    auto& s = hdSpriteState();
    const auto found = s.packs.find(name);
    if (found != s.packs.end()) return found->second;
    std::shared_ptr<HdSpritePack> pack;
    std::ifstream in(s.root / (name + ".hdspr"), std::ios::binary);
    std::uint32_t header[6]{};
    char magic[4]{};
    if (in && in.read(magic, 4) && std::string(magic, 4) == "CHD1"
        && in.read(reinterpret_cast<char*>(header), sizeof(header) - 4)
        && header[0] == 1 && static_cast<int>(header[1]) == s.scale
        && header[2] > 0 && header[3] > 0 && header[2] <= 4096 && header[3] <= 4096 && header[4] <= 4096) {
        auto loaded = std::make_shared<HdSpritePack>();
        loaded->scale = static_cast<int>(header[1]);
        loaded->canvasWidth = static_cast<int>(header[2]);
        loaded->canvasHeight = static_cast<int>(header[3]);
        loaded->frames.resize(header[4]);
        const std::size_t pixels = static_cast<std::size_t>(header[2]) * header[3] * header[1] * header[1];
        bool ok = true;
        for (auto& frame : loaded->frames) {
            std::uint32_t present = 0;
            if (!in.read(reinterpret_cast<char*>(&present), 4)) { ok = false; break; }
            if (!present) continue;
            frame.resize(pixels);
            if (!in.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(pixels * 4))) { ok = false; break; }
        }
        if (ok) pack = std::move(loaded);
    }
    s.packs.emplace(name, pack);
    return pack;
}

// HD pixels for one decoded SHP frame, cropped to the frame's box:
// (width*S) x (height*S) BGRA, or null when there is no HD art for it.
inline const std::vector<std::uint32_t>* hdSpriteFramePixels(const std::string& sourcePath, int frameIndex,
    int offsetX, int offsetY, int width, int height, int canvasWidth, int canvasHeight) {
    auto& s = hdSpriteState();
    if (s.scale <= 1 || frameIndex < 0 || width <= 0 || height <= 0 || s.root.empty()) return nullptr;
    const std::string name = hdSpriteName(sourcePath);
    const std::string key = name + "#" + std::to_string(frameIndex) + "#" + std::to_string(offsetX) + ","
        + std::to_string(offsetY) + "," + std::to_string(width) + "x" + std::to_string(height);
    const auto cached = s.crops.find(key);
    if (cached != s.crops.end()) return cached->second.get();
    if (hdTraceEnabled())
        std::fprintf(stderr, "HD lookup %s (from %s) frame=%d box=%d,%d %dx%d canvas=%dx%d\n", name.c_str(), sourcePath.c_str(),
            frameIndex, offsetX, offsetY, width, height, canvasWidth, canvasHeight);
    std::shared_ptr<std::vector<std::uint32_t>> crop;
    const auto pack = loadHdSpritePack(name);
    if (pack && pack->canvasWidth == canvasWidth && pack->canvasHeight == canvasHeight
        && frameIndex < static_cast<int>(pack->frames.size()) && !pack->frames[frameIndex].empty()
        && offsetX >= 0 && offsetY >= 0 && offsetX + width <= canvasWidth && offsetY + height <= canvasHeight) {
        const int S = pack->scale, stride = canvasWidth * S, w = width * S, h = height * S;
        crop = std::make_shared<std::vector<std::uint32_t>>(static_cast<std::size_t>(w) * h);
        const auto& source = pack->frames[frameIndex];
        for (int y = 0; y < h; ++y) {
            const auto* row = source.data() + static_cast<std::size_t>(offsetY * S + y) * stride + offsetX * S;
            std::copy(row, row + w, crop->data() + static_cast<std::size_t>(y) * w);
        }
    }
    s.crops.emplace(key, crop);
    return crop.get();
}

// Replacement art: <hd root>/art/<file> (any scale) overrides the archive copy,
// so the 1x SHP that pairs with an HD pack ships next to it. "" = none.
inline std::string hdArtOverridePath(const std::string& fileName) {
    auto& s = hdSpriteState();
    if (s.root.empty() || fileName.empty()) return {};
    std::string lower = fileName;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::error_code error;
    const auto path = s.root / "art" / lower;
    return std::filesystem::is_regular_file(path, error) ? path.generic_string() : std::string();
}

// Back-buffer capture: the next HD map frame is saved to `path` as a BMP.
inline void requestHdCapture(const std::string& path) { hdSpriteState().capturePath = path; }
inline std::string takeHdCaptureRequest() {
    std::string path;
    path.swap(hdSpriteState().capturePath);
    return path;
}

}  // namespace concord::render::hd
