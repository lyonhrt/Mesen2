#pragma once
#include "pch.h"
#include "SMS/HdPacks/HdDataSms.h"
#include "Shared/SettingTypes.h"
#include "Shared/ColorUtilities.h"
#include "SMS/SmsMemoryManager.h"

// SMS HD Pack Constants
namespace SmsHdPackConstants {
    constexpr uint32_t SMS_TILE_SIZE = 8;              // SMS tiles are 8x8 pixels
    constexpr uint32_t SMS_TILE_DATA_SIZE = 32;        // 32 bytes per tile (4 bitplanes * 8 rows)
    constexpr uint32_t SMS_PALETTE_SIZE = 32;          // 32 colors total (16 bg + 16 sprite)
    constexpr uint32_t SMS_BG_PALETTE_SIZE = 16;       // Background palette size
    constexpr uint32_t SMS_SPRITE_PALETTE_SIZE = 16;   // Sprite palette size
    constexpr uint32_t SMS_VRAM_BANK_SIZE = 0x1000;    // 4KB VRAM banks
    constexpr uint32_t SMS_VRAM_TOTAL_SIZE = 0x4000;   // 16KB total VRAM
    constexpr uint32_t SMS_COLOR_DEPTH = 4;            // 4 bits per pixel
    constexpr uint32_t SMS_BITPLANES = 4;              // 4 bitplanes per tile
    constexpr uint32_t DEFAULT_HD_PACK_VERSION = 100;  // Default version number
}

class Emulator;
class SmsConsole;
class SmsVdp;

struct HdPackBuilderOptions {
    string SaveFolder;
    ScaleFilterType FilterType = ScaleFilterType::Prescale;
    uint32_t Scale = 1;
    uint32_t VramBankSize = 0x1000;
    bool GroupBlankTiles = false;
    bool SortByUsageFrequency = false;
    bool GroupRelatedSpriteTiles = false; // Group related sprite tiles contiguously in sheets
    bool IgnoreOverscan = false;
    bool DebugMode = false;            // Enable debug visualization
    bool UseActualPalette = true;      // Use actual SMS palette colors
    bool ShowPaletteInfo = false;      // Show palette info in logs
    bool ShowTileInfo = false;         // Show detailed tile extraction info in logs
    bool DumpTileGrid = false;         // Dump a debug tile grid with all tiles
    bool DumpVramContents = false;     // Dump VRAM contents for debugging
    bool HighlightSprites = false;     // Add visual indicators to sprite tiles
    bool DumpPaletteImage = false;     // Generate palette visualization image
    bool ShowTileGrid = false;         // Show grid lines around tiles in output
    bool TraceSprites = false;       // Special debug for sprite rendering issues
    bool VerboseLogging = false;       // Extra verbose logging for all operations
    bool UseNesStylePipeline = false;  // Route ProcessTile through simplified NES-style pipeline
    bool DrawTileBorders = false;      // Draw borders around tiles in PNG sheets
};

class HdPackBuilderSms {
private:
    Emulator* _emu = nullptr;
    SmsConsole* _console = nullptr;
    SmsVdp* _vdp = nullptr;
    bool _isRecording = false;
    bool _isVram = true;
    string _saveFolder;
    string _romName;
    HdPackBuilderOptions _options = {};
    HdPackDataSms _hdData = {};
    
    // Track tile usage
    unordered_map<HdTileKeySms, uint32_t> _tileUsageCount;
    unordered_map<HdTileKeySms, HdPackTileInfoSms*> _tilesByKey;
    // Canonical deduplication map (pattern + palette selection + sprite flag)
    unordered_map<uint64_t, HdPackTileInfoSms*> _tilesByCanonicalHash;
    // Combined usage count by canonical hash for stable ordering
    unordered_map<uint64_t, uint32_t> _canonicalUsageCount;
    
    // SMS palette data
    uint32_t _palette[SmsHdPackConstants::SMS_PALETTE_SIZE] = {}; // Full SMS palette
    uint32_t _spritePalette[SmsHdPackConstants::SMS_SPRITE_PALETTE_SIZE] = {}; // Sprite palette
    uint32_t _bgPalette[SmsHdPackConstants::SMS_BG_PALETTE_SIZE] = {};     // Background palette

    // Co-occurrence tracking for sprite grouping
    struct SpriteOccurrence {
        HdPackTileInfoSms* Tile;
        uint16_t X;
        uint16_t Y;
    };
    // Sprite occurrences observed within the current frame
    std::vector<SpriteOccurrence> _currentFrameSpriteOccurrences;
    // Simple frame tracking using scanline wrap-around
    int _lastScanline = -1;
    // Symmetric co-occurrence graph between sprite tiles
    std::unordered_map<HdPackTileInfoSms*, std::unordered_map<HdPackTileInfoSms*, uint32_t>> _spriteCoOccurMap;
    
    // Private utility methods for better modularity
    void InitializeHdPackData();
    void LogInitializationInfo();
    bool LoadExistingPack();  // NES parity: Load existing tiles from hires.txt and PNGs
    void InitializeDefaultSmsPalette();
    uint32_t ExtractPixelFromBitplanes(uint8_t plane0, uint8_t plane1, uint8_t plane2, uint8_t plane3, int pixelX);
    void ProcessTileRow(HdPackTileInfoSms* tile, int rowY, uint8_t* tileData);
    bool ValidateTileData(const HdPackTileInfoSms* tile) const;
    void LogTileDebugInfo(const HdPackTileInfoSms* tile) const;
    
    // Debug information
    uint32_t _totalSpriteCount = 0;
    uint32_t _totalBgCount = 0;
    uint32_t _uniqueSpriteCount = 0;
    uint32_t _uniqueBgCount = 0;
    bool _hdPackSaved = false;
    
