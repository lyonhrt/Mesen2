#pragma once
#include "pch.h"
#include "SMS/HdPacks/HdPackConditionsSms.h"
#include "Utilities/VirtualFile.h"
#include "Shared/SettingTypes.h"
#include <utility>

// SMS-specific HD pack structures
struct HdTileKeySms {
    int32_t TileIndex = -1;
    uint32_t PaletteColors = 0;
    uint8_t TileData[32] = {}; // SMS tiles are 8x8 with 32 bytes (4 bitplanes * 8 rows)
    bool IsVramTile = false;
    bool IsSprite = false;
    
    static inline uint8_t NormalizePaletteIndex(uint32_t paletteColors) {
        // SmsVdp encodes palette selection into color bits for debugging.
        // Normalize to palette index: 0 for low palette, 1 for high palette.
        return (((paletteColors >> 16) & 0xFF) ? 1 : 0);
    }

    bool operator==(const HdTileKeySms& other) const {
        uint8_t thisPal = NormalizePaletteIndex(PaletteColors);
        uint8_t otherPal = NormalizePaletteIndex(other.PaletteColors);
        if(IsVramTile || other.IsVramTile) {
            return thisPal == otherPal && IsSprite == other.IsSprite && memcmp(TileData, other.TileData, sizeof(TileData)) == 0;
        } else {
            return TileIndex == other.TileIndex && thisPal == otherPal && IsSprite == other.IsSprite;
        }
    }
};

namespace std {
    template<> struct hash<HdTileKeySms> {
        size_t operator()(const HdTileKeySms& key) const {
            auto combine = [](size_t& seed, size_t value) {
                seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            };

            size_t h = 0;
            if(key.IsVramTile) {
                for(int i = 0; i < 32; i++) {
                    combine(h, static_cast<size_t>(key.TileData[i]));
                }
            } else {
                combine(h, static_cast<size_t>(key.TileIndex));
            }

            // Normalize palette index (0 or 1) for stability, then include sprite/background distinction
            combine(h, static_cast<size_t>(HdTileKeySms::NormalizePaletteIndex(key.PaletteColors)));
            combine(h, static_cast<size_t>(key.IsSprite ? 1 : 0));
            return h;
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

// SMS-specific screen info structure for HD pack conditions
struct HdScreenInfoSms {
    uint64_t FrameNumber = 0;
    unordered_map<uint32_t, uint8_t> WatchedAddressValues;
};

struct HdPackTileInfoSms : public HdTileKeySms {
    HdPackBitmapInfoSms* Bitmap = nullptr;
    uint32_t BitmapIndex = 0;
    uint32_t X = 0;          // X position in tile sheet
    uint32_t Y = 0;          // Y position in tile sheet
    uint32_t ScreenX = 0;    // X position on screen (for on-screen layout)
    uint32_t ScreenY = 0;    // Y position on screen (for on-screen layout)
    uint32_t Width = 8;
    uint32_t Height = 8;
    uint32_t Brightness = 255;
    uint32_t UsageCount = 0; // Total times this tile was encountered (for ordering)
    bool DefaultTile = false;
    bool ForceDisableCache = false;
    bool HorizontalMirroring = false; // Added for SMS HD pack conditions
    bool VerticalMirroring = false;   // Added for SMS HD pack conditions
    bool BackgroundPriority = false;  // Added for SMS HD pack conditions
    uint8_t PaletteIndex = 0;         // Palette index (0 or 1 for SMS)
    uint32_t VramBankId = 0;
    vector<uint32_t> HdTileData; // HD tile pixel data for PNG generation
    vector<HdPackConditionSms*> Conditions;

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
    // Define SMS-specific blend mode enum
    enum class HdPackBlendModeSms {
        Alpha,
        Add,
        Subtract
    };
    
    HdPackBlendModeSms BlendMode = HdPackBlendModeSms::Alpha;
    vector<HdPackConditionSms*> Conditions;
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
    vector<unique_ptr<HdPackConditionSms>> Conditions;
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
