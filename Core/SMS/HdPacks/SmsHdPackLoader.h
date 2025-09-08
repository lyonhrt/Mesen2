#pragma once
#include "pch.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>

class Emulator;

// Minimal SMS HD pack loader for hires.txt (HDNes-style for SMS)
// Maps runtime tiles to replacement image regions.
class SmsHdPackLoader {
public:
    struct Image {
        std::string Filename;
        std::vector<uint32_t> Pixels; // RGBA8888
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    struct ReplacementEntry {
        int ImgIndex = -1;
        uint16_t X = 0; // pixel in PNG
        uint16_t Y = 0; // pixel in PNG
        bool IsSprite = false;
        uint8_t PaletteGroup = 0; // 0 = BG, 1 = Sprite (normalized)
    };

    // Keyed by (tileIndex, paletteGroup, isSprite)
    struct Key {
        uint32_t TileIndex;
        uint8_t PaletteGroup;
        uint8_t IsSprite;
        bool operator==(const Key& o) const {
            return TileIndex == o.TileIndex && PaletteGroup == o.PaletteGroup && IsSprite == o.IsSprite;
        }
    };

    struct KeyHasher {
        size_t operator()(const Key& k) const {
            size_t h = 1469598103934665603ull;
            auto mix=[&](size_t v){ h ^= v; h *= 1099511628211ull; };
            mix((size_t)k.TileIndex);
            mix((size_t)k.PaletteGroup);
            mix((size_t)k.IsSprite);
            return h;
        }
    };

public:
    bool Load(Emulator* emu, const std::string& packFolder);

    bool IsLoaded() const { return _loaded; }
    uint32_t GetScale() const { return _scale; }

    // Lookup replacement for a tile key
    bool TryGetReplacement(uint32_t tileIndex, bool isSprite, uint8_t paletteGroup, ReplacementEntry& out) const;

    // Access image by index
    const Image* GetImage(int idx) const {
        if(idx < 0 || (size_t)idx >= _images.size()) return nullptr;
        return &_images[idx];
    }

private:
    bool _loaded = false;
    uint32_t _scale = 1;
    std::vector<Image> _images;
    std::unordered_map<Key, ReplacementEntry, KeyHasher> _map;

    static bool ReadManifest(const std::string& path, std::vector<std::string>& outImgs, std::vector<std::array<std::string,7>>& outTiles, uint32_t& outScale);
    static int ParseInt(const std::string& s, int base = 10);
    static uint32_t ParseHex32(const std::string& s);
};
