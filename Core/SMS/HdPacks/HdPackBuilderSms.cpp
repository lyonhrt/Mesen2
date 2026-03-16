#include "pch.h"
#include "SMS/HdPacks/HdPackBuilderSms.h"
#include "SMS/HdPacks/HdPackConditionsSms.h"
#include "SMS/SmsVdp.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsTypes.h"
#include "Shared/MessageManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/xBRZ/xbrz.h"
#include "Utilities/HQX/hqx.h"
#include "Utilities/Scale2x/scalebit.h"
#include "Utilities/KreedSaiEagle/SaiEagle.h"
#include <map>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <set>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <iomanip>

// Optional debug logging helpers (no-op unless SMS_HD_DEBUG is defined)
#include "SMS/HdPacks/HdPackDebug.h"

HdPackBuilderSms::HdPackBuilderSms(Emulator* emu, SmsConsole* console, HdPackBuilderOptions options) {
    _emu = emu;
    _console = console;
    _vdp = _console->GetVdp();
    _isVram = true;
    _options = options;
    _saveFolder = options.SaveFolder;
    
    // Get ROM name for save folder
    RomInfo romInfo = _emu->GetRomInfo();
    _romName = FolderUtilities::GetFilename(romInfo.RomFile.GetFileName(), false);
    
    // Initialize HD pack data and palette
    InitializeHdPackData();
    UpdatePalette();
    
    // Log initialization information
    LogInitializationInfo();
}

void HdPackBuilderSms::UpdateConsolePointers(SmsConsole* console)
{
    _console = console;
    _vdp = console ? console->GetVdp() : nullptr;
    MessageManager::Log("[SMS HD Pack] Builder re-attached to new console (tiles preserved: " +
        std::to_string(_hdData.Tiles.size()) + ")");
}

void HdPackBuilderSms::ProcessFrame(HdScreenInfoSms* frameInfo)
{
    if(!_isRecording || !frameInfo) {
        return;
    }

    // Screen dimensions for tile processing
    constexpr uint32_t ScreenWidth = 256;
    constexpr uint32_t ScreenHeight = 240;
    if(frameInfo->ScreenTiles.size() < ScreenWidth * ScreenHeight) {
        MessageManager::Log("[SMS HD Pack] ProcessFrame: ScreenTiles size too small: " + 
                           std::to_string(frameInfo->ScreenTiles.size()));
        return;
    }

    std::unordered_set<HdTileKeySms> bgProcessed;
    std::unordered_set<HdTileKeySms> spriteProcessed;
    bgProcessed.reserve(1024);
    spriteProcessed.reserve(512);
    
    // Debug: Count pixels with data - log more frames to diagnose capture issues
    static int frameCount = 0;
    frameCount++;
    
    int bgPixels = 0, spritePixels = 0;
    for(uint32_t i = 0; i < ScreenWidth * ScreenHeight; i++) {
        if(frameInfo->ScreenTiles[i].Background.TileIndex >= 0) bgPixels++;
        if(frameInfo->ScreenTiles[i].SpriteCount > 0) spritePixels++;
    }
    
    // Log every 60 frames (roughly once per second) or first 10 frames
    if(frameCount <= 10 || frameCount % 60 == 0) {
        MessageManager::Log("[SMS HD Pack] Frame " + std::to_string(frameCount) + 
                           ": bgPixels=" + std::to_string(bgPixels) + 
                           ", spritePixels=" + std::to_string(spritePixels) +
                           ", totalTiles=" + std::to_string(_hdData.Tiles.size()));
    }
    
    // Warn if frame has no data
    if(bgPixels == 0 && spritePixels == 0) {
        static int emptyFrameCount = 0;
        if(++emptyFrameCount <= 10) {
            MessageManager::Log("[SMS HD Pack] WARNING: Frame " + std::to_string(frameCount) + " has no tile data!");
        }
    }

    for(uint32_t y = 0; y < ScreenHeight; y++) {
        for(uint32_t x = 0; x < ScreenWidth; x++) {
            HdSmsPixelInfo& pixel = frameInfo->ScreenTiles[y * ScreenWidth + x];

            const HdSmsTileInfo& bgInfo = pixel.Background;
            if(bgInfo.TileIndex >= 0) {
                HdTileKeySms key = static_cast<const HdTileKeySms&>(bgInfo);
                key.IsSprite = false;
                key.IsVramTile = true;
                
                // NES parity: detect if sprites overlay this background tile
                bool hasSpriteOver = (pixel.SpriteCount > 0);
                
                if(bgProcessed.insert(key).second) {
                    ProcessTileNesStyle(key, bgInfo.TileAddr, false, hasSpriteOver);
                } else if(hasSpriteOver) {
                    // Mark existing tile as needing transparency
                    auto existingTile = _tilesByKey.find(key);
                    if(existingTile != _tilesByKey.end()) {
                        existingTile->second->TransparencyRequired = true;
                    }
                }
            }

            for(uint8_t i = 0; i < pixel.SpriteCount && i < 4; i++) {
                const HdSmsTileInfo& sprInfo = pixel.Sprites[i];
                if(sprInfo.TileIndex < 0) {
                    continue;
                }

                HdTileKeySms key = static_cast<const HdTileKeySms&>(sprInfo);
                key.IsSprite = true;
                key.IsVramTile = true;
                if(spriteProcessed.insert(key).second) {
                    ProcessTileNesStyle(key, sprInfo.TileAddr, true);
                }
            }
        }
    }

    // Build pattern-only sets from pixel-captured tiles so we can detect
    // VRAM-scan tiles whose pattern was already seen with correct palette.
    // A pattern is keyed on TileData[32] + IsSprite (no palette).
    auto makePatternKey = [](const HdTileKeySms& k) -> std::pair<std::array<uint8_t,32>, bool> {
        std::array<uint8_t,32> arr;
        memcpy(arr.data(), k.TileData, 32);
        return { arr, k.IsSprite };
    };
    std::set<std::pair<std::array<uint8_t,32>, bool>> pixelCapturedPatterns;
    for(const auto& k : bgProcessed) {
        pixelCapturedPatterns.insert(makePatternKey(k));
    }
    for(const auto& k : spriteProcessed) {
        pixelCapturedPatterns.insert(makePatternKey(k));
    }

    // Process extra tiles from VRAM scan (nametable + sprite table walk)
    // These ensure tiles from animated sequences/transitions are captured
    // even if pixel-by-pixel capture missed them.
    // For tiles whose pattern was already captured pixel-by-pixel (with correct palette),
    // skip them entirely — the pixel-captured version is authoritative.
    uint32_t extraBgAdded = 0, extraSprAdded = 0;
    for(const HdSmsTileInfo& bgInfo : frameInfo->ExtraBgTiles) {
        if(bgInfo.TileIndex < 0) continue;
        HdTileKeySms key = static_cast<const HdTileKeySms&>(bgInfo);
        key.IsSprite = false;
        key.IsVramTile = true;
        // Skip if this pattern was already captured pixel-by-pixel with correct palette
        if(pixelCapturedPatterns.count(makePatternKey(key))) continue;
        // Keep actual PaletteColors from ScanVramTiles so the hash matches at render time.
        // SG-1000 tiles use a color hash (not CRAM bytes) so they also benefit from keeping it.
        if(bgProcessed.insert(key).second) {
            ProcessTileNesStyle(key, bgInfo.TileAddr, false);
            extraBgAdded++;
        }
    }
    for(const HdSmsTileInfo& sprInfo : frameInfo->ExtraSpriteTiles) {
        if(sprInfo.TileIndex < 0) continue;
        HdTileKeySms key = static_cast<const HdTileKeySms&>(sprInfo);
        key.IsSprite = true;
        key.IsVramTile = true;
        // Skip if this pattern was already captured pixel-by-pixel with correct palette
        if(pixelCapturedPatterns.count(makePatternKey(key))) continue;
        // Keep actual PaletteColors from ScanVramTiles so the hash matches at render time.
        if(spriteProcessed.insert(key).second) {
            ProcessTileNesStyle(key, sprInfo.TileAddr, true);
            extraSprAdded++;
        }
    }

    _totalBgCount += (uint32_t)bgProcessed.size();
    _totalSpriteCount += (uint32_t)spriteProcessed.size();
    
    // Debug: Log tile processing results periodically
    if(frameCount <= 10 || frameCount % 60 == 0) {
        MessageManager::Log("[SMS HD Pack] Frame " + std::to_string(frameCount) + 
                           " processed: bgTiles=" + std::to_string(bgProcessed.size()) + 
                           ", spriteTiles=" + std::to_string(spriteProcessed.size()) + 
                           ", extraBg=" + std::to_string(extraBgAdded) +
                           ", extraSpr=" + std::to_string(extraSprAdded) +
                           ", totalUniqueTiles=" + std::to_string(_hdData.Tiles.size()));
    }
}

// Compute a hash of tile pattern data + sprite flag, ignoring palette.
// Used for fade detection: tiles with the same pattern but different palettes share this hash.
uint64_t HdPackBuilderSms::ComputePatternHash(const uint8_t* tileData, bool isSprite)
{
    const uint64_t FNV_OFFSET = 1469598103934665603ULL;
    const uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t h = FNV_OFFSET;
    for(int i = 0; i < (int)SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        h ^= (uint64_t)tileData[i];
        h *= FNV_PRIME;
    }
    h ^= (uint64_t)(isSprite ? 1 : 0);
    h *= FNV_PRIME;
    return h;
}

// Compute a simple brightness metric from packed PaletteColors (4 palette entries).
// SMS: Each byte is --BBGGRR (2 bits per channel, values 0-3). Range 0..36.
// GG: Each byte is GGGGRRRR (low byte of 12-bit color). Range 0..120.
// Returns sum of all channel components across the 4 entries.
uint32_t HdPackBuilderSms::ComputePaletteBrightness(uint32_t paletteColors)
{
    bool isGG = _console && (_console->GetModel() == SmsModel::GameGear);
    uint32_t sum = 0;
    for(int i = 0; i < 4; i++) {
        uint8_t entry = (paletteColors >> (i * 8)) & 0xFF;
        if(isGG) {
            // GG PaletteColors packs the low byte of each 2-byte entry: GGGGRRRR
            sum += (entry & 0x0F) + ((entry >> 4) & 0x0F);
        } else {
            // SMS palette: --BBGGRR (2 bits per channel)
            sum += (entry & 0x03) + ((entry >> 2) & 0x03) + ((entry >> 4) & 0x03);
        }
    }
    return sum;
}

// Check if a tile with the given key is a faded variant of an already-captured tile.
// Updates _baseTileMap with the brightest palette seen for each pattern.
// Returns true if this tile should be skipped (it's dimmer than the base).
bool HdPackBuilderSms::IsFadedVariant(const HdTileKeySms& key, bool isSprite)
{
    uint64_t patternHash = ComputePatternHash(key.TileData, isSprite);
    uint32_t brightness = ComputePaletteBrightness(key.PaletteColors);
    
    auto it = _baseTileMap.find(patternHash);
    if(it == _baseTileMap.end()) {
        // First time seeing this pattern — register as base
        _baseTileMap[patternHash] = { key.PaletteColors, brightness };
        return false;
    }
    
    // Same palette as the base? Not a fade variant.
    if(key.PaletteColors == it->second.PaletteColors) {
        return false;
    }
    
    // Different palette. If this one is brighter, it becomes the new base (not skipped).
    if(brightness > it->second.BrightnessSum) {
        it->second.PaletteColors = key.PaletteColors;
        it->second.BrightnessSum = brightness;
        return false;
    }
    
    // This palette is dimmer or equal — it's a faded variant, skip it.
    return true;
}

// Helper: Check if tile is a solid color (every pixel has the same color index).
// SMS Mode 4 bitplane format: 8 rows × 4 bytes/row. Each row has one byte per bitplane.
// A solid-color row means each bitplane byte is either 0x00 or 0xFF (all 8 pixels same bit).
// All rows must be identical for the tile to be solid.
static bool IsSolidColorTile(const uint8_t* tileData) {
    // Check first row: each plane byte must be 0x00 or 0xFF
    for(int p = 0; p < 4; p++) {
        if(tileData[p] != 0x00 && tileData[p] != 0xFF) {
            return false;
        }
    }
    // All subsequent rows must match the first row
    for(int row = 1; row < 8; row++) {
        if(memcmp(tileData + row * 4, tileData, 4) != 0) {
            return false;
        }
    }
    return true;
}

// NES-style simplified pipeline: mirrors NES HdPackBuilder::ProcessTile
// - Registers tiles based on key (pattern + palette), counts usage
// - Creates HdPackTileInfoSms on first sight and generates HD tile immediately
// - Avoids complex canonical hashing and co-occurrence logic
void HdPackBuilderSms::ProcessTileNesStyle(HdTileKeySms& key, uint32_t tileAddr, bool isSprite, bool transparencyRequired)
{
    static int _processCallCount = 0;
    _processCallCount++;
    
    // NES parity: Blank tile grouping — DISABLED
    // The hash system is palette-sensitive (PaletteColors is part of the canonical hash),
    // so grouping blank tiles to one palette causes hash misses at render time for blank
    // tiles with different palettes. Each palette variation is a different solid color.
    // We still count blank tiles for diagnostic purposes.
    bool isSolid = IsSolidColorTile(key.TileData);
    if(isSolid) {
        _blankTileCount++;
    }
    
    // Diagnostic: log first 50 unique tile submissions to track what's being captured
    if(_processCallCount <= 50) {
        std::string tileBytes;
        for(int i = 0; i < 4; i++) tileBytes += HexUtilities::ToHex(key.TileData[i]) + " ";
        MessageManager::Log("[ProcessTile #" + std::to_string(_processCallCount) + "] " +
            (isSprite ? "SPR" : "BG") + " addr=0x" + HexUtilities::ToHex(tileAddr) +
            " pal=0x" + HexUtilities::ToHex32(key.PaletteColors) +
            " palIdx=" + std::to_string(key.PaletteIndex) +
            " solid=" + std::to_string(isSolid) +
            " row0=" + tileBytes +
            " totalTiles=" + std::to_string(_hdData.Tiles.size()));
    }
    
    // Check for existing tile - use palette-sensitive lookup if CapturePaletteVariations is enabled
    bool tileExists = false;
    HdPackTileInfoSms* existingTile = nullptr;
    
    if(_options.CapturePaletteVariations) {
        // Palette-sensitive deduplication: same pattern + same palette colors = same tile
        HdTileKeySmsPalette paletteKey;
        static_cast<HdTileKeySms&>(paletteKey) = key;
        auto existingIt = _tilesByPaletteKey.find(paletteKey);
        if(existingIt != _tilesByPaletteKey.end()) {
            tileExists = true;
            existingTile = existingIt->second;
        }
    } else {
        // Standard deduplication: same pattern + same palette bank = same tile
        auto existingIt = _tilesByKey.find(key);
        if(existingIt != _tilesByKey.end()) {
            tileExists = true;
            existingTile = existingIt->second;
        }
    }
    
    if(!tileExists) {
        // Fade detection: skip dumping faded sprite/BG variants
        // Exempt solid-color tiles — different palettes produce genuinely different colors
        if(_options.SkipFadedSprites && !isSolid && IsFadedVariant(key, isSprite)) {
            _fadedTilesSkipped++;
            if(_fadedTilesSkipped <= 50 || _fadedTilesSkipped % 200 == 0) {
                std::string tileBytes;
                for(int i = 0; i < 4; i++) tileBytes += HexUtilities::ToHex(key.TileData[i]) + " ";
                MessageManager::Log("[FadeSkip #" + std::to_string(_fadedTilesSkipped) + "] " +
                    string(isSprite ? "SPR" : "BG") +
                    " pal=0x" + HexUtilities::ToHex32(key.PaletteColors) +
                    " row0=" + tileBytes +
                    " totalSkipped=" + std::to_string(_fadedTilesSkipped) +
                    " totalCaptured=" + std::to_string(_hdData.Tiles.size()));
            }
            return;
        }

        // First time seeing this tile/palette combination, create and register
        auto hdTile = std::make_unique<HdPackTileInfoSms>();
        hdTile->PaletteColors = key.PaletteColors;
        hdTile->BasePaletteColors = key.PaletteColors; // This IS the base (brightest) palette
        // When SkipFadedSprites is on (default): apply fade at render time (Y in manifest)
        // When off (dump all mode): user explicitly dumped this variant, don't apply fade (N in manifest)
        hdTile->ApplyFade = _options.SkipFadedSprites;
        hdTile->CapturedPalette = key.CapturedPalette;  // Copy the full captured palette
        hdTile->TileIndex = (int32_t)tileAddr;
        hdTile->DefaultTile = false;
        hdTile->Brightness = 255;
        hdTile->IsSprite = isSprite;
        hdTile->IsSg1000Mode = key.IsSg1000Mode;  // Critical: copy SG-1000 mode flag for correct tile generation
        hdTile->PaletteIndex = key.PaletteIndex;
        hdTile->VramBankId = GetVramBankId(tileAddr);
        hdTile->TransparencyRequired = transparencyRequired;
        hdTile->UsageCount = 1;

        memcpy(hdTile->TileData, key.TileData, SmsHdPackConstants::SMS_TILE_DATA_SIZE);

        // Ensure HD data uses current scale
        if(_hdData.Scale == 0) { _hdData.Scale = _options.Scale; }

        // Generate HD tile pixels now (uses VRAM read + palette)
        GenerateHdTile(hdTile.get());

        // Register in lookup maps
        HdPackTileInfoSms* rawPtr = hdTile.get();
        _tilesByKey[key] = rawPtr;
        _tileUsageCount[key] = 1;
        
        // Also register in palette-sensitive map if enabled
        if(_options.CapturePaletteVariations) {
            HdTileKeySmsPalette paletteKey;
            static_cast<HdTileKeySms&>(paletteKey) = key;
            _tilesByPaletteKey[paletteKey] = rawPtr;
        }

        // Update unique counters
        if(isSprite) {
            _uniqueSpriteCount++;
        } else {
            _uniqueBgCount++;
        }

        // Add to the main tile collection (only once!)
        _hdData.Tiles.push_back(std::move(hdTile));

        if(_options.DebugMode && (_hdData.Tiles.size() % 50 == 0)) {
            MessageManager::Log("[SMS HD Pack] Unique tiles: " + std::to_string(_hdData.Tiles.size()) +
                               " (BG: " + std::to_string(_uniqueBgCount) + 
                               ", Sprite: " + std::to_string(_uniqueSpriteCount) + ")");
        }
    } else {
        // Tile already exists - just update usage count and flags
        if(transparencyRequired && existingTile) {
            existingTile->TransparencyRequired = true;
        }
        
        auto usageIt = _tileUsageCount.find(key);
        if(usageIt != _tileUsageCount.end() && usageIt->second < 0x7FFFFFFF) {
            usageIt->second++;
            if(existingTile) {
                existingTile->UsageCount = usageIt->second;
            }
        }
    }
}

void HdPackBuilderSms::DrawTileBorder(int gridX, int gridY, uint32_t* pngBuffer, int pngWidth, int tileSize, uint32_t color)
{
    if(!pngBuffer || pngWidth <= 0 || tileSize <= 0) {
        return;
    }

    int x0 = gridX * tileSize;
    int y0 = gridY * tileSize;
    int x1 = x0 + tileSize - 1;
    int y1 = y0 + tileSize - 1;
    int pngHeight = 16 * tileSize; // Fixed 16x16 grid layout

    auto putPixel = [&](int x, int y) {
        if(x >= 0 && x < pngWidth && y >= 0 && y < pngHeight) {
            pngBuffer[y * pngWidth + x] = color;
        }
    };

    for(int x = x0; x <= x1; x++) {
        putPixel(x, y0);
        putPixel(x, y1);
    }
    for(int y = y0; y <= y1; y++) {
        putPixel(x0, y);
        putPixel(x1, y);
    }
}