    // NES parity: Blank tile grouping
    uint32_t _blankTileIndex = 0;
    uint32_t _blankTilePalette = 0;
    uint32_t _blankTileCount = 0;  // Count of blank tiles encountered

public:
    HdPackBuilderSms(Emulator* emu, SmsConsole* console, HdPackBuilderOptions options);
    ~HdPackBuilderSms();

    void ProcessTile(uint32_t cycle, uint32_t scanline, uint32_t tileAddr, HdTileKeySms& tile, 
                    bool isSprite, uint32_t bankHash, bool hasBgSprite);
    void ProcessFrame(HdScreenInfoSms* frameInfo);
    
    // Recording workflow methods
    void StartRecording();
    void StopRecording();
    bool IsRecording() const { return _isRecording; }
    void SaveHdPackNow();  // Explicit save method for recording workflow
    
    void SaveHdPack();
    void DumpVramContents(const string& filename);
    void DumpPaletteVisualizer(const string& filename);
    void TraceSprites();
    void GenerateDebugOverlay(HdPackTileInfoSms* tile, bool isSprite);

private:
    void AddTile(HdPackTileInfoSms* tile, uint32_t usageCount);
    uint32_t GetVramBankId(uint32_t tileAddr);
    void GenerateHdTile(HdPackTileInfoSms* tile);
    void SaveTileSheet(const vector<HdPackTileInfoSms*>& tiles, const string& saveFolder, const string& filename, bool isSprite);
    void DrawTile(HdPackTileInfoSms* tile, int tileNumber, uint32_t* pngBuffer, int pngWidth);
    
    // SaveTileSheet helper functions for better modularity
    vector<HdPackTileInfoSms*> FilterValidTiles(const vector<HdPackTileInfoSms*>& inputTiles, bool isSprite);
    void CreateTileSheets(const vector<HdPackTileInfoSms*>& tiles, const string& saveFolder, const string& filename, bool isSprite);
    void DrawTileToBuffer(HdPackTileInfoSms* tile, int gridX, int gridY, uint32_t* pngBuffer, int pngWidth, int tileSize);
    void DrawTileBorder(int gridX, int gridY, uint32_t* pngBuffer, int pngWidth, int tileSize, uint32_t color);
    string GenerateSheetFilename(const string& baseFilename, int sheetIndex, int totalSheets);
    
    // GenerateHdTile helper functions for better modularity
    bool ReadTileDataFromVram(HdPackTileInfoSms* tile, uint8_t* tileData);
    uint32_t GetPixelColor(uint8_t colorIndex, HdPackTileInfoSms* tile);
    void ProcessTilePixels(HdPackTileInfoSms* tile, const uint8_t* tileData);
    void ApplyDebugEffects(HdPackTileInfoSms* tile);
    void ApplyPrescale(const std::vector<uint32_t>& src, std::vector<uint32_t>& dst, int scale);
    void ProcessTileNesStyle(HdTileKeySms& key, uint32_t tileAddr, bool isSprite, bool transparencyRequired = false);
    
    // UpdatePalette helper functions for better modularity
    bool ValidateVdpAndSetDefaults();
    void ProcessGameGearPalette();
    void ProcessSmsPalette();
    void ProcessSg1000Palette();
    void FinalizeTransparencyAndLogging();
    
    // HD Pack manifest generation
    void GenerateHdPackTileEntries(std::ofstream& manifestFile);
    void GenerateHdNesManifest(std::ofstream& manifestFile);
    void ValidateHdNesManifestFile(const string& manifestPath);
    
    // Advanced deduplication and arrangement methods
    bool ProcessTile(HdPackTileInfoSms* tile);
    uint64_t GetTileHash(HdPackTileInfoSms* tile);
    uint64_t GetPreciseTileHash(HdPackTileInfoSms* tile);
    uint32_t GetTileVisualHash(const HdPackTileInfoSms* tile) const;
    uint64_t GetCanonicalHash(const HdTileKeySms& key) const;
    uint64_t GetCanonicalHash(const HdPackTileInfoSms* tile) const;
    uint32_t countBits(uint32_t value) const;
    void GroupRelatedTiles(std::vector<HdPackTileInfoSms*>& tiles, std::vector<std::vector<HdPackTileInfoSms*>>& groupedTiles, bool useCache);
    void VerifyPaletteUsage(HdPackTileInfoSms* tile, uint8_t* paletteRam);
    string CleanFilename(const string& filename);  // Helper for sanitizing filenames

    // Sprite grouping helpers
    void NoteSpriteOccurrence(HdPackTileInfoSms* tile, uint32_t x, uint32_t y);
    void ResetFrameIfNeeded(uint32_t scanline);
    std::vector<HdPackTileInfoSms*> ApplySpriteGrouping(const std::vector<HdPackTileInfoSms*>& input);
    
    // Palette and debugging functions
    void UpdatePalette();  // Update palette data from VDP
    uint32_t ConvertSmsColor(uint8_t smsColor);  // Convert SMS 6-bit color to RGBA
    void DumpBackgroundTilesFromVram();  // Dump background tiles directly from VRAM
    void SaveDebugTileGrid();  // Save a debug grid showing all tiles
    void LogPaletteInfo();  // Log palette information for debugging
    void SaveDebugInfo();  // Save debug information to a log file

    // Mapping of saved tile sheets and per-tile positions for HDNes-style manifest output
    struct SheetTileRef { HdPackTileInfoSms* Tile; uint16_t X; uint16_t Y; };
    struct SheetInfo { string Filename; bool IsSprite; std::vector<SheetTileRef> Tiles; };
    std::vector<SheetInfo> _sheetInfos;
};
