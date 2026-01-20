#pragma once
#include "pch.h"
#include "SMS/HdPacks/HdPackConditionsSms.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/PNGHelper.h"
#include "Shared/SettingTypes.h"
#include <utility>

// SMS-specific HD pack structures
struct HdTileKeySms {
    int32_t TileIndex = -1;
    uint32_t PaletteColors = 0;
    uint8_t TileData[32] = {}; // SMS tiles are 8x8 with 32 bytes (4 bitplanes * 8 rows)
    uint8_t PaletteIndex = 0;  // 0 = low palette, 1 = high palette (used for deduplication)
    bool IsVramTile = false;
    bool IsSprite = false;

    bool operator==(const HdTileKeySms& other) const {
        if(IsVramTile || other.IsVramTile) {
            // Compare by tile data, palette index, and sprite flag
            return PaletteIndex == other.PaletteIndex && IsSprite == other.IsSprite && 
                   memcmp(TileData, other.TileData, sizeof(TileData)) == 0;
        } else {
            // Compare by tile index, palette index, and sprite flag
            return TileIndex == other.TileIndex && PaletteIndex == other.PaletteIndex && 
                   IsSprite == other.IsSprite;
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

            // Use PaletteIndex directly (0 or 1), then include sprite/background distinction
            combine(h, static_cast<size_t>(key.PaletteIndex));
            combine(h, static_cast<size_t>(key.IsSprite ? 1 : 0));
            return h;
        }
    };
}

struct HdPackBitmapInfoSms {
private:
    bool _initDone = false;

public:
    vector<uint8_t> FileData;
    string PngName;
    vector<uint32_t> PixelData;
    uint32_t* RgbData = nullptr;
    uint32_t Width = 0;
    uint32_t Height = 0;

    void Init() {
        if(_initDone) {
            return;
        }
        _initDone = true;

        if(PNGHelper::ReadPNG(FileData, PixelData, Width, Height)) {
            RgbData = PixelData.data();
            PremultiplyAlpha();
        }
        FileData = {}; // Free file data after loading
    }

    void PremultiplyAlpha() {
        for(size_t i = 0; i < PixelData.size(); i++) {
            if(PixelData[i] < 0xFF000000) {
                uint8_t* output = (uint8_t*)(PixelData.data() + i);
                uint8_t alpha = output[3] + 1;
                output[0] = (uint8_t)((alpha * output[0]) >> 8);
                output[1] = (uint8_t)((alpha * output[1]) >> 8);
                output[2] = (uint8_t)((alpha * output[2]) >> 8);
            }
        }
    }
};

// SMS tile info for HD capture (used during VDP rendering)
struct HdSmsTileInfo : public HdTileKeySms {
    uint32_t TileAddr = 0;           // VRAM address of tile data
    // PaletteIndex is inherited from HdTileKeySms (0 = low palette, 1 = high palette)
    bool HorizontalMirroring = false;
    bool VerticalMirroring = false;
    bool BackgroundPriority = false;
    
    void Reset() {
        TileIndex = -1;
        TileAddr = 0;
        PaletteColors = 0;
        PaletteIndex = 0;  // Inherited from base class
        IsVramTile = false;
        IsSprite = false;
        HorizontalMirroring = false;
        VerticalMirroring = false;
        BackgroundPriority = false;
        memset(TileData, 0, sizeof(TileData));
    }
};

// Per-pixel tile information for HD capture
struct HdSmsPixelInfo {
    HdSmsTileInfo Background;
    HdSmsTileInfo Sprites[4];  // Up to 4 sprites can overlap a pixel
    uint8_t SpriteCount = 0;
    uint8_t ScrollX = 0;
    uint8_t ScrollY = 0;
    
    void Reset() {
        Background.Reset();
        for(int i = 0; i < 4; i++) {
            Sprites[i].Reset();
        }
        SpriteCount = 0;
        ScrollX = 0;
        ScrollY = 0;
    }
};

// SMS-specific screen info structure for HD pack conditions
struct HdScreenInfoSms {
    uint64_t FrameNumber = 0;
    unordered_map<uint32_t, uint8_t> WatchedAddressValues;
    vector<HdSmsPixelInfo> ScreenTiles;  // 256x240 = 61440 pixels
    
    HdScreenInfoSms() {
        // Pre-allocate for full screen (256x240)
        ScreenTiles.resize(256 * 240);
    }
    
    void Reset() {
        for(auto& pixel : ScreenTiles) {
            pixel.Reset();
        }
    }
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
    bool TransparencyRequired = false; // NES parity: tile needs transparency
    bool Blank = false;                // NES parity: tile is blank/empty
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
    
    // NES parity: Analyze tile data for transparency and blank detection
    void UpdateFlags() {
        Blank = true;
        TransparencyRequired = false;
        
        if(HdTileData.empty()) {
            return;
        }
        
        uint32_t firstPixel = HdTileData[0];
        for(uint32_t pixel : HdTileData) {
            // Check for transparency (alpha < 255)
            if((pixel >> 24) < 255) {
                TransparencyRequired = true;
            }
            // Check if all pixels are the same (blank tile)
            if(pixel != firstPixel) {
                Blank = false;
            }
        }
    }
    
    // NES parity: Check if this is a sprite tile
    bool IsSpriteTile() const {
        return IsSprite;
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