void HdPackBuilderSms::ValidateHdNesManifestFile(const string& manifestPath)
{
    std::ifstream in(manifestPath);
    if(!in.is_open()) {
        MessageManager::Log("[SMS HD Pack] Manifest validation: cannot open file: " + manifestPath);
        return;
    }

    std::string line;
    bool hasVer = false, hasScale = false, hasRom = false;
    std::vector<std::string> imgs;
    int lineNum = 0;
    std::string romShaInFile;
    while(std::getline(in, line)) {
        lineNum++;
        if(line.rfind("<ver>", 0) == 0) {
            hasVer = true;
        } else if(line.rfind("<scale>", 0) == 0) {
            hasScale = true;
        } else if(line.rfind("<supportedRom>", 0) == 0) {
            hasRom = true;
            std::string sha = line.substr(std::string("<supportedRom>").size());
            // Trim spaces
            sha.erase(0, sha.find_first_not_of(" \t"));
            sha.erase(sha.find_last_not_of(" \t\r\n") + 1);
            romShaInFile = sha;
            if(sha.size() != 40) {
                MessageManager::Log("[SMS HD Pack] Manifest validation: WARNING - SHA1 length is not 40: '" + sha + "'");
            }
        } else if(line.rfind("<img>", 0) == 0) {
            std::string fn = line.substr(5);
            fn.erase(0, fn.find_first_not_of(" \t"));
            fn.erase(fn.find_last_not_of(" \t\r\n") + 1);
            imgs.push_back(fn);
        }
    }
    in.close();

    if(!hasVer)  MessageManager::Log("[SMS HD Pack] Manifest validation: Missing <ver> tag");
    if(!hasScale)MessageManager::Log("[SMS HD Pack] Manifest validation: Missing <scale> tag");
    if(!hasRom)  MessageManager::Log("[SMS HD Pack] Manifest validation: Missing <supportedRom> tag");

    // Verify ROM SHA1 matches actual ROM file
    if(!romShaInFile.empty()) {
        std::string actualSha = _emu->GetRomInfo().RomFile.GetSha1Hash();
        for(char& c : actualSha) { c = (char)toupper((unsigned char)c); }
        if(actualSha != romShaInFile) {
            MessageManager::Log("[SMS HD Pack] Manifest validation: WARNING - supportedRom does not match current ROM SHA1");
            HDLOG_TAG("Manifest", "supportedRom=" + romShaInFile + ", actual=" + actualSha);
        }
    }

    // Check that all referenced images exist on disk
    int missing = 0;
    for(const std::string& img : imgs) {
        std::error_code ec;
        std::string full = FolderUtilities::CombinePath(_saveFolder, img);
        bool exists = std::filesystem::exists(std::filesystem::u8path(full), ec);
        if(!exists) {
            missing++;
            MessageManager::Log("[SMS HD Pack] Manifest validation: Missing PNG referenced by <img>: " + img);
        }
    }

    // Parse <tile> lines and validate field format and ranges
    std::ifstream in2(manifestPath);
    bool hasTile = false;
    int badTile = 0;
    const int tileSize = 8 * (int)_hdData.Scale;
    const int sheetWidth = 16 * tileSize;
    const int sheetHeight = 16 * tileSize;
    while(std::getline(in2, line)) {
        if(line.rfind("<tile>", 0) == 0) {
            hasTile = true;
            std::string rest = line.substr(6);
            // split by ','
            std::vector<std::string> parts;
            std::stringstream ss(rest);
            std::string item;
            while(std::getline(ss, item, ',')) {
                // trim
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t\r\n") + 1);
                parts.push_back(item);
            }
            if(parts.size() < 5) { badTile++; continue; }  // Minimum: imgIndex,tileData,paletteIndex,x,y
            // imgIndex
            int imgIdx = -1; try { imgIdx = std::stoi(parts[0]); } catch(...) { badTile++; continue; }
            if(imgIdx < 0 || imgIdx >= (int)imgs.size()) { badTile++; continue; }
            // tileDataHex: non-empty hex (we won't enforce length here)
            if(parts[1].empty()) { badTile++; continue; }
            // paletteIndex: decimal integer (0 or 1 for SMS, 0-255 for SG-1000)
            // No strict validation needed - just ensure it's parseable
            try { (void)std::stoi(parts[2]); } catch(...) { badTile++; continue; }
            // x,y
            int x=-1,y=-1; try { x = std::stoi(parts[3]); y = std::stoi(parts[4]); } catch(...) { badTile++; continue; }
            if(x < 0 || y < 0 || x >= sheetWidth || y >= sheetHeight) { badTile++; continue; }
            if((x % tileSize) != 0 || (y % tileSize) != 0) { badTile++; continue; }
            // brightness (parts[5]) and default (parts[6]) are accepted as-is
        }
    }
    in2.close();
    if(!hasTile) {
        MessageManager::Log("[SMS HD Pack] Manifest validation: No <tile> entries found");
    }
    if(badTile > 0) {
        MessageManager::Log("[SMS HD Pack] Manifest validation: Found " + std::to_string(badTile) + " malformed <tile> entries");
    }

    HDLOG_TAG("Manifest", "Validation summary: imgs=" + std::to_string((int)imgs.size()) +
        ", missingImgs=" + std::to_string(missing) + ", hasTile=" + std::to_string(hasTile) +
        ", badTile=" + std::to_string(badTile));
}

// Generate an HDNes-style hires.txt manifest
// Format:
// <ver>0
// <scale>N
// <supportedRom>SHA1
// <img>filename.png (one per sheet, index is order)
// #filename.png
// <tile>imgIndex,tileDataHex,paletteHex,x,y,1,N
void HdPackBuilderSms::GenerateHdNesManifest(std::ofstream& manifestFile)
{
    if(!manifestFile.is_open()) {
        MessageManager::Log("[SMS HD Pack] Error: Manifest file is not open");
        return;
    }

    // Header comments describing the manifest format
    manifestFile << "# SMS HD Pack Manifest" << std::endl;
    manifestFile << "# Generated by Mesen SMS HD Pack Builder" << std::endl;
    manifestFile << "#" << std::endl;
    manifestFile << "# <ver>       - Manifest version (always 0)" << std::endl;
    manifestFile << "# <scale>     - HD scale factor (e.g. 4 = each 8x8 tile maps to 32x32 pixels in PNG)" << std::endl;
    manifestFile << "# <supportedRom> - SHA1 hash of the ROM this pack was created for" << std::endl;
    manifestFile << "# <img>       - PNG sheet filename (index is order of appearance, starting at 0)" << std::endl;
    manifestFile << "# <paletteFormat> - Palette byte format: sms (--BBGGRR 6-bit) or gg (GGGGRRRR 12-bit)" << std::endl;
    manifestFile << "#" << std::endl;
    manifestFile << "# <tile> format: 11 comma-separated fields" << std::endl;
    manifestFile << "#   Field 0: imgIndex         - Index of the PNG sheet (matches <img> declaration order)" << std::endl;
    manifestFile << "#   Field 1: tileData         - 32-byte tile pattern as 64-char hex (8x8 tile, 4 bitplanes)" << std::endl;
    manifestFile << "#   Field 2: paletteIndex     - Palette bank: 0 = low (BG), 1 = high (sprites). SG-1000: color byte" << std::endl;
    manifestFile << "#   Field 3: x                - X pixel coordinate of this tile in the PNG sheet" << std::endl;
    manifestFile << "#   Field 4: y                - Y pixel coordinate of this tile in the PNG sheet" << std::endl;
    manifestFile << "#   Field 5: brightness       - Brightness value (default: 1)" << std::endl;
    manifestFile << "#   Field 6: defaultTile      - Default tile flag: Y = fallback tile, N = normal" << std::endl;
    manifestFile << "#   Field 7: paletteColors    - 4-byte packed CRAM snapshot as 8-char hex (palette-sensitive matching)" << std::endl;
    manifestFile << "#   Field 8: applyFade        - Y = emulator applies runtime fade effect, N = use tile as-is" << std::endl;
    manifestFile << "#   Field 9: basePaletteColors - Base (brightest) palette as 8-char hex (used for fade calculation)" << std::endl;
    manifestFile << "#   Field 10: isSg1000Mode    - S = SG-1000/TMS9918 tile, M = SMS/GG Mode 4 tile" << std::endl;
    manifestFile << "#" << std::endl;
    manifestFile << "# Section comments (# SPRITES_xxx.png, # BGTILES_xxx.png) indicate sprite vs background tiles" << std::endl;
    manifestFile << std::endl;

    // Header
    manifestFile << "<ver>0" << std::endl;
    manifestFile << "<scale>" << _options.Scale << std::endl;

    // Palette format tag: tells the loader how to interpret PaletteColors bytes
    // SMS: 6-bit RGB (--BBGGRR, 1 byte per entry), GG: 12-bit RGB (GGGGRRRR low byte, 2 bytes per entry)
    {
        SmsModel model = _console->GetModel();
        if(model == SmsModel::GameGear) {
            manifestFile << "<paletteFormat>gg" << std::endl;
        } else {
            manifestFile << "<paletteFormat>sms" << std::endl;
        }
    }

    // Supported ROM SHA1 (uppercase)
    std::string sha1 = _emu->GetRomInfo().RomFile.GetSha1Hash();
    for(char& c : sha1) { c = (char)toupper((unsigned char)c); }
    manifestFile << "<supportedRom>" << sha1 << std::endl;

    // Use _sheetInfos in the order they were saved (sprites first, then BG)
    // This ensures imgIndex in manifest matches the order of <img> declarations

    // Image list - write in the same order as _sheetInfos
    for(size_t i = 0; i < _sheetInfos.size(); i++) {
        manifestFile << "<img>" << _sheetInfos[i].Filename << std::endl;
    }
    manifestFile << std::endl;

    // Per-image sections
    auto writeHex = [](uint32_t value, int width) {
        std::stringstream ss; ss << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << value; return ss.str();
    };

    // Helper to write a single tile entry to manifest
    auto writeTileEntry = [&](const HdPackTileInfoSms* t, int imgIndex, uint32_t x, uint32_t y) {
        if(!t) return;
        
        std::stringstream tileDataHex;
        tileDataHex << std::uppercase << std::hex;
        for(int b = 0; b < (int)SmsHdPackConstants::SMS_TILE_DATA_SIZE; b++) {
            tileDataHex << std::setw(2) << std::setfill('0') << (int)t->TileData[b];
        }
        
        int paletteIndex = (int)t->PaletteIndex;
        char defFlag = t->DefaultTile ? 'Y' : 'N';
        std::string palColorsHex = HexUtilities::ToHex(t->PaletteColors);
        char fadeFlag = t->ApplyFade ? 'Y' : 'N';
        std::string basePalHex = HexUtilities::ToHex(t->BasePaletteColors);
        char sg1000Flag = t->IsSg1000Mode ? 'S' : 'M';
        
        manifestFile << "<tile>" << imgIndex << "," << tileDataHex.str() << "," << paletteIndex
                     << "," << x << "," << y << ",1," << defFlag << "," << palColorsHex
                     << "," << fadeFlag << "," << basePalHex << "," << sg1000Flag << std::endl;
    };

    for(size_t i = 0; i < _sheetInfos.size(); i++) {
        const SheetInfo& si = _sheetInfos[i];
        int imgIndex = (int)i;
        manifestFile << "#" << si.Filename << std::endl;

        for(const SheetTileRef& tr : si.Tiles) {
            // Write primary tile
            writeTileEntry(tr.Tile, imgIndex, tr.X, tr.Y);
            
            // Write alternate tiles (same visual, different TileData/palette) pointing to same PNG location
            for(const HdPackTileInfoSms* alt : tr.AlternateTiles) {
                writeTileEntry(alt, imgIndex, tr.X, tr.Y);
            }
        }

        manifestFile << std::endl;
    }
}

// Frame boundary detection and co-occurrence accumulation
void HdPackBuilderSms::ResetFrameIfNeeded(uint32_t scanline)
{
    if(_lastScanline == -1) {
        _lastScanline = (int)scanline;
        return;
    }

    if((int)scanline < _lastScanline) {
        // New frame detected: accumulate co-occurrence for sprites seen in the previous frame
        const int closeThresh = 16; // pixels, roughly 2 tiles
        for(size_t i = 0; i < _currentFrameSpriteOccurrences.size(); i++) {
            HdPackTileInfoSms* a = _currentFrameSpriteOccurrences[i].Tile;
            if(!a) continue;
            for(size_t j = i + 1; j < _currentFrameSpriteOccurrences.size(); j++) {
                HdPackTileInfoSms* b = _currentFrameSpriteOccurrences[j].Tile;
                if(!b || a == b) continue;
                int dx = std::abs((int)_currentFrameSpriteOccurrences[i].X - (int)_currentFrameSpriteOccurrences[j].X);
                int dy = std::abs((int)_currentFrameSpriteOccurrences[i].Y - (int)_currentFrameSpriteOccurrences[j].Y);
                int md = std::max(dx, dy);
                if(md <= closeThresh) {
                    uint32_t weight = (md <= 8 ? 3u : (md <= 16 ? 2u : 1u));
                    _spriteCoOccurMap[a][b] += weight;
                    _spriteCoOccurMap[b][a] += weight;
                }
            }
        }
        _currentFrameSpriteOccurrences.clear();
    }
    _lastScanline = (int)scanline;
}

void HdPackBuilderSms::NoteSpriteOccurrence(HdPackTileInfoSms* tile, uint32_t x, uint32_t y)
{
    if(!tile) return;
    SpriteOccurrence occ { tile, (uint16_t)x, (uint16_t)y };
    _currentFrameSpriteOccurrences.push_back(occ);
}

// Reorder sprites so that strongly co-occurring tiles are grouped contiguously.
std::vector<HdPackTileInfoSms*> HdPackBuilderSms::ApplySpriteGrouping(const std::vector<HdPackTileInfoSms*>& input)
{
    std::vector<HdPackTileInfoSms*> output;
    output.reserve(input.size());
    if(input.empty()) return output;

    // Build a quick lookup set for membership tests
    std::unordered_set<HdPackTileInfoSms*> inSet;
    inSet.reserve(input.size()*2);
    for(auto* t : input) { if(t) inSet.insert(t); }

    std::unordered_set<HdPackTileInfoSms*> visited;
    visited.reserve(input.size()*2);

    // Minimum strength to consider 2 tiles related
    const uint32_t minStrength = 2;

    for(HdPackTileInfoSms* seed : input) {
        if(!seed || visited.count(seed)) continue;

        // Grow a group from this seed using a greedy BFS by co-occurrence strength
        std::vector<HdPackTileInfoSms*> queue;
        queue.push_back(seed);
        visited.insert(seed);

        while(!queue.empty()) {
            HdPackTileInfoSms* cur = queue.back();
            queue.pop_back();
            output.push_back(cur);

            auto it = _spriteCoOccurMap.find(cur);
            if(it == _spriteCoOccurMap.end()) continue;

            // Collect eligible neighbors
            std::vector<std::pair<HdPackTileInfoSms*, uint32_t>> neighbors;
            neighbors.reserve(it->second.size());
            for(const auto& kv : it->second) {
                HdPackTileInfoSms* nb = kv.first;
                uint32_t weight = kv.second;
                if(!nb || !inSet.count(nb) || visited.count(nb)) continue;
                if(weight >= minStrength) {
                    neighbors.emplace_back(nb, weight);
                }
            }

            // Sort neighbors by strength (desc), fallback to usage count (desc)
            std::sort(neighbors.begin(), neighbors.end(), [](const auto& a, const auto& b){
                if(a.second != b.second) return a.second > b.second;
                // Prefer tiles seen more often
                return (a.first->UsageCount) > (b.first->UsageCount);
            });

            for(const auto& p : neighbors) {
                HdPackTileInfoSms* nb = p.first;
                if(!visited.count(nb)) {
                    visited.insert(nb);
                    queue.push_back(nb);
                }
            }
        }
    }

    // If anything was skipped (e.g., no co-occurrence data), append it in original order
    for(HdPackTileInfoSms* t : input) {
        if(t && !std::count(output.begin(), output.end(), t)) {
            output.push_back(t);
        }
    }
    return output;
}

HdPackBuilderSms::~HdPackBuilderSms() {
    MessageManager::Log("[SMS HD Pack] Destructor called");
    MessageManager::Log("[SMS HD Pack] Current tile count: " + std::to_string(_hdData.Tiles.size()));
    
    // Only save if recording was explicitly stopped or if we have tiles and haven't saved yet
    if (!_hdPackSaved && !_hdData.Tiles.empty()) {
        MessageManager::Log("[SMS HD Pack] Auto-saving HD pack data on destruction");
        SaveHdPack();
    }
    
    MessageManager::Log("[SMS HD Pack] Destructor completed");
}

void HdPackBuilderSms::StartRecording() {
    if (_isRecording) {
        MessageManager::Log("[SMS HD Pack] Recording already in progress");
        return;
    }
    
    _isRecording = true;
    _hdPackSaved = false;
    
    // Reset counters and tracking data
    _uniqueSpriteCount = 0;
    _uniqueBgCount = 0;
    _sheetInfos.clear();
    _tileUsageCount.clear();
    _tilesByKey.clear();
    _tilesByCanonicalHash.clear();
    _canonicalUsageCount.clear();
    _hdData.Tiles.clear();
    
    // NES parity: Reset blank tile grouping
    _blankTileIndex = 0;
    _blankTilePalette = 0;
    _blankTileCount = 0;
    
    // Reset fade detection state
    _baseTileMap.clear();
    _tilesByPaletteKey.clear();
    _fadedTilesSkipped = 0;
    
    // Ensure the save folder exists up-front so users can see it immediately
    FolderUtilities::CreateFolder(_saveFolder);
    {
        std::error_code ec;
        bool exists = std::filesystem::exists(std::filesystem::u8path(_saveFolder), ec);
        if(!exists) {
            MessageManager::Log("[SMS HD Pack] ERROR: Failed to ensure save folder exists: " + _saveFolder);
        } else {
            HDLOG_TAG("Folders", "Ensured save folder exists: " + _saveFolder);
        }
    }
    
    // NES parity: Load existing pack to preserve edited tiles
    // Existing tiles are loaded with their HD pixel data from PNGs
    // New tiles will be added, existing tiles will be preserved
    if(LoadExistingPack()) {
        MessageManager::Log("[SMS HD Pack] Loaded existing pack - new tiles will be merged");
    } else {
        MessageManager::Log("[SMS HD Pack] No existing pack - starting fresh");
    }
    
    MessageManager::Log("[SMS HD Pack] Started recording tile data");
    MessageManager::Log("[SMS HD Pack] Save folder: " + _saveFolder);
    
    // Update palette for the current game state
    UpdatePalette();
}

void HdPackBuilderSms::StopRecording() {
    if (!_isRecording) {
        MessageManager::Log("[SMS HD Pack] No recording in progress");
        return;
    }
    
    _isRecording = false;
    
    MessageManager::Log("[SMS HD Pack] Stopped recording - saving HD pack data");
    MessageManager::Log("[SMS HD Pack] Total tiles captured: " + std::to_string(_hdData.Tiles.size()));
    MessageManager::Log("[SMS HD Pack] Background tiles: " + std::to_string(_uniqueBgCount));
    MessageManager::Log("[SMS HD Pack] Sprite tiles: " + std::to_string(_uniqueSpriteCount));
    
    // Report blank tile statistics
    if(_blankTileCount > 0) {
        MessageManager::Log("[SMS HD Pack] Blank tile occurrences: " + std::to_string(_blankTileCount) + 
                           " (each palette variation kept as separate tile)");
    }
    
    // Save the HD pack data immediately when recording stops
    SaveHdPackNow();
}

void HdPackBuilderSms::SaveHdPackNow() {
    if (_hdPackSaved) {
        MessageManager::Log("[SMS HD Pack] HD pack already saved");
        return;
    }
    
    MessageManager::Log("[SMS HD Pack] Explicitly saving HD pack data");
    SaveHdPack();
    
    // Generate debug information if requested
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Generating debug information");
        SaveDebugInfo();
        if(_options.DumpTileGrid) {
            SaveDebugTileGrid();
        }
    }
    
    _hdPackSaved = true;
    MessageManager::Log("[SMS HD Pack] HD pack save completed");
}

void HdPackBuilderSms::InitializeHdPackData() {
    // Initialize HD pack data with SMS-specific settings
    _hdData.Scale = _options.Scale;
    _hdData.Version = SmsHdPackConstants::DEFAULT_HD_PACK_VERSION;
    
    // Initialize counters and tracking data
    _totalSpriteCount = 0;
    _totalBgCount = 0;
    _uniqueSpriteCount = 0;
    _uniqueBgCount = 0;
    _tileUsageCount.clear();
    _tilesByKey.clear();
}

