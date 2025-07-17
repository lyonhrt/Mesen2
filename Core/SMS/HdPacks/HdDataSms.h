#pragma once
#include "pch.h"
#include "NES/HdPacks/HdPackConditions.h"
#include "Utilities/VirtualFile.h"
#include "Shared/SettingTypes.h"
#include <utility>

// Forward declarations
struct HdPackCondition;

// SMS-specific HD pack structures adapted from NES
struct HdTileKeySms {
    int32_t TileIndex = -1;
    uint32_t PaletteColors = 0;
    uint8_t TileData[32] = {}; // SMS tiles can be 8x8 or 8x16
    bool IsChrRamTile = false;

    bool operator==(const HdTileKeySms& other) const {
        if(IsChrRamTile) {
            return memcmp(TileData, other.TileData, sizeof(TileData)) == 0 && PaletteColors == other.PaletteColors;
        } else {
            return TileIndex == other.TileIndex && PaletteColors == other.PaletteColors;
        }
    }
};

namespace std {
    template<> struct hash<HdTileKeySms> {
        size_t operator()(const HdTileKeySms& key) const {
            if(key.IsChrRamTile) {
                size_t hash = key.PaletteColors;
                for(int i = 0; i < 32; i++) {
                    hash ^= key.TileData[i] + 0x9e3779b9 + (hash << 6) + (hash >> 2);
                }
                return hash;
            } else {
                return (size_t)key.TileIndex ^ ((size_t)key.PaletteColors << 16);
            }
        }
    };
}

struct HdPackBitmapInfoSms {
    vector<uint8_t> FileData;
    string PngName;
    uint32_t* RgbData = nullptr;
    uint32_t Width = 0;
    uint32_t Height = 0;

    void Init() {
        // TODO: Initialize bitmap from PNG data
    }
};

struct HdPackTileInfoSms : public HdTileKeySms {
    HdPackBitmapInfoSms* Bitmap = nullptr;
    uint32_t BitmapIndex = 0;
    uint32_t X = 0;
    uint32_t Y = 0;
    uint32_t Width = 8;
    uint32_t Height = 8;
    uint32_t Brightness = 255;
    bool DefaultTile = false;
    bool ForceDisableCache = false;
    bool IsSprite = false;  // Added for SMS sprite/background distinction
    uint32_t ChrBankId = 0;
    vector<uint32_t> HdTileData; // HD tile pixel data for PNG generation
    vector<HdPackCondition*> Conditions;

    HdTileKeySms GetKey(bool defaultKey) {
        HdTileKeySms key = *this;
        if(defaultKey) {
            key.PaletteColors = 0xFFFFFFFF;
        }
        return key;
    }

    void Init() {
        if(Bitmap) {
            Bitmap->Init();
        }
    }
};

struct HdBackgroundInfoSms {
    HdPackBitmapInfoSms* Data = nullptr;
    uint32_t Brightness = 255;
    float HorizontalScrollRatio = 0;
    float VerticalScrollRatio = 0;
    int32_t Priority = 10;
    int32_t Left = 0;
    int32_t Top = 0;
    HdPackBlendMode BlendMode = HdPackBlendMode::Alpha;
    vector<HdPackCondition*> Conditions;
};

struct HdPackAdditionalSpriteInfoSms {
    HdTileKeySms OriginalTile;
    HdTileKeySms AdditionalTile;
    int32_t OffsetX = 0;
    int32_t OffsetY = 0;
    bool IgnorePalette = false;
};

struct BgmTrackInfoSms {
    VirtualFile Filename;
    uint32_t LoopPosition = 0;
};

struct HdPackDataSms {
    uint32_t Version = 0;
    uint32_t Scale = 1;
    uint32_t OptionFlags = 0;
    bool HasOverscanConfig = false;
    OverscanDimensions Overscan = {};
    
    vector<unique_ptr<HdPackBitmapInfoSms>> ImageFileData;
    vector<unique_ptr<HdPackBitmapInfoSms>> BackgroundFileData;
    vector<unique_ptr<HdPackTileInfoSms>> Tiles;
    vector<unique_ptr<HdPackCondition>> Conditions;
    vector<HdBackgroundInfoSms> Backgrounds;
    vector<HdPackAdditionalSpriteInfoSms> AdditionalSprites;
    vector<std::pair<uint32_t, uint32_t>> FallbackTiles;
    
    unordered_map<HdTileKeySms, vector<HdPackTileInfoSms*>> TileByKey;
    unordered_map<int32_t, vector<HdBackgroundInfoSms>> BackgroundsByPriority;
    unordered_map<uint32_t, HdTileKeySms> WatchedMemoryAddresses;
    unordered_map<string, VirtualFile> PatchesByHash;
    unordered_map<int, BgmTrackInfoSms> BgmFilesById;
    unordered_map<int, VirtualFile> SfxFilesById;
    vector<uint32_t> Palette;

    void LoadAsync() {
        // TODO: Load bitmaps asynchronously
    }
};