// NES parity: Load existing tiles from hires.txt and PNGs to preserve edited tiles
bool HdPackBuilderSms::LoadExistingPack() {
    string manifestPath = FolderUtilities::CombinePath(_saveFolder, "hires.txt");
    
    std::error_code ec;
    if(!std::filesystem::exists(std::filesystem::u8path(manifestPath), ec)) {
        MessageManager::Log("[SMS HD Pack] No existing pack found at: " + manifestPath);
        return false;
    }
    
    MessageManager::Log("[SMS HD Pack] Loading existing pack from: " + manifestPath);
    
    // Parse manifest file
    std::ifstream in(manifestPath);
    if(!in.is_open()) {
        MessageManager::Log("[SMS HD Pack] Failed to open manifest: " + manifestPath);
        return false;
    }
    
    std::vector<std::string> imgFiles;
    std::vector<std::tuple<int, std::string, std::string, int, int, bool, uint32_t, bool>> tileEntries; // imgIdx, tileHex, palHex, x, y, isSprite, paletteColors, isSg1000
    uint32_t existingScale = 1;
    bool sectionIsSprite = false;
    
    std::string line;
    while(std::getline(in, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        size_t end = line.find_last_not_of(" \t\r\n");
        if(start == std::string::npos) continue;
        line = line.substr(start, end - start + 1);
        
        if(line.empty()) continue;
        
        if(line[0] == '#') {
            // Section header
            std::string name = line.substr(1);
            sectionIsSprite = (name.find("SPRITES") != std::string::npos);
            continue;
        }
        
        if(line.rfind("<scale>", 0) == 0) {
            std::string rest = line.substr(7);
            size_t lt = rest.find('<');
            if(lt != std::string::npos) rest = rest.substr(0, lt);
            try { existingScale = std::stoi(rest); } catch(...) {}
            continue;
        }
        
        if(line.rfind("<img>", 0) == 0) {
            std::string fn = line.substr(5);
            size_t s = fn.find_first_not_of(" \t");
            size_t e = fn.find_last_not_of(" \t\r\n");
            if(s != std::string::npos) fn = fn.substr(s, e - s + 1);
            imgFiles.push_back(fn);
            continue;
        }
        
        if(line.rfind("<tile>", 0) == 0) {
            std::string content = line.substr(6);
            size_t close = content.find("</tile>");
            if(close != std::string::npos) content = content.substr(0, close);
            
            // Parse: imgIndex,tileDataHex,paletteHex,x,y,...
            std::vector<std::string> parts;
            std::stringstream ss(content);
            std::string tok;
            while(std::getline(ss, tok, ',')) {
                size_t s = tok.find_first_not_of(" \t");
                size_t e = tok.find_last_not_of(" \t");
                if(s != std::string::npos) tok = tok.substr(s, e - s + 1);
                parts.push_back(tok);
            }
            
            if(parts.size() >= 5) {
                try {
                    int imgIdx = std::stoi(parts[0]);
                    int x = std::stoi(parts[3]);
                    int y = std::stoi(parts[4]);
                    // Parse PaletteColors from field 7 (hex) if present
                    uint32_t palColors = 0;
                    if(parts.size() >= 8 && !parts[7].empty()) {
                        try { palColors = (uint32_t)std::stoul(parts[7], nullptr, 16); } catch(...) {}
                    }
                    // Parse IsSg1000Mode from field 10 (S or M) if present
                    bool isSg1000 = false;
                    if(parts.size() >= 11 && !parts[10].empty()) {
                        isSg1000 = (parts[10][0] == 'S' || parts[10][0] == 's');
                    }
                    tileEntries.push_back({imgIdx, parts[1], parts[2], x, y, sectionIsSprite, palColors, isSg1000});
                } catch(...) {}
            }
            continue;
        }
    }
    in.close();
    
    // Check scale compatibility
    if(existingScale != _options.Scale) {
        MessageManager::Log("[SMS HD Pack] WARNING: Existing pack scale (" + std::to_string(existingScale) + 
                           ") differs from requested scale (" + std::to_string(_options.Scale) + ")");
        // Use existing scale to preserve tiles
        _hdData.Scale = existingScale;
    }
    
    // Load PNG images
    std::vector<std::vector<uint32_t>> loadedImages;
    std::vector<std::pair<uint32_t, uint32_t>> imageSizes; // width, height
    
    for(const std::string& imgFile : imgFiles) {
        std::string fullPath = FolderUtilities::CombinePath(_saveFolder, imgFile);
        std::vector<uint8_t> pngData;
        uint32_t w = 0, h = 0;
        
        if(PNGHelper::ReadPNG(fullPath, pngData, w, h)) {
            std::vector<uint32_t> pixels(w * h);
            if(pngData.size() == w * h * 4) {
                memcpy(pixels.data(), pngData.data(), pngData.size());
            }
            loadedImages.push_back(std::move(pixels));
            imageSizes.push_back({w, h});
            MessageManager::Log("[SMS HD Pack] Loaded image: " + imgFile + " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
        } else {
            loadedImages.push_back({});
            imageSizes.push_back({0, 0});
            MessageManager::Log("[SMS HD Pack] Failed to load image: " + imgFile);
        }
    }
    
    // Create tile entries with preserved HD pixel data
    uint32_t tileSize = 8 * _hdData.Scale;
    int loadedCount = 0;
    
    for(const auto& entry : tileEntries) {
        int imgIdx = std::get<0>(entry);
        const std::string& tileHex = std::get<1>(entry);
        const std::string& palHex = std::get<2>(entry);
        int x = std::get<3>(entry);
        int y = std::get<4>(entry);
        bool isSprite = std::get<5>(entry);
        uint32_t paletteColors = std::get<6>(entry);
        bool isSg1000 = std::get<7>(entry);
        
        if(imgIdx < 0 || imgIdx >= (int)loadedImages.size()) continue;
        if(loadedImages[imgIdx].empty()) continue;
        
        // Parse tile data from hex
        uint8_t tileData[32] = {};
        for(size_t i = 0; i + 1 < tileHex.size() && i/2 < 32; i += 2) {
            auto hexVal = [](char c) -> int {
                if(c >= '0' && c <= '9') return c - '0';
                if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return 0;
            };
            tileData[i/2] = (hexVal(tileHex[i]) << 4) | hexVal(tileHex[i+1]);
        }
        
        // Parse palette index (stored as decimal integer, not hex)
        // For SMS Mode 4: 0 or 1 (palette bank)
        // For SG-1000: color byte (0-255)
        uint8_t paletteIndex = 0;
        try { paletteIndex = (uint8_t)std::stoi(palHex); } catch(...) {}
        
        // Create tile info
        auto hdTile = std::make_unique<HdPackTileInfoSms>();
        memcpy(hdTile->TileData, tileData, 32);
        hdTile->PaletteColors = paletteColors;  // Loaded from manifest for palette-sensitive hashing
        hdTile->IsSprite = isSprite;
        hdTile->PaletteIndex = paletteIndex;  // Use parsed value from manifest
        hdTile->X = x;
        hdTile->Y = y;
        hdTile->IsVramTile = true;
        hdTile->IsSg1000Mode = isSg1000;
        
        // Extract HD pixel data from loaded PNG
        uint32_t imgW = imageSizes[imgIdx].first;
        uint32_t imgH = imageSizes[imgIdx].second;
        const std::vector<uint32_t>& imgPixels = loadedImages[imgIdx];
        
        hdTile->HdTileData.resize(tileSize * tileSize);
        for(uint32_t dy = 0; dy < tileSize && (y + dy) < imgH; dy++) {
            for(uint32_t dx = 0; dx < tileSize && (x + dx) < imgW; dx++) {
                uint32_t srcIdx = (y + dy) * imgW + (x + dx);
                if(srcIdx < imgPixels.size()) {
                    hdTile->HdTileData[dy * tileSize + dx] = imgPixels[srcIdx];
                }
            }
        }
        
        // Create key and add to tracking maps
        // CRITICAL: Key must include all fields used in hash function
        HdTileKeySms key;
        memcpy(key.TileData, tileData, 32);
        key.PaletteColors = paletteColors;  // From manifest, used in hash
        key.PaletteIndex = hdTile->PaletteIndex;  // Must match hash
        key.IsSprite = isSprite;
        key.IsVramTile = true;
        key.IsSg1000Mode = isSg1000;  // From manifest, must match captured tiles
        
        HdPackTileInfoSms* rawPtr = hdTile.get();
        _hdData.Tiles.push_back(std::move(hdTile));
        _tilesByKey[key] = rawPtr;
        
        // Mark with high usage count to preserve order (NES parity)
        _tileUsageCount[key] = 0xFFFFFFFF - loadedCount;
        rawPtr->UsageCount = _tileUsageCount[key];
        
        loadedCount++;
    }
    
    MessageManager::Log("[SMS HD Pack] Loaded " + std::to_string(loadedCount) + " existing tiles from pack");
    
    // Count sprites vs backgrounds from loaded tiles
    for(const auto& tile : _hdData.Tiles) {
        if(tile->IsSprite) {
            _uniqueSpriteCount++;
        } else {
            _uniqueBgCount++;
        }
    }
    
    return loadedCount > 0;
}

void HdPackBuilderSms::LogInitializationInfo() {
    MessageManager::Log("[SMS HD Pack] Started tile dumping for: " + _romName);
    MessageManager::Log("[SMS HD Pack] Save location: " + _saveFolder);
    
    // Log debug options
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Debug mode enabled");
        if(_options.UseActualPalette) {
            MessageManager::Log("[SMS HD Pack] Using actual SMS palette colors");
        } else {
            MessageManager::Log("[SMS HD Pack] Using debug color scheme");
        }
        if(_options.ShowPaletteInfo) {
            LogPaletteInfo();
        }
        if(_options.DumpTileGrid) {
            MessageManager::Log("[SMS HD Pack] Tile grid dump will be generated");
        }
    }
}

// Main function to update palette data from VDP.
// 
// This function coordinates the entire palette update process:
// 1. Validates VDP availability and sets defaults if needed
// 2. Determines the console model and video mode
// 3. Processes palette data using the appropriate format (Game Gear, SMS, or SG-1000)
// 4. Finalizes transparency settings and debug logging
// 
// The function now uses a modular approach with helper functions for better
// maintainability and clarity. Each console type and palette format is handled
// by a focused helper function.
void HdPackBuilderSms::UpdatePalette() {
    // Validate VDP and set defaults if needed
    if(!ValidateVdpAndSetDefaults()) {
        return;
    }
    
    // Get VDP state to determine video mode
    SmsVdpState state = _vdp->GetState();
    
    if(state.UseMode4) {
        // SMS Mode 4 - determine console model for palette format
        SmsModel model = _console->GetModel();
        
        if(model == SmsModel::GameGear) {
            // Game Gear uses 12-bit RGB stored in internal palette RAM
            ProcessGameGearPalette();
        } else {
            // SMS uses 6-bit RGB stored in palette RAM
            ProcessSmsPalette();
        }
        
        // Finalize transparency and logging for Mode 4
        FinalizeTransparencyAndLogging();
    } else {
        // SG-1000 mode - use 15-bit RGB fixed palette
        ProcessSg1000Palette();
    }
}

uint32_t HdPackBuilderSms::ConvertSmsColor(uint8_t smsColor)
{
    // SMS color format: 00BBGGRR (2 bits per channel)
    uint8_t r = smsColor & 0x03;
    uint8_t g = (smsColor >> 2) & 0x03;
    uint8_t b = (smsColor >> 4) & 0x03;
    
    // Expand 2-bit values (0-3) to full 8-bit range (0-255)
    // Use enhanced scaling for better color reproduction in Castle of Illusion
    // Values: 0 -> 0, 1 -> 85, 2 -> 170, 3 -> 255
    // This provides better color depth and accuracy
    static const uint8_t colorLookup[4] = {0, 85, 170, 255};
    r = colorLookup[r];
    g = colorLookup[g];
    b = colorLookup[b];
    
    // Add debug logging if enabled
    if(_options.DebugMode && _options.ShowPaletteInfo) {
        std::stringstream ss;
        ss << "[SMS HD Pack] Color conversion: SMS=0x" << std::hex << (int)smsColor
           << " (R=" << (int)(smsColor & 0x03) << ", G=" << (int)((smsColor >> 2) & 0x03) << ", B=" << (int)((smsColor >> 4) & 0x03) << ")"
           << " -> RGB(" << std::dec << (int)r << "," << (int)g << "," << (int)b << ")";
        MessageManager::Log(ss.str());
    }
    
    // Return as ARGB format (0xAARRGGBB) with full alpha
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void HdPackBuilderSms::InitializeDefaultSmsPalette() {
    // Initialize with default SMS palette colors when VDP is not available
    // These are standard SMS colors that provide good visibility
    
    // Default background palette
    static const uint8_t defaultBgColors[16] = {
        0x00, 0x15, 0x2A, 0x3F, 0x05, 0x1A, 0x2F, 0x35,
        0x0A, 0x1F, 0x25, 0x3A, 0x0F, 0x25, 0x3A, 0x3F
    };
    
    // Default sprite palette (slightly different for distinction)
    static const uint8_t defaultSpriteColors[16] = {
        0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
        0x15, 0x1A, 0x2A, 0x2F, 0x35, 0x3A, 0x2F, 0x3F
    };
    
    for(int i = 0; i < SmsHdPackConstants::SMS_BG_PALETTE_SIZE; i++) {
        _bgPalette[i] = ConvertSmsColor(defaultBgColors[i]);
    }
    
    for(int i = 0; i < SmsHdPackConstants::SMS_SPRITE_PALETTE_SIZE; i++) {
        _spritePalette[i] = ConvertSmsColor(defaultSpriteColors[i]);
    }
    
    // Only sprite color 0 is transparent (lets BG show through)
    // BG color 0 must remain opaque - it's the actual background color
    _spritePalette[0] = 0x00000000;
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Initialized default SMS palette");
    }
}

void HdPackBuilderSms::LogPaletteInfo() {
    MessageManager::Log("[SMS HD Pack] Palette Information:");
    
    // Log sprite palette
    MessageManager::Log("[SMS HD Pack] Sprite Palette:");
    for(int i = 0; i < 16; i++) {
        uint32_t color = _spritePalette[i];
        uint8_t r = color & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = (color >> 16) & 0xFF;
        
        std::stringstream ss;
        ss << "[SMS HD Pack] Color " << std::dec << i << ": RGB(" 
           << std::dec << (int)r << "," << (int)g << "," << (int)b << ") - "
           << "#" << std::hex << std::setw(2) << std::setfill('0') << (int)r
           << std::hex << std::setw(2) << std::setfill('0') << (int)g
           << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        
        MessageManager::Log(ss.str());
    }
    
    // Log background palette if different
    bool palettesAreDifferent = false;
    for(int i = 0; i < 16; i++) {
        if(_spritePalette[i] != _bgPalette[i]) {
            palettesAreDifferent = true;
            break;
        }
    }
    
    if(palettesAreDifferent) {
        MessageManager::Log("[SMS HD Pack] Background Palette (different from sprite palette):");
        for(int i = 0; i < 16; i++) {
            uint32_t color = _bgPalette[i];
            uint8_t r = color & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = (color >> 16) & 0xFF;
            
            std::stringstream ss;
            ss << "[SMS HD Pack] Color " << std::dec << i << ": RGB(" 
               << std::dec << (int)r << "," << (int)g << "," << (int)b << ") - "
               << "#" << std::hex << std::setw(2) << std::setfill('0') << (int)r
               << std::hex << std::setw(2) << std::setfill('0') << (int)g
               << std::hex << std::setw(2) << std::setfill('0') << (int)b;
            
            MessageManager::Log(ss.str());
        }
    } else {
    }
}

void HdPackBuilderSms::ProcessTile(uint32_t cycle, uint32_t scanline, uint32_t tileAddr, HdTileKeySms& tile, 
                                    bool isSprite, uint32_t bankHash, bool hasBgSprite)
{
    // Debug: Log every sprite tile processed
    if(_options.DebugMode && isSprite) {
        static uint32_t s_spriteProcessCount = 0;
        s_spriteProcessCount++;
        if(s_spriteProcessCount <= 10) {
            MessageManager::Log("[SMS HD Pack] SPRITE TILE PROCESSED #" + std::to_string(s_spriteProcessCount) + 
                              " at cycle=" + std::to_string(cycle) + " scanline=" + std::to_string(scanline));
        }
    }
    
    // Skip if not recording - this ensures tiles are only captured during recording sessions
    if (!_isRecording) {
        return;
    }
    
    // Skip if VDP is not available
    if(!_vdp) {
        return;
    }

    // Optional: simplified NES-style pipeline for stability and easier debugging
    if(_options.UseNesStylePipeline) {
        ProcessTileNesStyle(tile, tileAddr, isSprite);
        return;
    }
    
    // Detect frame boundaries and accumulate co-occurrence data on wrap
    ResetFrameIfNeeded(scanline);
    
    // Track total tile counts
    if(isSprite) {
        _totalSpriteCount++;
    } else {
        _totalBgCount++;
    }
    
    // SMS VDP VRAM regions: 
    // - Pattern Generator Table (Tile data): 0x0000-0x3FFF
    // - Name Table (Background tilemap): 0x3800-0x3EFF
    // - Sprite Attribute Table: 0x3F00-0x3FFF
    std::string vramRegion;
    if(tileAddr < 0x3800) {
        vramRegion = "Pattern Generator Table";
    } else if(tileAddr < 0x3F00) {
        vramRegion = "Name Table (Background)";
    } else {
        vramRegion = "Sprite Attribute Table";
    }
    
    // Track usage count (per-struct key) and canonical usage (pattern+palette+sprite)
    _tileUsageCount[tile]++;
    uint64_t canonicalHash = GetCanonicalHash(tile);
    _canonicalUsageCount[canonicalHash]++;

    if(_options.DebugMode && isSprite) {
        static std::unordered_map<uint64_t, uint32_t> s_spriteHashLogCount;
        uint32_t& logCount = s_spriteHashLogCount[canonicalHash];
        if(logCount < 3) {
            std::stringstream ss;
            ss << "[SMS HD Pack] Sprite canonical hash: 0x" << std::hex << canonicalHash
               << " pal=" << std::dec << (int)tile.PaletteIndex
               << " usage=" << _canonicalUsageCount[canonicalHash];

            std::string dataHex;
            dataHex.reserve(SmsHdPackConstants::SMS_TILE_DATA_SIZE * 2);
            for(int i = 0; i < (int)SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
                char buf[3];
                sprintf_s(buf, sizeof(buf), "%02X", tile.TileData[i]);
                dataHex.append(buf);
            }
            ss << " data=" << dataHex;
            MessageManager::Log(ss.str());
        }
        logCount++;
    }
    
    // Prefer canonical deduplication to guarantee no repeats
    HdPackTileInfoSms* canonicalTile = nullptr;
    auto itCanon = _tilesByCanonicalHash.find(canonicalHash);
    if(itCanon != _tilesByCanonicalHash.end()) {
        canonicalTile = itCanon->second;
        // Update combined usage and sprite occurrence
        if(canonicalTile) {
            canonicalTile->UsageCount = _canonicalUsageCount[canonicalHash];
            // Insert fast path mapping for this exact key for future lookups
            _tilesByKey[tile] = canonicalTile;
            if(isSprite) {
                NoteSpriteOccurrence(canonicalTile, cycle, scanline);
            }
        }
        if(_options.DebugMode && isSprite) {
            static std::unordered_map<uint64_t, uint32_t> s_spriteDupLogCount;
            uint32_t& dupCount = s_spriteDupLogCount[canonicalHash];
            if(dupCount < 3) {
                std::stringstream ss;
                ss << "[SMS HD Pack] Sprite canonical duplicate hit: 0x" << std::hex << canonicalHash
                   << " usage=" << std::dec << _canonicalUsageCount[canonicalHash];
                MessageManager::Log(ss.str());
            }
            if(canonicalTile && memcmp(canonicalTile->TileData, tile.TileData, SmsHdPackConstants::SMS_TILE_DATA_SIZE) != 0) {
                static std::unordered_map<uint64_t, uint32_t> s_spriteCollisionLogCount;
                uint32_t& collisionCount = s_spriteCollisionLogCount[canonicalHash];
                if(collisionCount < 3) {
                    std::string existingHex;
                    existingHex.reserve(SmsHdPackConstants::SMS_TILE_DATA_SIZE * 2);
                    std::string incomingHex;
                    incomingHex.reserve(SmsHdPackConstants::SMS_TILE_DATA_SIZE * 2);
                    for(int i = 0; i < (int)SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
                        char buf[3];
                        sprintf_s(buf, sizeof(buf), "%02X", canonicalTile->TileData[i]);
                        existingHex.append(buf);
                    }
                    for(int i = 0; i < (int)SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
                        char buf[3];
                        sprintf_s(buf, sizeof(buf), "%02X", tile.TileData[i]);
                        incomingHex.append(buf);
                    }

                    std::stringstream ss;
                    ss << "[SMS HD Pack] SPRITE HASH COLLISION detected: hash=0x" << std::hex << canonicalHash
                       << " existing=" << existingHex << " new=" << incomingHex;
                    MessageManager::Log(ss.str());
                }
                collisionCount++;
            }
            dupCount++;
        }
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] DEBUG: Canonical duplicate tile encountered, updated usage count: " + std::to_string(_canonicalUsageCount[canonicalHash]));
        }
        return;
    }
    
    // Check if we already have this tile by struct key (should be rare if canonical caught it)
    auto existingTile = _tilesByKey.find(tile);
    if(existingTile == _tilesByKey.end()) {
        // Track unique tile counts
        if(isSprite) {
            _uniqueSpriteCount++;
        } else {
            _uniqueBgCount++;
        }
        
        // Create new HD pack tile info
        HdPackTileInfoSms* hdTile = new HdPackTileInfoSms();
        *((HdTileKeySms*)hdTile) = tile;
        
        // Store screen position information for on-screen tile layout
        hdTile->ScreenX = cycle;
        hdTile->ScreenY = scanline;
        hdTile->X = cycle;
        hdTile->Y = scanline;
        hdTile->Width = 8 * _options.Scale;
        hdTile->Height = 8 * _options.Scale;
        hdTile->Brightness = 255;
        hdTile->DefaultTile = false;
        hdTile->ForceDisableCache = false;
        hdTile->VramBankId = GetVramBankId(tileAddr);
        hdTile->IsSprite = isSprite;
        // Store VRAM address in TileIndex for reference (no separate VramAddr field)
        hdTile->TileIndex = tileAddr;
        
        // Extract palette information from tile attributes
        // For SMS: background tiles use palette index from upper bits, sprites use palette 1
        if(isSprite) {
            // SMS sprites always use high palette (palette index 1)
            hdTile->PaletteIndex = 1;
            if(_options.DebugMode) {
                MessageManager::Log("[SMS HD Pack] SPRITE tile at addr=0x" + HexUtilities::ToHex(tileAddr, true) + 
                                   ", PaletteIndex=1");
            }
        } else {
            // Background tiles: extract palette index from upper bits of PaletteColors
            hdTile->PaletteIndex = (tile.PaletteColors >> 16) & 0xFF;
            if(_options.DebugMode) {
                MessageManager::Log("[SMS HD Pack] BACKGROUND tile at addr=0x" + HexUtilities::ToHex(tileAddr, true) + 
                                   ", PaletteIndex=" + std::to_string(hdTile->PaletteIndex));
            }
        }
        
        // Log tile information if debug mode is enabled
        if(_options.VerboseLogging) {
            std::stringstream ss;
            ss << "[SMS HD Pack] Processing " << (isSprite ? "sprite" : "background") << " tile at (" 
               << cycle << "," << scanline << "), VRAM addr: 0x" << std::hex << tileAddr
               << ", palette: " << (int)hdTile->PaletteIndex
               << ", bank hash: 0x" << std::hex << bankHash;
            MessageManager::Log(ss.str());
        }
        
        // Initialize per-tile usage count for ordering/sorting (use combined canonical count)
        hdTile->UsageCount = _canonicalUsageCount[canonicalHash];
        _tilesByKey[tile] = hdTile;
        _tilesByCanonicalHash[canonicalHash] = hdTile;
        _hdData.Tiles.push_back(unique_ptr<HdPackTileInfoSms>(hdTile));
        
        // Log tile statistics periodically
        if(_hdData.Tiles.size() % 100 == 0 || _options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] Tile stats: " + 
                              std::to_string(_hdData.Tiles.size()) + " unique tiles (" + 
                              std::to_string(_uniqueSpriteCount) + " sprites, " + 
                              std::to_string(_uniqueBgCount) + " background)");
        }
        
        // Generate the HD tile image
        GenerateHdTile(hdTile);
        
        // Track sprite occurrence within the current frame for grouping
        if(isSprite) {
            NoteSpriteOccurrence(hdTile, cycle, scanline);
        }

        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] DEBUG: Tile processing completed successfully");
        }
    } else {
        // Update existing tile's usage for ordering
        if(existingTile->second) {
            // Use canonical usage count for stability across equivalent keys
            existingTile->second->UsageCount = _canonicalUsageCount[canonicalHash];
            // Register canonical map if missing (should normally be missing only once)
            _tilesByCanonicalHash[canonicalHash] = existingTile->second;
        }
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] DEBUG: Tile already exists, usage count: " + 
                              std::to_string(_canonicalUsageCount[canonicalHash]));
        }

        // Track sprite occurrence within the current frame for grouping
        if(isSprite && existingTile->second) {
            NoteSpriteOccurrence(existingTile->second, cycle, scanline);
        }
    }
}

void HdPackBuilderSms::AddTile(HdPackTileInfoSms* tile, uint32_t usageCount) {
    if(!tile) {
        MessageManager::Log("[SMS HD Pack] ERROR: Attempted to add null tile");
        return;
    }
    
    // Ensure the tile has HD data generated for visual comparison
    if(tile->HdTileData.empty()) {
        this->GenerateHdTile(tile);
    }
    
    // Build the key directly from raw tile data and normalized palette/sprite flags
    HdTileKeySms tileKey;
    tileKey.TileIndex = tile->TileIndex;
    tileKey.PaletteColors = tile->PaletteColors;
    tileKey.IsVramTile = true;
    tileKey.IsSprite = tile->IsSprite;
    memcpy(tileKey.TileData, tile->TileData, SmsHdPackConstants::SMS_TILE_DATA_SIZE);
    
    // Check if we already have this tile
    auto existingTile = _tilesByKey.find(tileKey);
    if(existingTile == _tilesByKey.end()) {
        // Create a unique_ptr for the tile and add it to the collection
        auto tilePtr = std::make_unique<HdPackTileInfoSms>(*tile);
        // Initialize per-tile usage count (used for ordering)
        tilePtr->UsageCount = usageCount;
        
        // Update usage count using the correct key type
        _tileUsageCount[tileKey] = usageCount;
        _tilesByKey[tileKey] = tilePtr.get();
        
        // Add to the main tile collection
        _hdData.Tiles.push_back(std::move(tilePtr));
        
        // Update unique counters
        if(tile->IsSprite) {
            _uniqueSpriteCount++;
        } else {
            _uniqueBgCount++;
        }
        
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] Added " + std::string(tile->IsSprite ? "sprite" : "background") + 
                               " tile, total tiles: " + std::to_string(_hdData.Tiles.size()));
        }
    } else {
        // Just update the usage count for existing tiles
        _tileUsageCount[tileKey] += usageCount;
        if(existingTile->second) {
            // Keep the per-tile usage in sync for ordering
            existingTile->second->UsageCount = _tileUsageCount[tileKey];
        }
        
        // Update the screen position if this is a better instance
        if(existingTile->second) {
            existingTile->second->ScreenX = tile->ScreenX;
            existingTile->second->ScreenY = tile->ScreenY;
        }
        
        // Clean up the duplicate tile
        delete tile;
    }
}

uint32_t HdPackBuilderSms::GetVramBankId(uint32_t tileAddr) {
    // SMS VDP VRAM is organized in 16KB regions, divide into 4KB banks for organization
    return tileAddr / SmsHdPackConstants::SMS_VRAM_BANK_SIZE;
}

uint32_t HdPackBuilderSms::ExtractPixelFromBitplanes(uint8_t plane0, uint8_t plane1, uint8_t plane2, uint8_t plane3, int pixelX) {
    // SMS tiles use a 2-bitplane format, not 4-bitplane
    // Each row has 4 bytes, but they represent 2 bitplanes with 2 bytes each
    // Based on VDP analysis: SMS uses 4-bit color (16 colors) with 2 bitplanes
    
    // Calculate the bit mask for this pixel (7-pixelX to flip the order)
    uint8_t bitMask = 1 << (7 - pixelX);
    
    // SMS format: only use first 2 planes for 4-bit color (16 colors)
    // plane0 and plane1 are the actual bitplanes
    // plane2 and plane3 might be unused or used differently
    uint8_t bit0 = (plane0 & bitMask) ? 1 : 0;
    uint8_t bit1 = (plane1 & bitMask) ? 1 : 0;
    uint8_t bit2 = (plane2 & bitMask) ? 1 : 0;
    uint8_t bit3 = (plane3 & bitMask) ? 1 : 0;
    
    // Combine all 4 bits to form the color index (SMS supports 16 colors)
    uint8_t colorIndex = (bit3 << 3) | (bit2 << 2) | (bit1 << 1) | bit0;
    
    return colorIndex;
}

bool HdPackBuilderSms::ValidateTileData(const HdPackTileInfoSms* tile) const {
    if(!tile) {
        MessageManager::Log("[SMS HD Pack] ERROR: Null tile pointer");
        return false;
    }
    
    // Validate tile data size (compile-time check, always true for correctly defined structs)
    static_assert(sizeof(HdPackTileInfoSms::TileData) == SmsHdPackConstants::SMS_TILE_DATA_SIZE, "TileData size mismatch");
    (void)tile; // Suppress unused warning after static_assert
    
    // Additional validation can be added here
    return true;
}

void HdPackBuilderSms::LogTileDebugInfo(const HdPackTileInfoSms* tile) const {
    if(!_options.DebugMode || !tile) return;
    
    std::stringstream ss;
    ss << "[SMS HD Pack] DEBUG: " << (tile->IsSprite ? "Sprite" : "Background") << " tile";
    ss << " at VRAM address 0x" << std::hex << std::setw(4) << std::setfill('0') << tile->TileIndex;
    MessageManager::Log(ss.str());
    
    if(_options.ShowTileInfo) {
        ss.str("");
        ss << "[SMS HD Pack] DEBUG: Raw tile data: ";
        for(int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)tile->TileData[i] << " ";
        }
        MessageManager::Log(ss.str());
    }
}

// Main function to generate HD tile data from SMS tile data.
// 
// This function coordinates the entire HD tile generation process:
// 1. Validates input and initializes the HD tile data buffer
// 2. Reads tile data from VRAM using helper function
// 3. Processes all pixels using modular pixel processing
// 4. Applies debug effects and verification if enabled
// 
// The function now uses a modular approach with helper functions for better
// maintainability and clarity. All tile data reading, pixel processing, and
// debug effects are handled by focused helper functions.
// 
// @param tile Pointer to the tile to generate HD data for
void HdPackBuilderSms::GenerateHdTile(HdPackTileInfoSms* tile) {
    // Validate input - VDP is optional if tile has captured palette data
    if(!tile) {
        MessageManager::Log("[SMS HD Pack] ERROR: Invalid tile");
        return;
    }
    if(!_vdp && !tile->CapturedPalette.IsValid()) {
        MessageManager::Log("[SMS HD Pack] ERROR: No VDP and no captured palette - cannot generate tile");
        return;
    }
    
    // NES parity: Skip regeneration if tile already has HD data (preserves edited tiles)
    if(!tile->HdTileData.empty()) {
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] Skipping HD generation - tile already has HD data (preserved from existing pack)");
        }
        return;
    }
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Generating HD tile at address 0x" + 
                           HexUtilities::ToHex(tile->TileIndex) + 
                           ", IsSprite: " + std::string(tile->IsSprite ? "true" : "false") + 
                           ", PaletteIndex: " + std::to_string(tile->PaletteIndex));
    }
    
    // Initialize HD tile data buffer
    int scale = _hdData.Scale;
    int hdScale = (scale < 1) ? 1 : scale;
    int width = SmsHdPackConstants::SMS_TILE_SIZE * hdScale;
    int height = SmsHdPackConstants::SMS_TILE_SIZE * hdScale;
    
    // Use the already-captured tile data from the key (captured during VDP rendering)
    // DO NOT read from current VRAM - it may have changed since the tile was captured
    uint8_t tileData[SmsHdPackConstants::SMS_TILE_DATA_SIZE] = {0};
    memcpy(tileData, tile->TileData, SmsHdPackConstants::SMS_TILE_DATA_SIZE);
    
    // Only update palette as fallback when tile has no captured palette data,
    // or when the captured palette is all-zero (CRAM not yet initialized at capture time).
    // All-zero palette produces all-black tiles which is never correct for a visible tile.
    if((!tile->CapturedPalette.IsValid() || tile->CapturedPalette.IsAllZero()) && _vdp) {
        UpdatePalette();
    }
    
    // Use tile's own mode flag instead of querying VDP (which may be null after detach)
    bool isSg1000Mode = tile->IsSg1000Mode;
    
    // Step 1: Generate the original 8x8 tile with colors
    std::vector<uint32_t> originalTile(8 * 8, 0);
    
    if(isSg1000Mode) {
        // SG-1000 / TMS9918 mode: 1 bit per pixel
        // TileData[0..7] = pattern, CapturedPalette.Data[0..7] = per-row colors (BG only)
        // For sprites: PaletteColors = sprite color index (0 = transparent)
        
        // Diagnostic: log first few SG-1000 tile generations
        static int _sgGenCount = 0;
        if(++_sgGenCount <= 10) {
            std::string palData;
            for(int i = 0; i < 8; i++) palData += HexUtilities::ToHex(tile->CapturedPalette.Data[i]) + " ";
            std::string patData;
            for(int i = 0; i < 8; i++) patData += HexUtilities::ToHex(tileData[i]) + " ";
            MessageManager::Log("[SG GenTile #" + std::to_string(_sgGenCount) + "] " +
                (tile->IsSprite ? "SPR" : "BG") +
                " palValid=" + std::to_string(tile->CapturedPalette.IsValid()) +
                " palEntries=" + std::to_string(tile->CapturedPalette.EntryCount) +
                " palData=" + palData +
                " pattern=" + patData +
                " palColors=0x" + HexUtilities::ToHex(tile->PaletteColors) +
                " vdp=" + std::to_string((uint64_t)_vdp));
        }
        
        // Helper: convert SG-1000 color index (0-15) to 32-bit ARGB
        // SG-1000/TMS9918 uses a fixed 16-color palette - hardcode it to avoid null VDP issues
        // Standard TMS9918 palette (RGB values)
        static const uint32_t sg1000Palette[16] = {
            0x00000000,  // 0: Transparent
            0xFF000000,  // 1: Black
            0xFF21C842,  // 2: Medium Green
            0xFF5EDC78,  // 3: Light Green
            0xFF5455ED,  // 4: Dark Blue
            0xFF7D76FC,  // 5: Light Blue
            0xFFD4524D,  // 6: Dark Red
            0xFF42EBF5,  // 7: Cyan
            0xFFFC5554,  // 8: Medium Red
            0xFFFF7978,  // 9: Light Red
            0xFFD4C154,  // 10: Dark Yellow
            0xFFE6CE80,  // 11: Light Yellow
            0xFF21B03B,  // 12: Dark Green
            0xFFC95BBA,  // 13: Magenta
            0xFFCCCCCC,  // 14: Gray
            0xFFFFFFFF   // 15: White
        };
        auto sgToArgb = [](uint8_t colorIdx) -> uint32_t {
            if(colorIdx >= 16) return 0xFF000000;
            return sg1000Palette[colorIdx];
        };
        
        if(tile->IsSprite) {
            // SG-1000 sprites: single color, color 0 = fully transparent
            uint8_t spriteColor = (uint8_t)(tile->PaletteColors & 0x0F);
            uint32_t sprRgb = sgToArgb(spriteColor);
            
            for(int y = 0; y < 8; y++) {
                uint8_t patternByte = tileData[y];
                for(int x = 0; x < 8; x++) {
                    bool pixelSet = (patternByte >> (7 - x)) & 0x01;
                    // Set pixels use sprite color; clear pixels are fully transparent
                    originalTile[y * 8 + x] = pixelSet ? sprRgb : 0x00000000;
                }
            }
        } else {
            // SG-1000 BG tiles: per-row FG/BG colors from CapturedPalette
            // For BG tiles, color 0 should be opaque (backdrop color), not transparent
            auto sgToArgbBg = [](uint8_t colorIdx) -> uint32_t {
                if(colorIdx >= 16) return 0xFF000000;
                if(colorIdx == 0) return 0xFF000000; // Color 0 = opaque black for BG tiles
                return sg1000Palette[colorIdx];
            };
            
            for(int y = 0; y < 8; y++) {
                uint8_t colorByte = tile->CapturedPalette.Data[y]; // Per-row color byte
                uint8_t fgColor = (colorByte >> 4) & 0x0F;
                uint8_t bgColor = colorByte & 0x0F;
                uint32_t fgRgb = sgToArgbBg(fgColor);
                uint32_t bgRgb = sgToArgbBg(bgColor);
                
                uint8_t patternByte = tileData[y];
                for(int x = 0; x < 8; x++) {
                    bool pixelSet = (patternByte >> (7 - x)) & 0x01;
                    originalTile[y * 8 + x] = pixelSet ? fgRgb : bgRgb;
                }
            }
        }
    } else {
        // SMS Mode 4: 4 bits per pixel, 32 bytes per tile (4 bitplanes)
        static int _genDbgCount = 0;
        bool doLog = tile->IsSprite && _genDbgCount < 5;
        if(doLog) {
            _genDbgCount++;
            const auto& cp = tile->CapturedPalette;
            std::string palStr = "fmt=" + std::to_string((int)cp.Format) + " cnt=" + std::to_string(cp.EntryCount) + " bpe=" + std::to_string(cp.BytesPerEntry) + " data=";
            for(int i = 0; i < std::min((int)cp.EntryCount, 16); i++) palStr += HexUtilities::ToHex(cp.Data[i]) + " ";
            MessageManager::Log("[GenTile Spr#" + std::to_string(_genDbgCount) + "] palValid=" + std::to_string(cp.IsValid()) + " " + palStr);
            std::string tdStr = "tileData row0=";
            for(int i = 0; i < 4; i++) tdStr += HexUtilities::ToHex(tileData[i]) + " ";
            MessageManager::Log("[GenTile Spr#" + std::to_string(_genDbgCount) + "] " + tdStr);
        }
        for(int y = 0; y < 8; y++) {
            for(int x = 0; x < 8; x++) {
                int rowOffset = y * 4;
                uint8_t plane0 = tileData[rowOffset + 0];
                uint8_t plane1 = tileData[rowOffset + 1];
                uint8_t plane2 = tileData[rowOffset + 2];
                uint8_t plane3 = tileData[rowOffset + 3];
                uint8_t colorIndex = ExtractPixelFromBitplanes(plane0, plane1, plane2, plane3, x);
                uint32_t pixelColor = GetPixelColor(colorIndex, tile);
                if(doLog && y == 0) {
                    MessageManager::Log("[GenTile Spr#" + std::to_string(_genDbgCount) + "] px[" + std::to_string(x) + ",0] ci=" + std::to_string(colorIndex) + " color=0x" + HexUtilities::ToHex32(pixelColor));
                }
                originalTile[y * 8 + x] = pixelColor;
            }
        }
    }
    
    // Step 2: Apply filter to scale up
    tile->HdTileData.clear();
    tile->HdTileData.resize(width * height, 0);
    
    switch(_options.FilterType) {
        case ScaleFilterType::HQX:
            if(hdScale >= 2 && hdScale <= 4) {
                hqx(hdScale, originalTile.data(), tile->HdTileData.data(), 8, 8);
            } else {
                // Fallback to prescale for unsupported scales
                ApplyPrescale(originalTile, tile->HdTileData, hdScale);
            }
            break;
            
        case ScaleFilterType::xBRZ:
            if(hdScale >= 2 && hdScale <= 6) {
                xbrz::scale(hdScale, originalTile.data(), tile->HdTileData.data(), 8, 8, xbrz::ColorFormat::ARGB);
            } else {
                ApplyPrescale(originalTile, tile->HdTileData, hdScale);
            }
            break;
            
        case ScaleFilterType::Scale2x:
            if(hdScale >= 2 && hdScale <= 4) {
                ::scale(hdScale, tile->HdTileData.data(), 8 * sizeof(uint32_t) * hdScale, originalTile.data(), 8 * sizeof(uint32_t), 4, 8, 8);
            } else {
                ApplyPrescale(originalTile, tile->HdTileData, hdScale);
            }
            break;
            
        case ScaleFilterType::_2xSai:
            if(hdScale == 2) {
                twoxsai_generic_xrgb8888(8, 8, originalTile.data(), 8, tile->HdTileData.data(), 8 * hdScale);
            } else {
                ApplyPrescale(originalTile, tile->HdTileData, hdScale);
            }
            break;
            
        case ScaleFilterType::Super2xSai:
            if(hdScale == 2) {
                supertwoxsai_generic_xrgb8888(8, 8, originalTile.data(), 8, tile->HdTileData.data(), 8 * hdScale);
            } else {
                ApplyPrescale(originalTile, tile->HdTileData, hdScale);
            }
            break;
            
        case ScaleFilterType::SuperEagle:
            if(hdScale == 2) {
                supereagle_generic_xrgb8888(8, 8, originalTile.data(), 8, tile->HdTileData.data(), 8 * hdScale);
            } else {
                ApplyPrescale(originalTile, tile->HdTileData, hdScale);
            }
            break;
            
        case ScaleFilterType::Prescale:
        default:
            ApplyPrescale(originalTile, tile->HdTileData, hdScale);
            break;
    }
    
    // Apply debug effects if enabled
    ApplyDebugEffects(tile);
    
    // Verify palette usage if debugging is enabled
    if(_options.DebugMode || _options.VerboseLogging) {
        uint8_t* paletteRam = _vdp->GetPaletteRam();
        if(paletteRam) {
            VerifyPaletteUsage(tile, paletteRam);
        }
    }
    
    // NES parity: Analyze tile for transparency and blank detection
    tile->UpdateFlags();
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Tile generation completed for tile at address 0x" + 
                           HexUtilities::ToHex(tile->TileIndex));
    }
}

// Helper function to clean filename for filesystem use
string HdPackBuilderSms::CleanFilename(const string& filename) {
    string clean = filename;
    
    // Replace problematic characters with safe alternatives
    for(char& c : clean) {
        if(c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
            c = '_';
        } else if(c == ',' || c == ':' || c == ';') {
            c = '_';
        } else if(c == '"' || c == '\'' || c == '`') {
            c = '_';
        } else if(c == '<' || c == '>' || c == '|' || c == '?') {
            c = '_';
        } else if(c == '*') {
            c = '_';
        }
    }
    
    // Limit length to avoid path too long errors
    if(clean.length() > 100) {
        clean = clean.substr(0, 100);
    }
    
    // Remove trailing dots and spaces
    while(!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) {
        clean.pop_back();
    }
    
    return clean;
}

void HdPackBuilderSms::SaveDebugInfo() {
    // Create debug info file
    string debugPath = _saveFolder + "/debug_info.txt";
    ofstream debugFile(debugPath);
    
    if(debugFile) {
        debugFile << "SMS HD Pack Debug Information" << std::endl;
        debugFile << "===========================" << std::endl;
        debugFile << std::endl;
        
        debugFile << "ROM: " << _romName << std::endl;
        debugFile << "Scale: " << _options.Scale << std::endl;
        debugFile << std::endl;
        
        debugFile << "Tile Statistics:" << std::endl;
        debugFile << "---------------" << std::endl;
        debugFile << "Total sprite tiles processed: " << _totalSpriteCount << std::endl;
        debugFile << "Total background tiles processed: " << _totalBgCount << std::endl;
        debugFile << "Unique sprite tiles: " << _uniqueSpriteCount << std::endl;
        debugFile << "Unique background tiles: " << _uniqueBgCount << std::endl;
        debugFile << "Total unique tiles: " << _hdData.Tiles.size() << std::endl;
        debugFile << std::endl;
        
        // Add palette information
        debugFile << "Palette Information:" << std::endl;
        debugFile << "-------------------" << std::endl;
        
        // Sprite palette
        debugFile << "Sprite Palette:" << std::endl;
        for(int i = 0; i < 16; i++) {
            uint32_t color = _spritePalette[i];
            uint8_t r = color & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = (color >> 16) & 0xFF;
            
            debugFile << "Color " << std::dec << i << ": RGB(" 
                     << std::dec << (int)r << "," << (int)g << "," << (int)b << ") - "
                     << "#" << std::hex << std::setw(2) << std::setfill('0') << (int)r
                     << std::hex << std::setw(2) << std::setfill('0') << (int)g
                     << std::hex << std::setw(2) << std::setfill('0') << (int)b << std::endl;
        }
        debugFile << std::endl;
        
        // Background palette if different
        bool palettesAreDifferent = false;
        for(int i = 0; i < 16; i++) {
            if(_spritePalette[i] != _bgPalette[i]) {
                palettesAreDifferent = true;
                break;
            }
        }
        
        if(palettesAreDifferent) {
            debugFile << "Background Palette:" << std::endl;
            for(int i = 0; i < 16; i++) {
                uint32_t color = _bgPalette[i];
                uint8_t r = color & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = (color >> 16) & 0xFF;
                
                debugFile << "Color " << std::dec << i << ": RGB(" 
                         << std::dec << (int)r << "," << (int)g << "," << (int)b << ") - "
                         << "#" << std::hex << std::setw(2) << std::setfill('0') << (int)r
                         << std::hex << std::setw(2) << std::setfill('0') << (int)g
                         << std::hex << std::setw(2) << std::setfill('0') << (int)b << std::endl;
            }
        } else {
            debugFile << "Background Palette is identical to Sprite Palette" << std::endl;
        }
        
        debugFile.close();
        MessageManager::Log("[SMS HD Pack] Saved debug info: " + debugPath);
    } else {
        MessageManager::Log("[SMS HD Pack] Error: Failed to create debug info file: " + debugPath);
    }
}

void HdPackBuilderSms::DumpVramContents(const string& filename) {
    // Create a visual representation of the SMS VRAM contents
    // This is extremely useful for debugging tile extraction issues
    MessageManager::Log("[SMS HD Pack] Generating VRAM visualization: " + filename);
    
    // SMS VRAM is 16KB (0x4000 bytes)
    // We'll create a 256x256 image (each byte = 1 pixel)
    constexpr int width = 256;
    constexpr int height = 256;
    std::unique_ptr<uint32_t[]> imgData = std::make_unique<uint32_t[]>(width * height);
    
    // Get VRAM data from VDP
    uint8_t vramBuffer[0x4000] = {};
    // Read VRAM data using VDP ReadVram method
    for(uint16_t i = 0; i < 0x4000; i++) {
        vramBuffer[i] = _vdp->DebugReadVram(i);
    }
    
    // Fill the image with VRAM data visualization
    for(int y = 0; y < height; y++) {
        for(int x = 0; x < width; x++) {
            int vramAddr = y * width + x;
            if(vramAddr < 0x4000) {
                // Convert VRAM byte to grayscale color
                uint8_t value = vramBuffer[vramAddr];
                imgData[y * width + x] = 0xFF000000 | (value << 16) | (value << 8) | value;
                
                // Highlight different VRAM regions with color tints
                if(vramAddr < 0x3800) {
                    // Pattern Generator Table - add slight blue tint
                    imgData[y * width + x] = (imgData[y * width + x] & 0xFF7F7FFF) | 0x00404080;
                } else if(vramAddr < 0x3F00) {
                    // Name Table - add slight green tint
                    imgData[y * width + x] = (imgData[y * width + x] & 0xFF7FFF7F) | 0x00408040;
                } else {
                    // Sprite Attribute Table - add slight red tint
                    imgData[y * width + x] = (imgData[y * width + x] & 0xFFFF7F7F) | 0x00804040;
                }
            } else {
                // Out of VRAM bounds
                imgData[y * width + x] = 0xFF000000; // Black
            }
        }
    }
    
    // Draw grid lines to separate tile data (every 32 bytes = 4 tiles)
    for(int i = 0; i < width; i += 32) {
        for(int j = 0; j < height; j++) {
            imgData[j * width + i] = 0xFFFF0000; // Red vertical line
        }
    }
    
    for(int j = 0; j < height; j += 32) {
        for(int i = 0; i < width; i++) {
            imgData[j * width + i] = 0xFFFF0000; // Red horizontal line
        }
    }
    
    // Add special markers for important VRAM regions
    // Mark 0x3800 (Name Table start)
    int nameTableY = 0x3800 / width;
    int nameTableX = 0x3800 % width;
    for(int i = -2; i <= 2; i++) {
        for(int j = -2; j <= 2; j++) {
            int y = nameTableY + j;
            int x = nameTableX + i;
            if(y >= 0 && y < height && x >= 0 && x < width) {
                imgData[y * width + x] = 0xFF00FF00; // Green marker
            }
        }
    }
    
    // Mark 0x3F00 (Sprite Attribute Table start)
    int spriteTableY = 0x3F00 / width;
    int spriteTableX = 0x3F00 % width;
    for(int i = -2; i <= 2; i++) {
        for(int j = -2; j <= 2; j++) {
            int y = spriteTableY + j;
            int x = spriteTableX + i;
            if(y >= 0 && y < height && x >= 0 && x < width) {
                imgData[y * width + x] = 0xFFFF0000; // Red marker
            }
        }
    }
    
    // Save the image
    PNGHelper::WritePNG(filename, imgData.get(), width, height);
    MessageManager::Log("[SMS HD Pack] VRAM visualization saved: " + filename);
}

void HdPackBuilderSms::DumpPaletteVisualizer(const string& filename) {
    // Create a visual representation of the SMS palette
    // This helps debug palette extraction and color conversion issues
    MessageManager::Log("[SMS HD Pack] Generating palette visualization: " + filename);
    
    // Create a 256x128 image showing both sprite and background palettes
    constexpr int width = 256;
    constexpr int height = 128;
    std::unique_ptr<uint32_t[]> imgData = std::make_unique<uint32_t[]>(width * height);
    
    // Fill with black background
    for(int i = 0; i < width * height; i++) {
        imgData[i] = 0xFF000000;
    }
    
    // Draw sprite palette in top half
    const int colorWidth = 16; // Each color is 16x32 pixels
    const int colorHeight = 32;
    
    // Draw sprite palette (top half)
    for(int colorIndex = 0; colorIndex < 16; colorIndex++) {
        uint32_t color = _spritePalette[colorIndex];
        
        // Draw color swatch
        for(int y = 0; y < colorHeight; y++) {
            for(int x = 0; x < colorWidth; x++) {
                int pixelX = colorIndex * colorWidth + x;
                int pixelY = y;
                imgData[pixelY * width + pixelX] = color | 0xFF000000; // Ensure alpha is set
            }
        }
        
        // Draw color index number in contrasting color
        uint32_t textColor = ((color & 0x00808080) > 0) ? 0xFF000000 : 0xFFFFFFFF;
        
        // Simple digit rendering (very basic)
        int digitX = colorIndex * colorWidth + 4;
        int digitY = 4;
        int digit1 = colorIndex / 10;
        (void)digit1; // Suppress unused warning - digit rendering is simplified
        
        // Only draw first digit if not zero
        if(colorIndex >= 10) {
            // Draw a simple digit representation
            for(int i = 0; i < 3; i++) {
                for(int j = 0; j < 5; j++) {
                    imgData[(digitY + j) * width + (digitX + i)] = textColor;
                }
            }
        }
        
        // Draw second digit
        digitX += 4;
        for(int i = 0; i < 3; i++) {
            for(int j = 0; j < 5; j++) {
                imgData[(digitY + j) * width + (digitX + i)] = textColor;
            }
        }
    }
    
    // Draw background palette (bottom half)
    for(int colorIndex = 0; colorIndex < 16; colorIndex++) {
        uint32_t color = _bgPalette[colorIndex];
        
        // Draw color swatch
        for(int y = 0; y < colorHeight; y++) {
            for(int x = 0; x < colorWidth; x++) {
                int pixelX = colorIndex * colorWidth + x;
                int pixelY = colorHeight + y;
                imgData[pixelY * width + pixelX] = color | 0xFF000000; // Ensure alpha is set
            }
        }
        
        // Draw color index number in contrasting color
        uint32_t textColor = ((color & 0x00808080) > 0) ? 0xFF000000 : 0xFFFFFFFF;
        
        // Simple digit rendering (very basic)
        int digitX = colorIndex * colorWidth + 4;
        int digitY = colorHeight + 4;
        (void)colorIndex; // Suppress unused warning - digit rendering is simplified
        
        // Only draw first digit if not zero
        if(colorIndex >= 10) {
            // Draw a simple digit representation
            for(int i = 0; i < 3; i++) {
                for(int j = 0; j < 5; j++) {
                    imgData[(digitY + j) * width + (digitX + i)] = textColor;
                }
            }
        }
        
        // Draw second digit
        digitX += 4;
        for(int i = 0; i < 3; i++) {
            for(int j = 0; j < 5; j++) {
                imgData[(digitY + j) * width + (digitX + i)] = textColor;
            }
        }
    }
    
    // Draw dividing line
    for(int x = 0; x < width; x++) {
        imgData[colorHeight * width + x] = 0xFFFFFFFF;
    }
    
    // Save the image
    PNGHelper::WritePNG(filename, imgData.get(), width, height);
    MessageManager::Log("[SMS HD Pack] Palette visualization saved: " + filename);
}

void HdPackBuilderSms::TraceSprites() {
    // Special debug function for sprite rendering issues
    // This analyzes the game's sprite data and dumps detailed information
    MessageManager::Log("[SMS HD Pack] Starting sprite trace...");
    
    // Create a debug log file for sprite analysis
    string debugPath = _saveFolder + "/sprite_debug.txt";
    std::ofstream debugFile(debugPath);
    
    if(!debugFile.good()) {
        MessageManager::Log("[SMS HD Pack] Error: Failed to create sprite debug file");
        return;
    }
    
    debugFile << "SMS Sprite Debug Information" << std::endl;
    debugFile << "================================" << std::endl << std::endl;
    
    // Get VRAM data from VDP
    uint8_t vramBuffer[0x4000] = {};
    for(uint16_t i = 0; i < 0x4000; i++) {
        vramBuffer[i] = _vdp->DebugReadVram(i);
    }
    
    // Analyze Sprite Attribute Table (SAT) at 0x3F00-0x3F7F
    debugFile << "Sprite Attribute Table Analysis:" << std::endl;
    debugFile << "------------------------------" << std::endl;
    
    // SMS Sprite Attribute Table format:
    // Byte 0: Y position (0-255, 0=top of screen, Y=0xD0 means end of table)
    // Byte 1: X position (0-255, 0=left of screen)
    // Byte 2: Pattern index (0-255, points to a tile in VRAM)
    // Byte 3: Attributes (color, flip, etc.)
    
    for(int i = 0; i < 64; i++) { // SMS supports up to 64 sprites
        int satBase = 0x3F00 + (i * 4);
        uint8_t yPos = vramBuffer[satBase];
        
        // Y=0xD0 marks end of sprite table
        if(yPos == 0xD0) {
            debugFile << "End of sprite table marker at sprite " << i << std::endl;
            break;
        }
        
        uint8_t xPos = vramBuffer[satBase + 1];
        uint8_t patternIndex = vramBuffer[satBase + 2];
        uint8_t attributes = vramBuffer[satBase + 3];
        
        // Extract attribute information
        bool earlyClockBit = (attributes & 0x80) != 0;
        uint8_t paletteOffset = (attributes & 0x10) ? 16 : 0; // 0=first 16 colors, 16=second 16 colors
        bool vFlip = (attributes & 0x04) != 0;
        bool hFlip = (attributes & 0x02) != 0;
        uint8_t colorIndex = attributes & 0x0F; // Color index within palette
        
        // Calculate actual tile address in VRAM
        uint16_t tileAddr = patternIndex * 32; // Each tile is 32 bytes (8x8 pixels, 4bpp)
        
        debugFile << "Sprite " << i << ": ";
        debugFile << "Pos(" << (int)xPos << "," << (int)yPos << ") ";
        debugFile << "Pattern=" << (int)patternIndex << " ";
        debugFile << "TileAddr=0x" << std::hex << std::setw(4) << std::setfill('0') << (int)tileAddr << std::dec << " ";
        debugFile << "Attr=0x" << std::hex << std::setw(2) << std::setfill('0') << (int)attributes << std::dec << " ";
        debugFile << "(";
        if(hFlip) debugFile << "H-flip ";
        if(vFlip) debugFile << "V-flip ";
        if(earlyClockBit) debugFile << "Early ";
        debugFile << "Palette:" << (paletteOffset ? "1" : "0") << " ";
        debugFile << "Color:" << (int)colorIndex;
        debugFile << ")" << std::endl;
        
        // Dump the actual tile data
        debugFile << "  Tile data: ";
        for(int j = 0; j < 32; j++) {
            if(j % 8 == 0) debugFile << std::endl << "  ";
            debugFile << std::hex << std::setw(2) << std::setfill('0') << (int)vramBuffer[tileAddr + j] << " ";
        }
        debugFile << std::dec << std::endl;
        
        // Visualize the tile pattern (ASCII art)
        debugFile << "  Tile pattern:" << std::endl;
        for(int y = 0; y < 8; y++) {
            debugFile << "  ";
            for(int x = 0; x < 8; x++) {
                // SMS tiles are stored in 4 bitplanes
                // Calculate the pixel value at this position
                int pixelX = hFlip ? (7 - x) : x;
                int pixelY = vFlip ? (7 - y) : y;
                
                uint8_t bitplane0 = (vramBuffer[tileAddr + (pixelY * 4) + 0] >> (7 - pixelX)) & 0x01;
                uint8_t bitplane1 = (vramBuffer[tileAddr + (pixelY * 4) + 1] >> (7 - pixelX)) & 0x01;
                uint8_t bitplane2 = (vramBuffer[tileAddr + (pixelY * 4) + 2] >> (7 - pixelX)) & 0x01;
                uint8_t bitplane3 = (vramBuffer[tileAddr + (pixelY * 4) + 3] >> (7 - pixelX)) & 0x01;
                
                uint8_t pixelValue = (bitplane3 << 3) | (bitplane2 << 2) | (bitplane1 << 1) | bitplane0;
                
                // Use ASCII characters to represent different pixel values
                char pixelChar = ' ';
                if(pixelValue == 0) pixelChar = ' '; // Transparent
                else if(pixelValue < 4) pixelChar = '.'; // Light
                else if(pixelValue < 8) pixelChar = '+'; // Medium
                else if(pixelValue < 12) pixelChar = 'o'; // Dark
                else pixelChar = '#'; // Darkest
                
                debugFile << pixelChar << pixelChar; // Double width for better aspect ratio
            }
            debugFile << std::endl;
        }
        debugFile << std::endl;
    }
    
    debugFile.close();
    MessageManager::Log("[SMS HD Pack] Sprite trace saved to: " + debugPath);
    
    // Also generate a visual representation of the sprite tiles
    string spritesImagePath = _saveFolder + "/sprite_visualization.png";
    
    // Create a 256x256 image for sprite visualization
    constexpr int width = 256;
    constexpr int height = 256;
    std::unique_ptr<uint32_t[]> imgData = std::make_unique<uint32_t[]>(width * height);
    
    // Fill with black background
    for(int i = 0; i < width * height; i++) {
        imgData[i] = 0xFF000000;
    }
    
    // Draw each sprite tile at its position
    for(int i = 0; i < 64; i++) {
        int satBase = 0x3F00 + (i * 4);
        uint8_t yPos = vramBuffer[satBase];
        
        // Y=0xD0 marks end of sprite table
        if(yPos == 0xD0) break;
        
        uint8_t xPos = vramBuffer[satBase + 1];
        uint8_t patternIndex = vramBuffer[satBase + 2];
        uint8_t attributes = vramBuffer[satBase + 3];
        
        // Extract attribute information
        (void)attributes; // paletteOffset not used in simplified rendering
        bool vFlip = (attributes & 0x04) != 0;
        bool hFlip = (attributes & 0x02) != 0;
        
        // Calculate actual tile address in VRAM
        uint16_t tileAddr = patternIndex * 32;
        
        // Draw the sprite at its position
        for(int y = 0; y < 8; y++) {
            for(int x = 0; x < 8; x++) {
                // Calculate the pixel value at this position
                int pixelX = hFlip ? (7 - x) : x;
                int pixelY = vFlip ? (7 - y) : y;
                
                uint8_t bitplane0 = (vramBuffer[tileAddr + (pixelY * 4) + 0] >> (7 - pixelX)) & 0x01;
                uint8_t bitplane1 = (vramBuffer[tileAddr + (pixelY * 4) + 1] >> (7 - pixelX)) & 0x01;
                uint8_t bitplane2 = (vramBuffer[tileAddr + (pixelY * 4) + 2] >> (7 - pixelX)) & 0x01;
                uint8_t bitplane3 = (vramBuffer[tileAddr + (pixelY * 4) + 3] >> (7 - pixelX)) & 0x01;
                
                uint8_t pixelValue = (bitplane3 << 3) | (bitplane2 << 2) | (bitplane1 << 1) | bitplane0;
                
                // Skip transparent pixels (color index 0)
                if(pixelValue == 0) continue;
                
                // Get the color from the sprite palette
                uint32_t color = _spritePalette[pixelValue];
                
                // Calculate position in the output image
                int outX = xPos + x;
                int outY = yPos + y;
                
                // Ensure we're within bounds
                if(outX >= 0 && outX < width && outY >= 0 && outY < height) {
                    imgData[outY * width + outX] = color | 0xFF000000;
                }
            }
        }
        
        // Draw a box around the sprite for visibility
        for(int bx = 0; bx < 8; bx++) {
            if(yPos >= 0 && yPos < height && xPos + bx >= 0 && xPos + bx < width) {
                imgData[yPos * width + (xPos + bx)] = 0xFFFF0000; // Red top edge
            }
            if(yPos + 7 >= 0 && yPos + 7 < height && xPos + bx >= 0 && xPos + bx < width) {
                imgData[(yPos + 7) * width + (xPos + bx)] = 0xFFFF0000; // Red bottom edge
            }
        }
        for(int by = 0; by < 8; by++) {
            if(yPos + by >= 0 && yPos + by < height && xPos >= 0 && xPos < width) {
                imgData[(yPos + by) * width + xPos] = 0xFFFF0000; // Red left edge
            }
            if(yPos + by >= 0 && yPos + by < height && xPos + 7 >= 0 && xPos + 7 < width) {
                imgData[(yPos + by) * width + (xPos + 7)] = 0xFFFF0000; // Red right edge
            }
        }
    }
    
    // Save the image
    PNGHelper::WritePNG(spritesImagePath, imgData.get(), width, height);
    MessageManager::Log("[SMS HD Pack] Sprite visualization saved to: " + spritesImagePath);
}

void HdPackBuilderSms::GenerateDebugOverlay(HdPackTileInfoSms* tile, bool isSprite) {
    // Add visual debug indicators to tile images to help identify issues
    if(!_options.HighlightSprites || !tile) {
        return;
    }
    
    // Get the tile's raw image data
    uint32_t* imgData = tile->HdTileData.data();
    uint32_t scale = _options.Scale;
    uint32_t width = 8 * scale;
    uint32_t height = 8 * scale;
    
    // Add visual indicators based on tile type
    if(isSprite) {
        // For sprites, add a red border
        for(uint32_t x = 0; x < width; x++) {
            // Top and bottom borders
            imgData[x] = 0xFFFF0000; // Red
            imgData[(height-1) * width + x] = 0xFFFF0000;
        }
        
        for(uint32_t y = 0; y < height; y++) {
            // Left and right borders
            imgData[y * width] = 0xFFFF0000;
            imgData[y * width + (width-1)] = 0xFFFF0000;
        }
        
        // Add "S" marker in top-left corner
        if(scale >= 4) {
            // Only add text if scale is large enough
            uint32_t textColor = 0xFFFFFF00; // Yellow
            
            // Simple "S" shape
            int offsetX = 2;
            int offsetY = 2;
            
            // Top horizontal line
            for(uint32_t x = 0; x < 3 && (offsetX + x < width); x++) {
                imgData[offsetY * width + (offsetX + x)] = textColor;
            }
            
            // Middle horizontal line
            for(uint32_t x = 0; x < 3 && (offsetX + x < width); x++) {
                imgData[(offsetY + 2) * width + (offsetX + x)] = textColor;
            }
            
            // Bottom horizontal line
            for(uint32_t x = 0; x < 3 && (offsetX + x < width); x++) {
                imgData[(offsetY + 4) * width + (offsetX + x)] = textColor;
            }
            
            // Top vertical line
            imgData[(offsetY + 1) * width + offsetX] = textColor;
            
            // Bottom vertical line
            imgData[(offsetY + 3) * width + (offsetX + 2)] = textColor;
        }
    } else {
        // For background tiles, add a green border
        for(uint32_t x = 0; x < width; x++) {
            // Top and bottom borders
            imgData[x] = 0xFF00FF00; // Green
            imgData[(height-1) * width + x] = 0xFF00FF00;
        }
        
        for(uint32_t y = 0; y < height; y++) {
            // Left and right borders
            imgData[y * width] = 0xFF00FF00;
            imgData[y * width + (width-1)] = 0xFF00FF00;
        }
        
        // Add "B" marker in top-left corner
        if(scale >= 4) {
            // Only add text if scale is large enough
            uint32_t textColor = 0xFFFFFF00; // Yellow
            
            // Simple "B" shape
            int offsetX = 2;
            int offsetY = 2;
            
            // Vertical line
            for(uint32_t y = 0; y < 5 && (offsetY + y < height); y++) {
                imgData[(offsetY + y) * width + offsetX] = textColor;
            }
            
            // Top horizontal line
            for(uint32_t x = 1; x < 3 && (offsetX + x < width); x++) {
                imgData[offsetY * width + (offsetX + x)] = textColor;
            }
            
            // Middle horizontal line
            for(uint32_t x = 1; x < 3 && (offsetX + x < width); x++) {
                imgData[(offsetY + 2) * width + (offsetX + x)] = textColor;
            }
            
            // Bottom horizontal line
            for(uint32_t x = 1; x < 3 && (offsetX + x < width); x++) {
                imgData[(offsetY + 4) * width + (offsetX + x)] = textColor;
            }
        }
    }
    
    // Add palette index indicator in bottom-right corner
    if(scale >= 4) {
        uint32_t paletteIndex = tile->PaletteColors & 0x0F;
        uint32_t textColor = 0xFFFFFFFF; // White
        
        int offsetX = width - 6;
        int offsetY = height - 6;
        
        // Draw palette index number (simple digit rendering)
        (void)paletteIndex; // Suppress unused warning - digit rendering is simplified
        
        // Only draw first digit if not zero
        if(paletteIndex >= 10) {
            // Draw a simple digit representation
            for(uint32_t i = 0; i < 3 && (offsetX + i < width); i++) {
                for(uint32_t j = 0; j < 5 && (offsetY + j < height); j++) {
                    imgData[(offsetY + j) * width + (offsetX + i)] = textColor;
                }
            }
            offsetX += 3;
        }
        
        // Draw second digit
        for(uint32_t i = 0; i < 3 && (offsetX + i < width); i++) {
            for(uint32_t j = 0; j < 5 && (offsetY + j < height); j++) {
                imgData[(offsetY + j) * width + (offsetX + i)] = textColor;
            }
        }
    }
}

void HdPackBuilderSms::DumpBackgroundTilesFromVram() {
    // Implementation based on SmsVdpTools.GetTilemap approach for correct SMS tile VRAM dumping
    MessageManager::Log("[SMS HD Pack] Dumping background tiles directly from VRAM");
    
    if(!_vdp || !_console) {
        MessageManager::Log("[SMS HD Pack] Error: VDP or console not available");
        return;
    }
    
    // Get VDP state to access pattern table and nametable addresses
    SmsVdpState state = _vdp->GetState();
    uint16_t patternTableBase = state.BgPatternTableAddress;
    uint16_t nametableBase = state.EffectiveNametableAddress;
    
    MessageManager::Log("[SMS HD Pack] Pattern table base address: 0x" + 
                       HexUtilities::ToHex(patternTableBase));
    MessageManager::Log("[SMS HD Pack] Nametable base address: 0x" + 
                       HexUtilities::ToHex(nametableBase));
    // Note: Mode is not directly accessible in SmsVdpState
    MessageManager::Log("[SMS HD Pack] Use Mode 4: " + std::string(state.UseMode4 ? "Yes" : "No"));
    
    // Create a 128x256 image (16x32 tiles of 8x8 pixels) to show all 512 tiles
    constexpr int gridWidth = 16;
    constexpr int gridHeight = 32; // Increased to 32 rows to show all 512 tiles
    constexpr int tileSize = 8;
    constexpr int pngWidth = gridWidth * tileSize;
    constexpr int pngHeight = gridHeight * tileSize;
    
    // Initialize buffer with dark background
    vector<uint32_t> pngBuffer(pngWidth * pngHeight, 0xFF303030);
    
    // DEBUGGING: Dump all 512 tiles directly from pattern table with different palette options
    // This ensures we get all font/text tiles regardless of nametable entries
    for(int tileIndex = 0; tileIndex < 512; tileIndex++) {
        // Calculate grid position - use first 256 slots for direct pattern table dump
        int gridX = tileIndex % gridWidth;
        int gridY = tileIndex / gridWidth;
        
        // Calculate tile address in VRAM
        uint16_t tileAddr = patternTableBase + (tileIndex * SmsHdPackConstants::SMS_TILE_DATA_SIZE);
        
        // DEBUGGING: Log tile address for the first few tiles
        if(tileIndex < 5) {
            MessageManager::Log("[SMS HD Pack] Tile " + std::to_string(tileIndex) + 
                               " VRAM address: 0x" + HexUtilities::ToHex(tileAddr));
        }
        
        // Process each pixel in the 8x8 tile
        for(int y = 0; y < tileSize; y++) {
            // Each row starts at y*4 in the tile data (4 bitplanes per row)
            int rowOffset = y * SmsHdPackConstants::SMS_BITPLANES;
            
            // Get the 4 bytes for this row (one byte per bitplane) using VDP's debug VRAM access
            uint8_t plane0 = _vdp->DebugReadVram(tileAddr + rowOffset + 0);
            uint8_t plane1 = _vdp->DebugReadVram(tileAddr + rowOffset + 1);
            uint8_t plane2 = _vdp->DebugReadVram(tileAddr + rowOffset + 2);
            uint8_t plane3 = _vdp->DebugReadVram(tileAddr + rowOffset + 3);
            
            // DEBUGGING: Log the raw bitplane data for the first few tiles
            if(tileIndex < 5 && y == 0) {
                MessageManager::Log("[SMS HD Pack] Tile " + std::to_string(tileIndex) + 
                                   " Row " + std::to_string(y) + 
                                   " Planes: " + HexUtilities::ToHex(plane0) + 
                                   ", " + HexUtilities::ToHex(plane1) + 
                                   ", " + HexUtilities::ToHex(plane2) + 
                                   ", " + HexUtilities::ToHex(plane3));
            }
            
            // Process each pixel in the current row
            for(int x = 0; x < tileSize; x++) {
                // Extract pixel color index using the correct formula
                uint8_t colorIndex = ExtractPixelFromBitplanes(plane0, plane1, plane2, plane3, x);
                
                // Use actual SMS palette for all tiles for accurate color representation
                uint32_t color;
                
                // For direct pattern table dump, make all colors visible including index 0
                // This helps with debugging tile boundaries and content
                if(colorIndex == 0) {
                    // Index 0 is typically transparent, but make it visible for debugging
                    color = 0xFF202020; // Dark gray instead of transparent
                } else {
                    // Use the actual SMS palette for all tiles
                    // For pattern table dump, we'll use the background palette
                    color = _bgPalette[colorIndex % 16];
                }
                
                // Calculate buffer position
                int bufferX = gridX * tileSize + x;
                int bufferY = gridY * tileSize + y;
                
                // Write pixel to buffer
                if(bufferX < pngWidth && bufferY < pngHeight) {
                    pngBuffer[bufferY * pngWidth + bufferX] = color;
                }
            }
        }
    }
    
    // Save the direct pattern table dump
    string directPngPath = _saveFolder + "/vram_pattern_table.png";
    if(PNGHelper::WritePNG(directPngPath, pngBuffer.data(), pngWidth, pngHeight)) {
        MessageManager::Log("[SMS HD Pack] Saved direct pattern table dump: " + directPngPath);
    } else {
        MessageManager::Log("[SMS HD Pack] Error: Failed to save direct pattern table dump");
    }
    
    // Now create a second image with tiles using nametable data for proper palette and attributes
    // Reset buffer
    std::fill(pngBuffer.begin(), pngBuffer.end(), 0xFF303030);
    
    // Dump up to 256 tiles from VRAM using nametable data for proper palette selection
    for(int tileIndex = 0; tileIndex < 256; tileIndex++) {
        // Calculate grid position
        int gridX = tileIndex % gridWidth;
        int gridY = tileIndex / gridWidth;
        
        // Get nametable entry for this tile
        uint16_t entryAddr = nametableBase + (tileIndex * 2);
        uint16_t ntData = (_vdp->DebugReadVram(entryAddr) | (_vdp->DebugReadVram(entryAddr + 1) << 8));
        
        // Extract tile attributes from nametable data
        uint8_t paletteOffset = ntData & 0x800 ? 0x10 : 0; // Palette selection (0=BG, 0x10=Sprite)
        bool hMirror = ntData & 0x200;                     // Horizontal mirroring
        bool vMirror = ntData & 0x400;                     // Vertical mirroring
        uint16_t actualTileIndex = ntData & 0x1FF;         // Actual tile index (9 bits)
        
        // Calculate tile address in VRAM
        uint16_t tileAddr = patternTableBase + (actualTileIndex * SmsHdPackConstants::SMS_TILE_DATA_SIZE);
        
        // Process each pixel in the 8x8 tile
        for(int y = 0; y < tileSize; y++) {
            // Apply vertical mirroring if needed
            uint8_t tileRow = vMirror ? (7 - y) : y;
            
            // Each row starts at y*4 in the tile data (4 bitplanes per row)
            int rowOffset = tileRow * SmsHdPackConstants::SMS_BITPLANES;
            
            // Get the 4 bytes for this row (one byte per bitplane) using VDP's debug VRAM access
            uint8_t plane0 = _vdp->DebugReadVram(tileAddr + rowOffset + 0);
            uint8_t plane1 = _vdp->DebugReadVram(tileAddr + rowOffset + 1);
            uint8_t plane2 = _vdp->DebugReadVram(tileAddr + rowOffset + 2);
            uint8_t plane3 = _vdp->DebugReadVram(tileAddr + rowOffset + 3);
            
            // Process each pixel in the current row
            for(int x = 0; x < tileSize; x++) {
                // Apply horizontal mirroring if needed
                uint8_t tileColumn = hMirror ? (7 - x) : x;
                
                // Extract pixel color index using the correct formula
                uint8_t colorIndex = ExtractPixelFromBitplanes(plane0, plane1, plane2, plane3, tileColumn);
                
                // Get the color from the appropriate palette
                uint32_t color;
                if(paletteOffset == 0) {
                    // Background palette
                    color = _bgPalette[colorIndex];
                } else {
                    // Sprite palette
                    color = _spritePalette[colorIndex];
                }
                
                // Calculate buffer position
                int bufferX = gridX * tileSize + x;
                int bufferY = gridY * tileSize + y;
                
                // Write pixel to buffer
                if(bufferX < pngWidth && bufferY < pngHeight) {
                    pngBuffer[bufferY * pngWidth + bufferX] = color;
                }
            }
        }
    }
    
    // Save the nametable-based PNG
    string pngPath = _saveFolder + "/vram_bg_tiles.png";
    if(PNGHelper::WritePNG(pngPath, pngBuffer.data(), pngWidth, pngHeight)) {
        MessageManager::Log("[SMS HD Pack] Saved VRAM background tiles: " + pngPath);
    } else {
        MessageManager::Log("[SMS HD Pack] Error: Failed to save VRAM background tiles");
    }
}

void HdPackBuilderSms::SaveDebugTileGrid() {
    // Create a grid of all tiles for visual inspection
    // This is helpful for debugging tile extraction
    
    // Also dump background tiles directly from VRAM for comparison
    DumpBackgroundTilesFromVram();
    
    // Separate tiles into sprite and background collections
    vector<HdPackTileInfoSms*> spriteTiles;
    vector<HdPackTileInfoSms*> bgTiles;
    
    for(const auto& tile : _hdData.Tiles) {
        if(tile->IsSprite) {
            spriteTiles.push_back(tile.get());
        } else {
            bgTiles.push_back(tile.get());
        }
    }
    
    // Create sprite tile grid
    if(!spriteTiles.empty()) {
        // Determine grid size (try to make it square-ish)
        int gridWidth = (int)ceil(sqrt(spriteTiles.size()));
        int gridHeight = (int)ceil((double)spriteTiles.size() / gridWidth);
        
        // Create PNG buffer
        int tileSize = 8 * _options.Scale;
        int pngWidth = gridWidth * tileSize;
        int pngHeight = gridHeight * tileSize;
        vector<uint32_t> pngBuffer(pngWidth * pngHeight, 0xFF303030); // Dark gray background
        
        // Draw tiles to grid
        for(size_t i = 0; i < spriteTiles.size(); i++) {
            int gridX = (int)(i % gridWidth);
            int gridY = (int)(i / gridWidth);
            
            // Draw tile
            HdPackTileInfoSms* tile = spriteTiles[i];
            for(int y = 0; y < tileSize; y++) {
                for(int x = 0; x < tileSize; x++) {
                    int bufferX = gridX * tileSize + x;
                    int bufferY = gridY * tileSize + y;
                    
                    if(bufferX < pngWidth && bufferY < pngHeight) {
                        pngBuffer[bufferY * pngWidth + bufferX] = tile->HdTileData[y * tileSize + x];
                    }
                }
            }
        }
        
        // Save PNG
        string pngPath = _saveFolder + "/debug_sprite_grid.png";
        if(PNGHelper::WritePNG(pngPath, pngBuffer.data(), pngWidth, pngHeight)) {
            MessageManager::Log("[SMS HD Pack] Saved sprite tile debug grid: " + pngPath);
        } else {
            MessageManager::Log("[SMS HD Pack] Error: Failed to save sprite tile debug grid: " + pngPath);
        }
    }
    
    // Create background tile grid
    if(!bgTiles.empty()) {
        // Determine grid size (try to make it square-ish)
        int gridWidth = 16;
        int gridHeight = (int)ceil((double)bgTiles.size() / gridWidth);
            
        // Create PNG buffer
        int tileSize = 8 * _options.Scale;
        int pngWidth = gridWidth * tileSize;
        int pngHeight = gridHeight * tileSize;
        vector<uint32_t> pngBuffer(pngWidth * pngHeight, 0xFF303030); // Dark gray background
            
        // Draw tiles to grid
        for(size_t i = 0; i < bgTiles.size(); i++) {
            int gridX = (int)(i % gridWidth);
            int gridY = (int)(i / gridWidth);
                
            // Draw tile
            HdPackTileInfoSms* tile = bgTiles[i];
            for(int y = 0; y < tileSize; y++) {
                for(int x = 0; x < tileSize; x++) {
                    int bufferX = gridX * tileSize + x;
                    int bufferY = gridY * tileSize + y;
                        
                    if(bufferX < pngWidth && bufferY < pngHeight) {
                        pngBuffer[bufferY * pngWidth + bufferX] = tile->HdTileData[y * tileSize + x];
                    }
                }
            }
        }
            
        
        // Save PNG
        string pngPath = _saveFolder + "/debug_bg_grid.png";
        if(PNGHelper::WritePNG(pngPath, pngBuffer.data(), pngWidth, pngHeight)) {
            MessageManager::Log("[SMS HD Pack] Saved background tile debug grid: " + pngPath);
        } else {
            MessageManager::Log("[SMS HD Pack] Error: Failed to save background tile debug grid: " + pngPath);
        }
    }
}

void HdPackBuilderSms::SaveHdPack()
{
    if(_hdPackSaved) {
        MessageManager::Log("[SMS HD Pack] HD pack already saved, skipping");
        return;
    }
    
    if(_hdData.Tiles.empty()) {
        MessageManager::Log("[SMS HD Pack] No tiles to save, skipping");
        return;
    }
    
    // Clear visual hash alternates map for fresh tracking
    _visualHashAlternates.clear();
        
    MessageManager::Log("[SMS HD Pack] Found " + std::to_string(_hdData.Tiles.size()) + " tiles to save");
    
    // Count tiles with/without HdTileData
    int withHd = 0, withoutHd = 0;
    for(const auto& t : _hdData.Tiles) {
        if(t->HdTileData.empty()) withoutHd++; else withHd++;
    }
    MessageManager::Log("[SMS HD Pack] Tiles with HdTileData: " + std::to_string(withHd) + ", without: " + std::to_string(withoutHd));
        
    // Create output directory if it doesn't exist
    FolderUtilities::CreateFolder(_saveFolder);
    {
        std::error_code ec;
        bool exists = std::filesystem::exists(std::filesystem::u8path(_saveFolder), ec);
        if(!exists) {
            MessageManager::Log("[SMS HD Pack] ERROR: Save folder does not exist and could not be created: " + _saveFolder);
        } else {
            HDLOG_TAG("Folders", "Confirmed save folder exists: " + _saveFolder);
        }
    }
    
    // Separate tiles into sprite and background collections
    vector<HdPackTileInfoSms*> spriteTiles;
    vector<HdPackTileInfoSms*> bgTiles;
    
    for(const auto& tile : _hdData.Tiles) {
        if(tile->IsSprite) {
            spriteTiles.push_back(tile.get());
        } else {
            bgTiles.push_back(tile.get());
        }
    }
    
    // Diagnostic: count tiles with all-zero tile data (solid backdrop color tiles)
    int allZeroBg = 0, allZeroSpr = 0, emptyHdBg = 0, emptyHdSpr = 0;
    for(const auto& tile : _hdData.Tiles) {
        bool allZero = true;
        for(int i = 0; i < 32; i++) { if(tile->TileData[i] != 0) { allZero = false; break; } }
        if(allZero) { if(tile->IsSprite) allZeroSpr++; else allZeroBg++; }
        if(tile->HdTileData.empty()) { if(tile->IsSprite) emptyHdSpr++; else emptyHdBg++; }
    }
    MessageManager::Log("[SMS HD Pack] Separated tiles: " + std::to_string(bgTiles.size()) + 
                       " background tiles, " + std::to_string(spriteTiles.size()) + " sprite tiles" +
                       " (allZeroBg=" + std::to_string(allZeroBg) + " allZeroSpr=" + std::to_string(allZeroSpr) +
                       " emptyHdBg=" + std::to_string(emptyHdBg) + " emptyHdSpr=" + std::to_string(emptyHdSpr) +
                       " fadedSkipped=" + std::to_string(_fadedTilesSkipped) + ")");

    // DEBUG: Check why sprites might be missing
    if(spriteTiles.empty()) {
        MessageManager::Log("[SMS HD Pack] WARNING: No sprite tiles found in _hdData.Tiles! Checking capture process...");
        MessageManager::Log("[SMS HD Pack] Total tiles captured: " + std::to_string(_hdData.Tiles.size()));
        // Scan _hdData to see if any marked as sprite exist
        int actualSprites = 0;
        for(const auto& t : _hdData.Tiles) {
            if(t->IsSprite) actualSprites++;
        }
        MessageManager::Log("[SMS HD Pack] Actual IsSprite=true count in _hdData: " + std::to_string(actualSprites));
    }

    // Optional: sort tiles by usage frequency (player sprites should surface first)
    if(_options.SortByUsageFrequency) {
        auto byStable = [this](HdPackTileInfoSms* a, HdPackTileInfoSms* b) {
            if(a && b) {
                uint64_t ha = GetCanonicalHash(a);
                uint64_t hb = GetCanonicalHash(b);
                uint32_t ua = 0, ub = 0;
                auto ita = _canonicalUsageCount.find(ha);
                if(ita != _canonicalUsageCount.end()) ua = ita->second;
                auto itb = _canonicalUsageCount.find(hb);
                if(itb != _canonicalUsageCount.end()) ub = itb->second;
                if(ua != ub) {
                    return ua > ub;
                }
                // Group backgrounds by VRAM bank for locality
                if(!a->IsSprite && !b->IsSprite && a->VramBankId != b->VramBankId) {
                    return a->VramBankId < b->VramBankId;
                }
                // Stable fallback: canonical hash order
                return ha < hb;
            }
            // Non-null comes before null
            return a != nullptr;
        };
        std::stable_sort(bgTiles.begin(), bgTiles.end(), byStable);
        std::stable_sort(spriteTiles.begin(), spriteTiles.end(), byStable);
    }
    
    // Optional: group related sprite tiles (metasprite clusters) contiguously
    if(_options.GroupRelatedSpriteTiles) {
        // Flush any pending occurrences into the co-occurrence map
        if(_lastScanline >= 0) {
            // Force a frame wrap to trigger flush
            ResetFrameIfNeeded(0);
        }
        spriteTiles = ApplySpriteGrouping(spriteTiles);
    }
    
    // Partition tiles into solid-color and detailed lists
    // Solid-color tiles (all opaque pixels same color) go into separate sheets for cleaner organization
    vector<HdPackTileInfoSms*> solidSpriteTiles, detailedSpriteTiles;
    vector<HdPackTileInfoSms*> solidBgTiles, detailedBgTiles;
    
    for(auto* t : spriteTiles) {
        if(t && t->SolidColor) solidSpriteTiles.push_back(t);
        else detailedSpriteTiles.push_back(t);
    }
    for(auto* t : bgTiles) {
        if(t && t->SolidColor) solidBgTiles.push_back(t);
        else detailedBgTiles.push_back(t);
    }
    
    MessageManager::Log("[SMS HD Pack] Tile breakdown: sprites=" + std::to_string(detailedSpriteTiles.size()) + 
        " detailed + " + std::to_string(solidSpriteTiles.size()) + " solid, bg=" + 
        std::to_string(detailedBgTiles.size()) + " detailed + " + std::to_string(solidBgTiles.size()) + " solid");

    // Helper lambda to save a list of tiles into numbered sheet PNGs
    auto saveTileSheets = [&](vector<HdPackTileInfoSms*>& tiles, const string& prefix, bool isSprite, vector<string>& sheets) {
        for(size_t i = 0; i < tiles.size(); i += 256) {
            size_t count = std::min((size_t)256, tiles.size() - i);
            std::vector<HdPackTileInfoSms*> sheetTiles;
            for(size_t j = 0; j < count; j++) {
                sheetTiles.push_back(tiles[i + j]);
            }
            std::stringstream ss;
            ss << prefix << std::setw(3) << std::setfill('0') << (i/256) << ".png";
            std::string sheetName = ss.str();
            SaveTileSheet(sheetTiles, _saveFolder, sheetName, isSprite);
            sheets.push_back(sheetName);
        }
    };

    // Process tiles in chunks of 256 (16x16 grid)
    vector<string> tileSheets;

    // Save detailed sprite tiles FIRST so player sprites appear early like NES dumps
    saveTileSheets(detailedSpriteTiles, "SPRITES_", true, tileSheets);
    
    // Save detailed background tiles
    saveTileSheets(detailedBgTiles, "BGTILES_", false, tileSheets);
    
    // Save solid-color tiles into separate sheets
    saveTileSheets(solidSpriteTiles, "SOLID_SPR_", true, tileSheets);
    saveTileSheets(solidBgTiles, "SOLID_BG_", false, tileSheets);
    
    // Create and write hires.txt manifest file (HDNes-style)
    MessageManager::Log("[SMS HD Pack] _sheetInfos has " + std::to_string(_sheetInfos.size()) + " entries before manifest write");
    string manifestPath = FolderUtilities::CombinePath(_saveFolder, "hires.txt");
    std::ofstream manifestFile(manifestPath);
    
    if(manifestFile.is_open()) {
        GenerateHdNesManifest(manifestFile);
        
        manifestFile.close();
        MessageManager::Log("[SMS HD Pack] HD Pack manifest (HDNes-style) created: " + manifestPath);
        // Validate manifest content and file list for correctness
        ValidateHdNesManifestFile(manifestPath);
        MessageManager::Log("[SMS HD Pack] Total tiles processed: " + std::to_string(_hdData.Tiles.size()));
        HDLOG_TAG("Manifest", "BG tiles: " + std::to_string(bgTiles.size()) +
            ", Sprite tiles: " + std::to_string(spriteTiles.size()));
    } else {
        MessageManager::Log("[SMS HD Pack] Error: Failed to create manifest file");
    }
    
    _hdPackSaved = true;
    MessageManager::Log("[SMS HD Pack] HD pack saved to " + _saveFolder);
}

void HdPackBuilderSms::DrawTile(HdPackTileInfoSms* tile, int tileNumber, uint32_t* pngBuffer, int pngWidth)
{
    if(!tile || tile->HdTileData.empty()) {
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] DrawTile: Skipping invalid tile at position " + std::to_string(tileNumber));
        }
        return;
    }
    
    int tileSize = 8 * _hdData.Scale;
    int tilesPerRow = pngWidth / tileSize;
    
    // Calculate position in the sheet
    int tileX = (tileNumber % tilesPerRow) * tileSize;
    int tileY = (tileNumber / tilesPerRow) * tileSize;
    
    // Verify we have enough data
    int expectedSize = tileSize * tileSize;
    if((int)tile->HdTileData.size() < expectedSize) {
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] DrawTile: Insufficient tile data - expected " + 
                               std::to_string(expectedSize) + ", got " + std::to_string(tile->HdTileData.size()));
        }
        return;
    }
    
    // Copy tile data to the buffer with bounds checking
    for(int y = 0; y < tileSize; y++) {
        for(int x = 0; x < tileSize; x++) {
            int srcIndex = y * tileSize + x;
            int dstIndex = (tileY + y) * pngWidth + (tileX + x);
            
            if(srcIndex < (int)tile->HdTileData.size() && 
               dstIndex >= 0 && 
               dstIndex < pngWidth * pngWidth) {
                pngBuffer[dstIndex] = tile->HdTileData[srcIndex];
            }
        }
    }
    
    // Optional: Draw 1px border around the tile (magenta color for visibility)
    // This helps visualize tile boundaries even at higher scales like 4x
    if(_options.DrawTileBorders) {
        constexpr uint32_t borderColor = 0xFFFF00FF; // Magenta (ARGB)
        int pngHeight = pngWidth; // Assuming square PNG
        
        // Top and bottom borders
        for(int x = 0; x < tileSize; x++) {
            int topIdx = tileY * pngWidth + (tileX + x);
            int botIdx = (tileY + tileSize - 1) * pngWidth + (tileX + x);
            if(topIdx >= 0 && topIdx < pngWidth * pngHeight) {
                pngBuffer[topIdx] = borderColor;
            }
            if(botIdx >= 0 && botIdx < pngWidth * pngHeight) {
                pngBuffer[botIdx] = borderColor;
            }
        }
        
        // Left and right borders
        for(int y = 0; y < tileSize; y++) {
            int leftIdx = (tileY + y) * pngWidth + tileX;
            int rightIdx = (tileY + y) * pngWidth + (tileX + tileSize - 1);
            if(leftIdx >= 0 && leftIdx < pngWidth * pngHeight) {
                pngBuffer[leftIdx] = borderColor;
            }
            if(rightIdx >= 0 && rightIdx < pngWidth * pngHeight) {
                pngBuffer[rightIdx] = borderColor;
            }
        }
    }
}

/**
 * Filters input tiles to remove empty/invalid tiles and eliminate visual duplicates.
 * 
 * This function performs two main operations:
 * 1. Filters out tiles that are empty, mostly transparent, or have insufficient content
 * 2. Removes visually identical tiles using RGB-based hashing to prevent duplicates
 * 
 * @param inputTiles Vector of tile pointers to filter
 * @param isSprite Whether these are sprite tiles (affects filtering criteria)
 * @return Vector of valid, unique tiles ready for sheet creation
 */
// Helper function to detect backdrop tiles (all-zero pattern data)
// These tiles render as solid backdrop/BG color and should be dumped even if they appear "empty" (all black)
static bool IsBackdropTile(HdPackTileInfoSms* tile) {
    if(!tile) return false;
    
    if(tile->IsSg1000Mode) {
        // SG-1000: check first 8 bytes (pattern data)
        // Bytes 8-15 contain per-row color data, so we only check pattern
        for(int i = 0; i < 8; i++) {
            if(tile->TileData[i] != 0) return false;
        }
        return true;
    } else {
        // SMS Mode 4: check all 32 bytes (4 bitplanes × 8 rows)
        for(int i = 0; i < 32; i++) {
            if(tile->TileData[i] != 0) return false;
        }
        return true;
    }
}

vector<HdPackTileInfoSms*> HdPackBuilderSms::FilterValidTiles(const vector<HdPackTileInfoSms*>& inputTiles, bool isSprite)
{
    vector<HdPackTileInfoSms*> validTiles;
    // Map visual hash -> index in validTiles (to track alternates)
    std::unordered_map<uint64_t, size_t> hashToIndex;
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Filtering " + std::to_string(inputTiles.size()) + " " + 
                           string(isSprite ? "sprite" : "background") + " tiles");
    }
    
    for(size_t i = 0; i < inputTiles.size(); i++) {
        HdPackTileInfoSms* tile = inputTiles[i];
        if(!tile) continue;
        
        // Ensure tile has HD data
        if(tile->HdTileData.empty()) {
            // Do NOT regenerate at save time - VRAM has likely changed since the frame was captured,
            // which would result in corrupt/incorrect graphics.
            continue;
        }
        
        // Filter out empty/black tiles
        bool isEmpty = true;
        int nonTransparentPixels = 0;
        
        for(uint32_t color : tile->HdTileData) {
            if((color & 0xFF000000) != 0) { // Not transparent
                nonTransparentPixels++;
                uint32_t rgb = color & 0xFFFFFF;
                if(rgb != 0x000000) {
                    isEmpty = false;
                }
            }
        }
        
        // Skip empty tiles
        // For sprites, we must be more permissive:
        // 1. Allow opaque black pixels (sprites often use black for outlines/shadows)
        // 2. Allow small tiles (particles/bullets might be < 3 pixels)
        if(isSprite) {
            if(nonTransparentPixels < 1) {
                continue;
            }
        } else {
            // Background tiles: check if this is a backdrop tile (all-zero pattern data)
            bool isBackdrop = IsBackdropTile(tile);
            
            if(isBackdrop) {
                // Backdrop tiles render as solid backdrop/BG color - allow them even if they appear "empty"
                // They must still have opaque pixels (backdrop color is opaque, not transparent)
                if(nonTransparentPixels < 1) {
                    continue;
                }
            } else {
                // Non-backdrop tiles: stricter filtering to avoid dumping empty/black space
                if(isEmpty || nonTransparentPixels < 3) {
                    continue;
                }
            }
        }
        
        // Compute visual hash including alpha for proper deduplication
        // Use FNV-1a hash for better distribution
        uint64_t visualHash = 14695981039346656037ULL; // FNV offset basis
        for(size_t j = 0; j < tile->HdTileData.size(); j++) {
            uint32_t pixel = tile->HdTileData[j];
            visualHash ^= (pixel & 0xFF);
            visualHash *= 1099511628211ULL; // FNV prime
            visualHash ^= ((pixel >> 8) & 0xFF);
            visualHash *= 1099511628211ULL;
            visualHash ^= ((pixel >> 16) & 0xFF);
            visualHash *= 1099511628211ULL;
            visualHash ^= ((pixel >> 24) & 0xFF);
            visualHash *= 1099511628211ULL;
        }
        
        // Also include sprite flag in hash to keep BG and sprite tiles separate
        visualHash ^= (isSprite ? 0xDEADBEEF : 0xCAFEBABE);
        visualHash *= 1099511628211ULL;
        
        // Check for duplicates - track alternates for manifest
        auto it = hashToIndex.find(visualHash);
        if(it == hashToIndex.end()) {
            // First tile with this visual hash
            hashToIndex[visualHash] = validTiles.size();
            validTiles.push_back(tile);
        } else {
            // Duplicate visual - store as alternate of the primary tile
            // The primary tile's AlternateTiles will be populated in CreateTileSheets
            // For now, store in a temporary map that CreateTileSheets can access
            _visualHashAlternates[visualHash].push_back(tile);
            if(_options.DebugMode) {
                MessageManager::Log("[SMS HD Pack] Found alternate tile for visual hash (different TileData/palette)");
            }
        }
    }
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Filtered " + std::to_string(inputTiles.size()) + " tiles down to " + 
                           std::to_string(validTiles.size()) + " unique tiles");
    }
    
    // NES parity: Sort tiles to group similar tiles together
    // Order: 1) Blank/solid color tiles first, 2) Then by dominant color for visual organization
    if(_options.GroupBlankTiles && validTiles.size() > 1) {
        std::stable_sort(validTiles.begin(), validTiles.end(), [](HdPackTileInfoSms* a, HdPackTileInfoSms* b) {
            // Blank tiles come first
            if(a->Blank != b->Blank) {
                return a->Blank;  // Blank tiles before non-blank
            }
            
            // For blank tiles, group by their solid color
            if(a->Blank && b->Blank) {
                if(!a->HdTileData.empty() && !b->HdTileData.empty()) {
                    return a->HdTileData[0] < b->HdTileData[0];
                }
            }
            
            // For non-blank tiles, compute average color for grouping similar tiles
            auto getAverageColor = [](HdPackTileInfoSms* tile) -> uint32_t {
                if(tile->HdTileData.empty()) return 0;
                uint64_t r = 0, g = 0, b = 0;
                int count = 0;
                for(uint32_t pixel : tile->HdTileData) {
                    if((pixel >> 24) > 0) {  // Non-transparent
                        r += (pixel >> 16) & 0xFF;
                        g += (pixel >> 8) & 0xFF;
                        b += pixel & 0xFF;
                        count++;
                    }
                }
                if(count == 0) return 0;
                return ((uint32_t)(r / count) << 16) | ((uint32_t)(g / count) << 8) | (uint32_t)(b / count);
            };
            
            return getAverageColor(a) < getAverageColor(b);
        });
        
        // Count blank tiles for logging
        int blankCount = 0;
        for(auto* tile : validTiles) {
            if(tile->Blank) blankCount++;
        }
        if(blankCount > 0) {
            MessageManager::Log("[SMS HD Pack] Grouped " + std::to_string(blankCount) + " solid color tiles at start of sheet");
        }
    }
    
    return validTiles;
}

/**
 * Draws a single tile to the PNG buffer at the specified grid position.
 * 
 * This function copies tile pixel data from the tile's HdTileData to the PNG buffer,
 * positioning it correctly within a 16x16 grid layout. Includes bounds checking
 * to prevent buffer overruns.
 * 
 * @param tile Pointer to the tile to draw (must have valid HdTileData)
 * @param gridX X position in the 16x16 grid (0-15)
 * @param gridY Y position in the 16x16 grid (0-15)
 * @param pngBuffer Target PNG buffer to draw into
 * @param pngWidth Width of the PNG buffer in pixels
 * @param tileSize Size of each tile in pixels (typically 8 * scale)
 */
void HdPackBuilderSms::DrawTileToBuffer(HdPackTileInfoSms* tile, int gridX, int gridY, uint32_t* pngBuffer, int pngWidth, int pngHeight, int tileSize)
{
    if(!tile || tile->HdTileData.empty() || !pngBuffer) {
        return;
    }
    
    int tileX = gridX * tileSize;
    int tileY = gridY * tileSize;
    
    // Copy tile data to buffer with bounds checking
    for(int y = 0; y < tileSize; y++) {
        for(int x = 0; x < tileSize; x++) {
            int srcIndex = y * tileSize + x;
            int dstIndex = (tileY + y) * pngWidth + (tileX + x);
            
            // Ensure writes stay within the PNG buffer
            if(srcIndex < (int)tile->HdTileData.size() && dstIndex >= 0 && 
               dstIndex < pngWidth * pngHeight) {
                pngBuffer[dstIndex] = tile->HdTileData[srcIndex];
            }
        }
    }
}

/**
 * Generates appropriate filename for tile sheets with numbering when multiple sheets exist.
 * 
 * For single sheets, returns the base filename unchanged. For multiple sheets,
 * inserts a sheet number before the file extension (e.g., "tiles.png" becomes "tiles_1.png").
 * 
 * @param baseFilename The base filename to modify
 * @param sheetIndex Zero-based index of the current sheet
 * @param totalSheets Total number of sheets being generated
 * @return Appropriately numbered filename for the sheet
 */
string HdPackBuilderSms::GenerateSheetFilename(const string& baseFilename, int sheetIndex, int totalSheets)
{
    if(totalSheets <= 1) {
        return baseFilename;
    }
    
    size_t dotPos = baseFilename.find_last_of('.');
    if(dotPos != string::npos) {
        return baseFilename.substr(0, dotPos) + "_" + std::to_string(sheetIndex + 1) + baseFilename.substr(dotPos);
    } else {
        return baseFilename + "_" + std::to_string(sheetIndex + 1);
    }
}

/**
 * Creates and saves 16x16 tile sheets from the provided tiles.
 * 
 * This function organizes tiles into 16x16 grids (256 tiles per sheet) to minimize
 * the number of output files while maximizing packing efficiency. Each sheet is
 * saved as a PNG file with appropriate numbering if multiple sheets are needed.
 * 
 * @param tiles Vector of valid tiles to organize into sheets
 * @param saveFolder Directory where PNG files should be saved
 * @param filename Base filename for the PNG files
 * @param isSprite Whether these are sprite tiles (affects logging messages)
 */
void HdPackBuilderSms::CreateTileSheets(const vector<HdPackTileInfoSms*>& tiles, const string& saveFolder, const string& filename, bool isSprite)
{
    const int TILES_PER_SHEET = 16 * 16;
    const int gridWidth = 16;
    int totalSheets = static_cast<int>((tiles.size() + TILES_PER_SHEET - 1) / TILES_PER_SHEET);
    
    for(int sheetIndex = 0; sheetIndex < totalSheets; sheetIndex++) {
        int startTile = sheetIndex * TILES_PER_SHEET;
        int endTile = std::min((int)tiles.size(), startTile + TILES_PER_SHEET);
        int tilesInSheet = endTile - startTile;
        
        // Calculate actual grid height needed (pack tightly like NES HD packs)
        int gridHeight = (tilesInSheet + gridWidth - 1) / gridWidth;
        if(gridHeight < 1) gridHeight = 1;
        
        int tileSize = 8 * _hdData.Scale;
        int pngWidth = gridWidth * tileSize;
        int pngHeight = gridHeight * tileSize;
        
        // Use magenta (0xFFFF00FF) as background - standard HD pack transparency color
        vector<uint32_t> pngBuffer(pngWidth * pngHeight, 0xFFFF00FF);
        
        // Prepare a new sheet info to track tile coordinates for manifest
        SheetInfo sheetInfo;
        sheetInfo.Filename = GenerateSheetFilename(filename, sheetIndex, totalSheets);
        sheetInfo.IsSprite = isSprite;

        // Draw tiles to buffer using helper function
        for(int i = 0; i < tilesInSheet; i++) {
            HdPackTileInfoSms* tile = tiles[startTile + i];
            if(!tile || tile->HdTileData.empty()) continue;
            
            int gridX = i % gridWidth;
            int gridY = i / gridWidth;
            
            DrawTileToBuffer(tile, gridX, gridY, pngBuffer.data(), pngWidth, pngHeight, tileSize);

            // Optional tile border overlay for debugging/visual alignment
            if(_options.DrawTileBorders) {
                // Cyan for BG, Magenta for sprites
                uint32_t borderColor = isSprite ? 0xFFFF00FFu : 0xFF00FFFFu;
                DrawTileBorder(gridX, gridY, pngBuffer.data(), pngWidth, tileSize, borderColor);
            }

            // Compute visual hash to find alternates
            uint64_t visualHash = 14695981039346656037ULL;
            for(size_t j = 0; j < tile->HdTileData.size(); j++) {
                uint32_t pixel = tile->HdTileData[j];
                visualHash ^= (pixel & 0xFF);
                visualHash *= 1099511628211ULL;
                visualHash ^= ((pixel >> 8) & 0xFF);
                visualHash *= 1099511628211ULL;
                visualHash ^= ((pixel >> 16) & 0xFF);
                visualHash *= 1099511628211ULL;
                visualHash ^= ((pixel >> 24) & 0xFF);
                visualHash *= 1099511628211ULL;
            }
            visualHash ^= (isSprite ? 0xDEADBEEF : 0xCAFEBABE);
            visualHash *= 1099511628211ULL;

            // Record mapping for manifest (pixel coordinates) with alternates
            SheetTileRef tr { tile, (uint16_t)(gridX * tileSize), (uint16_t)(gridY * tileSize), {} };
            
            // Look up alternates (tiles with same visual but different TileData/palette)
            auto altIt = _visualHashAlternates.find(visualHash);
            if(altIt != _visualHashAlternates.end()) {
                tr.AlternateTiles = altIt->second;
            }
            
            sheetInfo.Tiles.push_back(tr);
        }
        
        // Save PNG
        string sheetFilename = sheetInfo.Filename;
        string fullPath = FolderUtilities::CombinePath(saveFolder, sheetFilename);
        bool success = PNGHelper::WritePNG(fullPath, pngBuffer.data(), pngWidth, pngHeight, 32);
        
        if(success) {
            MessageManager::Log("[SMS HD Pack] Saved " + string(isSprite ? "sprite" : "background") + 
                               " sheet: " + sheetFilename + " (" + std::to_string(tilesInSheet) + " tiles)");
            _sheetInfos.push_back(std::move(sheetInfo));
        } else {
            MessageManager::Log("[SMS HD Pack] Failed to save " + string(isSprite ? "sprite" : "background") + " sheet: " + sheetFilename);
        }
    }
}

/**
 * Main function to save tiles as organized PNG sheets.
 * 
 * This function coordinates the entire tile sheet creation process:
 * 1. Validates input and performs early exit for empty tile sets
 * 2. Filters tiles to remove empty/invalid ones and eliminate duplicates
 * 3. Creates and saves 16x16 tile sheets with proper organization
 * 
 * The function uses a modular approach with helper functions for better
 * maintainability and clarity. All tiles are deduplicated using visual
 * content hashing to ensure no duplicate tiles appear in the output.
 * 
 * @param inputTiles Vector of tile pointers to process and save
 * @param saveFolder Directory where PNG files should be saved
 * @param filename Base filename for the PNG files
 * @param isSprite Whether these are sprite tiles (affects filtering and logging)
 */
void HdPackBuilderSms::SaveTileSheet(const vector<HdPackTileInfoSms*>& inputTiles, const string& saveFolder, const string& filename, bool isSprite)
{
    // Early exit for empty input
    if(inputTiles.empty()) {
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] No tiles to save for " + string(isSprite ? "sprite" : "background") + " sheet");
        }
        return;
    }
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Processing " + std::to_string(inputTiles.size()) + " " + 
                           string(isSprite ? "sprite" : "background") + " tiles");
    }
    
    // Step 1: Filter out empty/invalid tiles and remove duplicates
    vector<HdPackTileInfoSms*> validTiles = FilterValidTiles(inputTiles, isSprite);
    
    if(validTiles.empty()) {
        MessageManager::Log("[SMS HD Pack] No valid tiles found for " + string(isSprite ? "sprite" : "background") + " sheet");
        return;
    }
    
    // Step 2: Create and save 16x16 tile sheets
    size_t sheetsBefore = _sheetInfos.size();
    CreateTileSheets(validTiles, saveFolder, filename, isSprite);
    MessageManager::Log("[SMS HD Pack] SaveTileSheet: _sheetInfos grew from " + std::to_string(sheetsBefore) + " to " + std::to_string(_sheetInfos.size()) + " (" + filename + ")");
}

uint32_t HdPackBuilderSms::GetTileVisualHash(const HdPackTileInfoSms* tile) const
{
    if (!tile || tile->HdTileData.empty()) return 0;
    
    // Use a more comprehensive hash that captures the full visual appearance
    uint32_t hash = 0;
    
    // First, hash the raw tile data (the actual pixel pattern)
    for (int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        hash = hash * 31 + tile->TileData[i];
    }
    
    // Then, incorporate the palette colors used by this tile
    hash = hash * 31 + tile->PaletteColors;
    
    // Also consider sprite/background status as part of the hash
    hash = hash * 31 + (tile->IsSprite ? 1 : 0);
    
    // For truly unique visual appearance, sample key points in the HD tile data
    // This ensures visually identical tiles are grouped together
    const int samplePoints[] = {0, 1, 7, 8, 15, 16, 23, 24, 31, 32, 39, 40, 47, 48, 55, 56, 63};
    const int numSamples = sizeof(samplePoints) / sizeof(samplePoints[0]);
    
    for (int i = 0; i < numSamples; i++) {
        int idx = samplePoints[i];
        if (idx < tile->HdTileData.size()) {
            uint32_t color = tile->HdTileData[idx];
            // Use full color information for precise matching
            uint8_t r = (color & 0xFF);
            uint8_t g = ((color >> 8) & 0xFF);
            uint8_t b = ((color >> 16) & 0xFF);
            uint8_t a = ((color >> 24) & 0xFF);
            
            // Incorporate all color channels into the hash
            hash = hash * 31 + r;
            hash = hash * 31 + g;
            hash = hash * 31 + b;
            hash = hash * 31 + a;
        }
    }
    
    return hash;
}

// Canonical identity hash for tiles: pattern data + palette colors + sprite flag
// Uses FNV-1a 64-bit for stability across runs/platforms
uint64_t HdPackBuilderSms::GetCanonicalHash(const HdTileKeySms& key) const
{
    const uint64_t FNV_OFFSET = 1469598103934665603ULL;
    const uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t h = FNV_OFFSET;

    if(key.IsVramTile) {
        for(int i = 0; i < (int)SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
            h ^= (uint64_t)key.TileData[i];
            h *= FNV_PRIME;
        }
    } else {
        // Hash TileIndex bytes (little-endian) for non-VRAM tiles
        uint32_t idx = (uint32_t)key.TileIndex;
        for(int i = 0; i < 4; i++) {
            h ^= (uint64_t)((idx >> (i*8)) & 0xFF);
            h *= FNV_PRIME;
        }
    }

    // Include both PaletteIndex AND PaletteColors in hash for all modes.
    // This ensures tiles with same pattern but different palette colors get different hashes.
    // Without this, solid color tiles (e.g., blue sky vs yellow sky) would share the same
    // hash and PNG location, causing wrong colors at runtime.
    h ^= (uint64_t)key.PaletteIndex;
    h *= FNV_PRIME;
    h ^= (uint64_t)key.PaletteColors;
    h *= FNV_PRIME;

    // Sprite/background flag
    h ^= (uint64_t)(key.IsSprite ? 1 : 0);
    h *= FNV_PRIME;

    return h;
}

uint64_t HdPackBuilderSms::GetCanonicalHash(const HdPackTileInfoSms* tile) const
{
    if(!tile) return 0ULL;
    const uint64_t FNV_OFFSET = 1469598103934665603ULL;
    const uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t h = FNV_OFFSET;

    // Hash the 32-byte pattern
    for(int i = 0; i < (int)SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        h ^= (uint64_t)tile->TileData[i];
        h *= FNV_PRIME;
    }

    // Include both PaletteIndex AND PaletteColors in hash for all modes.
    // This ensures tiles with same pattern but different palette colors get different hashes.
    // Without this, solid color tiles (e.g., blue sky vs yellow sky) would share the same
    // hash and PNG location, causing wrong colors at runtime.
    h ^= (uint64_t)tile->PaletteIndex;
    h *= FNV_PRIME;
    h ^= (uint64_t)tile->PaletteColors;
    h *= FNV_PRIME;

    // Sprite/background flag
    h ^= (uint64_t)(tile->IsSprite ? 1 : 0);
    h *= FNV_PRIME;

    // Debug logging for sprite hash collisions
    if(_options.DebugMode && tile->IsSprite) {
        static std::map<uint64_t, std::string> spriteHashes;
        std::string tileDataHex;
        for(int i = 0; i < 32; i++) { // Full 32 bytes
            char buf[4];
            sprintf_s(buf, sizeof(buf), "%02X", tile->TileData[i]);
            tileDataHex += buf;
        }
        
        auto it = spriteHashes.find(h);
        if(it != spriteHashes.end()) {
            if(it->second != tileDataHex) {
                std::stringstream ss;
                ss << "[SMS HD Pack] HASH COLLISION DETECTED! Hash=0x" << std::hex << h
                   << " Existing=" << it->second << " New=" << tileDataHex 
                   << " Pal=0x" << std::hex << tile->PaletteColors;
                MessageManager::Log(ss.str());
            }
        } else {
            spriteHashes[h] = tileDataHex;
            std::stringstream ss;
            ss << "[SMS HD Pack] Recording sprite hash: 0x" << std::hex << h 
               << " Data=" << tileDataHex << " Pal=0x" << std::hex << tile->PaletteColors;
            MessageManager::Log(ss.str());
        }
    }

    return h;
}

// Helper function to read tile data from VRAM
// Reads 32 bytes of tile data and performs validation
bool HdPackBuilderSms::ReadTileDataFromVram(HdPackTileInfoSms* tile, uint8_t* tileData) {
    if (!tile || !_vdp) {
        MessageManager::Log("[SMS HD Pack] ERROR: Invalid tile or VDP not available");
        return false;
    }
    
    // TileIndex already contains the VRAM address, don't multiply again
    uint16_t tileAddr = tile->TileIndex;
    
    // DEBUG: Log tile address and first few bytes
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Reading tile from VRAM address 0x" + HexUtilities::ToHex(tileAddr));
    }
    
    // Read the tile data from VRAM
    for(int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        tileData[i] = _vdp->DebugReadVram(tileAddr + i);
    }
    
    // DEBUG: Log complete tile data to check for corruption patterns
    if(_options.DebugMode) {
        string hexData = "";
        for(int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
            if(i > 0 && i % 4 == 0) hexData += " | ";
            hexData += HexUtilities::ToHex(tileData[i]) + " ";
        }
        MessageManager::Log("[SMS HD Pack] Complete tile data: " + hexData);
        
        // Also decode the first row to see the pixel pattern
        string pixelRow = "Row 0 pixels: ";
        for(int x = 0; x < 8; x++) {
            uint8_t colorIndex = ExtractPixelFromBitplanes(tileData[0], tileData[1], tileData[2], tileData[3], x);
            pixelRow += std::to_string(colorIndex) + " ";
        }
        MessageManager::Log("[SMS HD Pack] " + pixelRow);
    }
    
    // Store the tile data in the tile object
    memcpy(tile->TileData, tileData, SmsHdPackConstants::SMS_TILE_DATA_SIZE);
    
    // Check if tile data is all zeros (which would result in blank tiles)
    bool allZeros = true;
    for(int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        if(tileData[i] != 0) {
            allZeros = false;
            break;
        }
    }
    
    if(allZeros && _options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] WARNING: Tile data is all zeros for tile at address 0x" + 
                           HexUtilities::ToHex(tileAddr) + ", this will result in a blank tile");
    }
    
    return true;
}

// Helper function to get pixel color from color index and palette
// SMS Mode 4: 4-bit color index (0-15), palette bank selected by PaletteIndex (0 or 1)
// Game Gear: Same as SMS but 12-bit RGB colors instead of 6-bit
// SG-1000: 1-bit pixels with FG/BG colors stored in PaletteColors
uint32_t HdPackBuilderSms::GetPixelColor(uint8_t colorIndex, HdPackTileInfoSms* tile) {
    // NES parity: Color index 0 is transparent ONLY for sprites (lets BG show through).
    // For BG tiles, color 0 is the actual background palette color and must be opaque.
    // This matches NES HdData.h ToRgb(): sprites use 0x00FFFFFF for color 0, BG tiles
    // render the palette color opaquely.
    if(colorIndex == 0 && tile->IsSprite) {
        return 0x00000000; // Transparent (alpha = 0)
    }

    // Use the captured palette from the tile if available
    const auto& captured = tile->CapturedPalette;
    
    if(captured.IsValid()) {
        // The captured palette contains 16 colors for the selected palette bank
        // For sprites, the captured palette is the sprite palette (colors 16-31 from CRAM)
        // For BG tiles, the captured palette is based on PaletteIndex (0 or 1)
        
        // Extract the palette-relative color index (0-15)
        uint8_t paletteColorIndex = colorIndex & 0x0F;
        
        if(paletteColorIndex < captured.EntryCount) {
            if(captured.Format == SmsHdPackSharedConstants::PaletteFormat::GameGear) {
                // Game Gear: 12-bit RGB (2 bytes per color)
                uint16_t ggColor = captured.Data[paletteColorIndex * 2] | (captured.Data[paletteColorIndex * 2 + 1] << 8);
                uint8_t r = (ggColor & 0x0F);
                uint8_t g = ((ggColor >> 4) & 0x0F);
                uint8_t b = ((ggColor >> 8) & 0x0F);
                r = (r << 4) | r;
                g = (g << 4) | g;
                b = (b << 4) | b;
                return 0xFF000000 | (r << 16) | (g << 8) | b;
            } else if(captured.Format == SmsHdPackSharedConstants::PaletteFormat::Sms) {
                // SMS: 6-bit RGB (1 byte per color)
                uint8_t smsColor = captured.Data[paletteColorIndex];
                return ConvertSmsColor(smsColor);
            } else if(captured.Format == SmsHdPackSharedConstants::PaletteFormat::Sg1000) {
                // SG-1000: Fixed 16-color palette
                // For SG-1000, CapturedPalette stores color indices, not RGB values
                // We need the VDP's fixed palette to convert
                const uint16_t* sgPalette = _vdp ? _vdp->GetSmsSgPalette() : nullptr;
                if(sgPalette && paletteColorIndex < 16) {
                    uint16_t rgb555 = sgPalette[paletteColorIndex];
                    uint8_t r = (rgb555 & 0x1F) << 3;
                    uint8_t g = ((rgb555 >> 5) & 0x1F) << 3;
                    uint8_t b = ((rgb555 >> 10) & 0x1F) << 3;
                    return 0xFF000000 | (r << 16) | (g << 8) | b;
                }
                // Fallback: return opaque black if VDP not available
                return 0xFF000000;
            }
        }
    }

    // Fallback to current VDP palette
    uint8_t palIdx = colorIndex & 0x0F;
    if(tile->IsSprite || tile->PaletteIndex == 1) {
        return _spritePalette[palIdx];
    }
    return _bgPalette[palIdx];
}

// Helper function to process all pixels in a tile
void HdPackBuilderSms::ProcessTilePixels(HdPackTileInfoSms* tile, const uint8_t* tileData) {
    int scale = _hdData.Scale;
    int width = SmsHdPackConstants::SMS_TILE_SIZE * scale;
    
    // SMS tiles use INTERLEAVED format:
    // 32 bytes = 8 rows × 4 bytes per row (one byte per bitplane)
    // Row 0: bytes 0-3 (bitplanes 0,1,2,3)
    // Row 1: bytes 4-7 (bitplanes 0,1,2,3)
    // ...
    // Row 7: bytes 28-31 (bitplanes 0,1,2,3)
    
    // Process each pixel in the original 8x8 tile
    for(int y = 0; y < SmsHdPackConstants::SMS_TILE_SIZE; y++) {
        for(int x = 0; x < SmsHdPackConstants::SMS_TILE_SIZE; x++) {
            // SMS INTERLEAVED format: each row has 4 consecutive bytes for 4 bitplanes
            int rowOffset = y * 4;
            uint8_t plane0 = tileData[rowOffset + 0];  // Bitplane 0 for row y
            uint8_t plane1 = tileData[rowOffset + 1];  // Bitplane 1 for row y
            uint8_t plane2 = tileData[rowOffset + 2];  // Bitplane 2 for row y
            uint8_t plane3 = tileData[rowOffset + 3];  // Bitplane 3 for row y
            
            // Extract pixel color index from bitplanes using SMS format
            uint8_t colorIndex = ExtractPixelFromBitplanes(plane0, plane1, plane2, plane3, x);
            
            // Get the color from the palette
            uint32_t color = GetPixelColor(colorIndex, tile);
            
            // Fill the scaled tile with the color
            for(int sy = 0; sy < scale; sy++) {
                for(int sx = 0; sx < scale; sx++) {
                    // Calculate the correct index in the HD tile data buffer
                    int destX = x * scale + sx;
                    int destY = y * scale + sy;
                    int hdIndex = destY * width + destX;
                    
                    // Ensure we're within bounds
                    if(hdIndex >= 0 && hdIndex < (int)tile->HdTileData.size()) {
                        tile->HdTileData[hdIndex] = color;
                    }
                }
            }
        }
    }
}

// Helper function to apply prescale (simple pixel duplication)
void HdPackBuilderSms::ApplyPrescale(const std::vector<uint32_t>& src, std::vector<uint32_t>& dst, int scale) {
    if(scale < 1) scale = 1;
    int srcWidth = 8;
    int dstWidth = 8 * scale;
    
    for(int y = 0; y < 8; y++) {
        for(int x = 0; x < 8; x++) {
            uint32_t color = src[y * srcWidth + x];
            for(int sy = 0; sy < scale; sy++) {
                for(int sx = 0; sx < scale; sx++) {
                    int dstIdx = (y * scale + sy) * dstWidth + (x * scale + sx);
                    if(dstIdx < (int)dst.size()) {
                        dst[dstIdx] = color;
                    }
                }
            }
        }
    }
}

// Helper function to apply debug effects to tiles
void HdPackBuilderSms::ApplyDebugEffects(HdPackTileInfoSms* tile) {
    if (!_options.DebugMode) {
        return;
    }
    
    int scale = _hdData.Scale;
    int width = SmsHdPackConstants::SMS_TILE_SIZE * scale;
    
    // Add debug grid pattern to help visualize pixel boundaries
    for(int y = 0; y < SmsHdPackConstants::SMS_TILE_SIZE; y++) {
        for(int x = 0; x < SmsHdPackConstants::SMS_TILE_SIZE; x++) {
            // Add edge highlighting for tile boundary visualization
            if(x == 0 || y == 0 || x == 7 || y == 7) {
                for(int sy = 0; sy < scale; sy++) {
                    for(int sx = 0; sx < scale; sx++) {
                        int destX = x * scale + sx;
                        int destY = y * scale + sy;
                        int hdIndex = destY * width + destX;
                        
                        if(hdIndex >= 0 && hdIndex < tile->HdTileData.size()) {
                            uint32_t color = tile->HdTileData[hdIndex];
                            
                            // Make edge pixels slightly darker for tile boundary visualization
                            uint8_t r = color & 0xFF;
                            uint8_t g = (color >> 8) & 0xFF;
                            uint8_t b = (color >> 16) & 0xFF;
                            
                            // Darken by 20%
                            r = (uint8_t)(r * 0.8f);
                            g = (uint8_t)(g * 0.8f);
                            b = (uint8_t)(b * 0.8f);
                            
                            tile->HdTileData[hdIndex] = 0xFF000000 | (b << 16) | (g << 8) | r;
                        }
                    }
                }
            }
        }
    }
    
    // Apply debug overlay if enabled
    if(_options.HighlightSprites) {
        GenerateDebugOverlay(tile, tile->IsSprite);
    }
}

// Helper function to validate VDP and set default palette if needed
bool HdPackBuilderSms::ValidateVdpAndSetDefaults() {
    if(!_vdp) {
        MessageManager::Log("[SMS HD Pack] Warning: VDP not available, using default palette");
        InitializeDefaultSmsPalette();
        return false;
    }
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Updating palette from VDP");
    }
    
    return true;
}

// Helper function to process Game Gear 12-bit RGB palette
void HdPackBuilderSms::ProcessGameGearPalette() {
    for(int i = 0; i < 16; i++) {
        // Background palette (entries 0-15)
        uint16_t ggColor = _vdp->GetInternalPaletteRam()[i];
        
        // Extract 4-bit RGB components from 12-bit GG color
        uint8_t r = (ggColor & 0x0F);
        uint8_t g = ((ggColor >> 4) & 0x0F);
        uint8_t b = ((ggColor >> 8) & 0x0F);
        
        // Convert 4-bit values (0-15) to 8-bit (0-255)
        r = (r << 4) | r;
        g = (g << 4) | g;
        b = (b << 4) | b;
        
        // Store as ARGB
        _bgPalette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        
        // Sprite palette (entries 16-31)
        ggColor = _vdp->GetInternalPaletteRam()[16 + i];
        
        // Extract and convert components
        r = (ggColor & 0x0F);
        g = ((ggColor >> 4) & 0x0F);
        b = ((ggColor >> 8) & 0x0F);
        
        r = (r << 4) | r;
        g = (g << 4) | g;
        b = (b << 4) | b;
        
        _spritePalette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

// Helper function to process SMS 6-bit RGB palette
void HdPackBuilderSms::ProcessSmsPalette() {
    uint8_t* paletteRam = _vdp->GetPaletteRam();
    if(!paletteRam) {
        MessageManager::Log("[SMS HD Pack] Warning: Could not access SMS palette RAM, using defaults");
        InitializeDefaultSmsPalette();
        return;
    }
    
    for(int i = 0; i < 16; i++) {
        // Background palette (entries 0-15)
        uint8_t smsColor = paletteRam[i];
        _bgPalette[i] = ConvertSmsColor(smsColor);
        
        // Sprite palette (entries 16-31)
        smsColor = paletteRam[16 + i];
        _spritePalette[i] = ConvertSmsColor(smsColor);
        
        // Log palette data for debugging if enabled
        if(_options.DebugMode && _options.ShowPaletteInfo) {
            std::stringstream ss;
            ss << "[SMS HD Pack] Palette [" << i << "] BG=0x" 
               << std::hex << std::setw(2) << std::setfill('0') << (int)paletteRam[i]
               << " -> 0x" << std::hex << std::setw(8) << std::setfill('0') << _bgPalette[i]
               << ", Sprite=0x" << std::hex << std::setw(2) << std::setfill('0') << (int)paletteRam[16+i]
               << " -> 0x" << std::hex << std::setw(8) << std::setfill('0') << _spritePalette[i];
            MessageManager::Log(ss.str());
        }
    }
    
    // Additional debug logging for background palette
    if(_options.DebugMode) {
        for(int i = 0; i < 16; i++) {
            MessageManager::Log("[SMS HD Pack] BG Palette [" + std::to_string(i) + "] = 0x" + 
                              HexUtilities::ToHex(paletteRam[i]) + " -> 0x" + 
                              HexUtilities::ToHex(_bgPalette[i]));
        }
    }
}

// Helper function to process SG-1000 15-bit RGB palette
void HdPackBuilderSms::ProcessSg1000Palette() {
    const uint16_t* sgPalette = _vdp->GetSmsSgPalette();
    if(!sgPalette) {
        MessageManager::Log("[SMS HD Pack] Warning: Could not access SG palette, using defaults");
        InitializeDefaultSmsPalette();
        return;
    }
    
    for(int i = 0; i < 16; i++) {
        // SG-1000 uses 15-bit RGB format (0BBBBBGGGGGRRRRR)
        uint16_t rgb555 = sgPalette[i];
        
        // Extract 5-bit components
        uint8_t r = rgb555 & 0x1F;
        uint8_t g = (rgb555 >> 5) & 0x1F;
        uint8_t b = (rgb555 >> 10) & 0x1F;
        
        // Convert 5-bit to 8-bit
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        
        // Store as ARGB
        uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
        _bgPalette[i] = color;
        _spritePalette[i] = color; // Same palette for both
    }
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Successfully loaded SG-1000 palette data");
    }
}

// Helper function to finalize transparency and logging
void HdPackBuilderSms::FinalizeTransparencyAndLogging() {
    // Only sprite color 0 is transparent (lets BG show through)
    // BG color 0 must remain opaque - it's the actual background color
    _spritePalette[0] = 0x00000000;
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Successfully loaded VDP palette data");
    }
    
    if(_options.DebugMode && _options.ShowPaletteInfo) {
        LogPaletteInfo();
    }
}

// Helper to convert tile data to hex string
static std::string TileDataToHex(const uint8_t* data, size_t len) {
    std::stringstream ss;
    for(size_t i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return ss.str();
}

// Generate HD pack tile entries for hires.txt manifest
// Creates proper tile mapping entries that the HD pack replacement system can use
// Format: <tile>tileData,paletteIndex,isSprite,x,y,filename</tile>
// Where tileData is 64-char hex string (32 bytes)
void HdPackBuilderSms::GenerateHdPackTileEntries(std::ofstream& manifestFile) {
    if(!manifestFile.is_open()) {
        MessageManager::Log("[SMS HD Pack] Error: Manifest file is not open");
        return;
    }
    
    // Write comment explaining the tile format
    // Format must match SmsHdPackApi::ParseManifest expectations:
    // <tile>imgIndex,tileData,paletteIndex,x,y</tile>
    manifestFile << "#Tile mapping format (SMS VRAM tiles use pattern data for matching):" << std::endl;
    manifestFile << "#<tile>imgIndex,tileData,paletteIndex,x,y</tile>" << std::endl;
    manifestFile << "#Where:" << std::endl;
    manifestFile << "#  imgIndex = index of the PNG file (from <img> declarations)" << std::endl;
    manifestFile << "#  tileData = 64-char hex string (32 bytes of SMS tile pattern data)" << std::endl;
    manifestFile << "#  paletteIndex = 0 for low palette, 1 for high palette" << std::endl;
    manifestFile << "#  x,y = position in tile sheet (in pixels)" << std::endl;
    manifestFile << std::endl;
    
    // Group tiles by type for organized output
    std::vector<HdPackTileInfoSms*> backgroundTiles;
    std::vector<HdPackTileInfoSms*> spriteTiles;
    
    for(auto& tile : _hdData.Tiles) {
        if(tile->IsSprite) {
            spriteTiles.push_back(tile.get());
        } else {
            backgroundTiles.push_back(tile.get());
        }
    }
    
    // Write background tiles section
    if(!backgroundTiles.empty()) {
        manifestFile << "#Background Tiles (" << backgroundTiles.size() << " tiles)" << std::endl;
        
        int tileCount = 0;
        for(auto* tile : backgroundTiles) {
            // Calculate position in tile sheet (16x16 grid)
            int sheetIndex = tileCount / 256; // 256 tiles per sheet
            int tileInSheet = tileCount % 256;
            int x = (tileInSheet % 16) * 8 * _hdData.Scale; // 16 tiles per row
            int y = (tileInSheet / 16) * 8 * _hdData.Scale; // 8 pixel tiles scaled
            
            // Generate filename
            std::stringstream filename;
            if(sheetIndex == 0) {
                filename << "background_tiles.png";
            } else {
                filename << "background_tiles_" << (sheetIndex + 1) << ".png";
            }
            
            // Write tile entry: imgIndex,tileData,paletteIndex,x,y
            // imgIndex is the sheet index (matches <img> declaration order)
            // For SG-1000, use PaletteColors (color byte) instead of PaletteIndex
            std::string tileDataHex = TileDataToHex(tile->TileData, 32);
            int palValue = tile->IsSg1000Mode ? (int)(tile->PaletteColors & 0xFF) : (int)tile->PaletteIndex;
            manifestFile << "<tile>" << sheetIndex << "," << tileDataHex << "," << palValue 
                        << "," << x << "," << y << "</tile>" << std::endl;
            
            tileCount++;
        }
        
        manifestFile << std::endl;
    }
    
    // Write sprite tiles section
    if(!spriteTiles.empty()) {
        manifestFile << "#Sprite Tiles (" << spriteTiles.size() << " tiles)" << std::endl;
        
        int tileCount = 0;
        // Sprite sheets come after background sheets in <img> declarations
        int bgSheetCount = (int)((backgroundTiles.size() + 255) / 256);
        
        for(auto* tile : spriteTiles) {
            // Calculate position in tile sheet (16x16 grid)
            int sheetIndex = tileCount / 256; // 256 tiles per sheet
            int tileInSheet = tileCount % 256;
            int x = (tileInSheet % 16) * 8 * _hdData.Scale; // 16 tiles per row
            int y = (tileInSheet / 16) * 8 * _hdData.Scale; // 8 pixel tiles scaled
            
            // imgIndex for sprites = bgSheetCount + sheetIndex
            int imgIndex = bgSheetCount + sheetIndex;
            
            // Write tile entry: imgIndex,tileData,paletteIndex,x,y
            // For SG-1000, use PaletteColors (color byte) instead of PaletteIndex
            std::string tileDataHex = TileDataToHex(tile->TileData, 32);
            int palValue = tile->IsSg1000Mode ? (int)(tile->PaletteColors & 0xFF) : (int)tile->PaletteIndex;
            manifestFile << "<tile>" << imgIndex << "," << tileDataHex << "," << palValue 
                        << "," << x << "," << y << "</tile>" << std::endl;
            
            tileCount++;
        }
        
        manifestFile << std::endl;
    }
    
    // Write summary
    manifestFile << "#Total tiles: " << _hdData.Tiles.size() << std::endl;
    manifestFile << "#Background: " << backgroundTiles.size() << ", Sprites: " << spriteTiles.size() << std::endl;
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Generated " + std::to_string(_hdData.Tiles.size()) + " tile entries in manifest");
        MessageManager::Log("[SMS HD Pack] Background tiles: " + std::to_string(backgroundTiles.size()));
        MessageManager::Log("[SMS HD Pack] Sprite tiles: " + std::to_string(spriteTiles.size()));
    }
}

void HdPackBuilderSms::VerifyPaletteUsage(HdPackTileInfoSms* tile, uint8_t* paletteRam) {
    if (!tile || !paletteRam) return;
    
    std::set<uint8_t> usedIndices;
    for (int y = 0; y < 8; y++) {
        int rowOffset = y * 4;
        uint8_t plane0 = tile->TileData[rowOffset + 0];
        uint8_t plane1 = tile->TileData[rowOffset + 1];
        uint8_t plane2 = tile->TileData[rowOffset + 2];
        uint8_t plane3 = tile->TileData[rowOffset + 3];
        
        for (int x = 0; x < 8; x++) {
            uint8_t colorIndex = ExtractPixelFromBitplanes(plane0, plane1, plane2, plane3, x);
            if (colorIndex > 0) {
                usedIndices.insert(colorIndex);
            }
        }
    }
    
    if (_options.DebugMode || _options.VerboseLogging) {
        std::stringstream ss;
        ss << "[SMS HD Pack] Tile 0x" << std::hex << tile->TileIndex 
           << " uses " << std::dec << usedIndices.size() << " unique colors: ";
        for (uint8_t idx : usedIndices) {
            ss << std::dec << (int)idx << " ";
        }
        MessageManager::Log(ss.str());
    }
}
