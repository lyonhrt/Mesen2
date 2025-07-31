#include "pch.h"
#include "SMS/HdPacks/HdPackBuilderSms.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsTypes.h"
#include "SMS/SmsVdp.h"
#include "SMS/SmsMemoryManager.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Shared/ColorUtilities.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <fstream>

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

HdPackBuilderSms::~HdPackBuilderSms() {
    MessageManager::Log("[SMS HD Pack] Destructor called - starting HD pack save process");
    MessageManager::Log("[SMS HD Pack] Current tile count: " + std::to_string(_hdData.Tiles.size()));
    MessageManager::Log("[SMS HD Pack] Save folder: " + _saveFolder);
    
    SaveHdPack();
    
    
    // Generate debug information if requested
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Generating debug information");
        SaveDebugInfo();
        if(_options.DumpTileGrid) {
            SaveDebugTileGrid();
        }
    }
    
    MessageManager::Log("[SMS HD Pack] Destructor completed");
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

void HdPackBuilderSms::UpdatePalette() {
    if(!_vdp) {
        MessageManager::Log("[SMS HD Pack] Warning: VDP not available for palette update");
        InitializeDefaultSmsPalette();
        return;
    }
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Updating palette from VDP");
    }
    
    // Get VDP state to access palette data
    SmsVdpState state = _vdp->GetState();
    
    if(state.UseMode4) {
        // SMS Mode 4 - get palette from VDP internal palette RAM
        SmsModel model = _console->GetModel();
        
        if(model == SmsModel::GameGear) {
            // Game Gear uses 12-bit RGB stored in internal palette RAM
            for(int i = 0; i < 16; i++) {
                // Background palette (entries 0-15)
                uint16_t ggColor = _vdp->GetInternalPaletteRam()[i];
                _bgPalette[i] = ColorUtilities::Rgb444ToArgb(ggColor);
                
                // Sprite palette (entries 16-31)
                ggColor = _vdp->GetInternalPaletteRam()[16 + i];
                _spritePalette[i] = ColorUtilities::Rgb444ToArgb(ggColor);
            }
        } else {
            // SMS uses 6-bit RGB stored in palette RAM
            uint8_t* paletteRam = _vdp->GetPaletteRam();
            if(paletteRam) {
                for(int i = 0; i < 16; i++) {
                    // Background palette (entries 0-15)
                    _bgPalette[i] = ColorUtilities::Rgb222ToArgb(paletteRam[i]);
                    
                    // Sprite palette (entries 16-31)
                    _spritePalette[i] = ColorUtilities::Rgb222ToArgb(paletteRam[16 + i]);
                }
            } else {
                MessageManager::Log("[SMS HD Pack] Warning: Could not access SMS palette RAM, using defaults");
                InitializeDefaultSmsPalette();
                return;
            }
        }
        
        // Always set transparent color (index 0) to fully transparent
        _bgPalette[0] = 0x00000000;
        _spritePalette[0] = 0x00000000;
        
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] Successfully loaded VDP palette data");
        }
    } else {
        // SG-1000 mode - use fixed palette
        const uint16_t* sgPalette = _vdp->GetSmsSgPalette();
        if(sgPalette) {
            for(int i = 0; i < 16; i++) {
                uint32_t color = ColorUtilities::Rgb555ToArgb(sgPalette[i]);
                _bgPalette[i] = color;
                _spritePalette[i] = color; // Same palette for both
            }
            
            if(_options.DebugMode) {
                MessageManager::Log("[SMS HD Pack] Successfully loaded SG-1000 palette data");
            }
        } else {
            MessageManager::Log("[SMS HD Pack] Warning: Could not access SG palette, using defaults");
            InitializeDefaultSmsPalette();
        }
    }
    
    if(_options.DebugMode && _options.ShowPaletteInfo) {
        LogPaletteInfo();
    }
}

uint32_t HdPackBuilderSms::ConvertSmsColor(uint8_t smsColor) {
    // SMS colors are stored in 6-bit format: --BBGGRR
    // Each component is 2 bits (0-3), need to expand to 8 bits (0-255)
    
    uint8_t r = (smsColor & 0x03);       // Red: bits 0-1
    uint8_t g = ((smsColor >> 2) & 0x03); // Green: bits 2-3  
    uint8_t b = ((smsColor >> 4) & 0x03); // Blue: bits 4-5
    
    // Expand 2-bit values (0-3) to full 8-bit range (0-255)
    // Use proper scaling: multiply by 85 to get 0, 85, 170, 255
    r = r * 85;
    g = g * 85;
    b = b * 85;
    
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
    
    // Always set transparent color (index 0) to fully transparent
    _bgPalette[0] = 0x00000000;
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
        MessageManager::Log("[SMS HD Pack] Background Palette is identical to Sprite Palette");
    }
}

void HdPackBuilderSms::ProcessTile(uint32_t cycle, uint32_t scanline, uint32_t tileAddr, HdTileKeySms& tile, 
                                   bool isSprite, uint32_t bankHash, bool hasBgSprite) {
    static uint32_t processCallCount = 0;
    processCallCount++;
    
    if(processCallCount % 100 == 1) { // Log every 100 calls to avoid spam
        MessageManager::Log("[SMS HD Pack] ProcessTile called " + std::to_string(processCallCount) + " times, current tiles: " + std::to_string(_hdData.Tiles.size()));
    }
    
    // Update palette data to ensure we have the latest colors
    UpdatePalette();
    
    // Add comprehensive debug logging with SMS-specific information
    static int totalProcessTileCalls = 0;
    totalProcessTileCalls++;
    
    // Track total and unique tile counts
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
    
    // Debug logging removed for GitHub release
    
    // Track usage count
    _tileUsageCount[tile]++;
    
    // Check if we already have this tile
    auto existingTile = _tilesByKey.find(tile);
    if(existingTile == _tilesByKey.end()) {
        // Track unique tile counts
        if(isSprite) {
            _uniqueSpriteCount++;
            // Debug logging removed for GitHub release
        } else {
            _uniqueBgCount++;
            // Debug logging removed for GitHub release
        }
        
        // Create new HD pack tile info
        HdPackTileInfoSms* hdTile = new HdPackTileInfoSms();
        *((HdTileKeySms*)hdTile) = tile;
        
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
        
        _tilesByKey[tile] = hdTile;
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
        
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] DEBUG: Tile processing completed successfully");
        }
    } else if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] DEBUG: Tile already exists, usage count: " + 
                          std::to_string(_tileUsageCount[tile]));
    }
}

void HdPackBuilderSms::AddTile(HdPackTileInfoSms* tile, uint32_t usageCount) {
    if(!tile) {
        MessageManager::Log("[SMS HD Pack] ERROR: Attempted to add null tile");
        return;
    }
    
    // Create a tile key for tracking usage
    HdTileKeySms tileKey;
    // Copy relevant data from tile to create the key
    tileKey.TileIndex = tile->TileIndex;
    tileKey.PaletteColors = tile->PaletteColors;
    
    // Create a unique_ptr for the tile and add it to the collection
    auto tilePtr = std::make_unique<HdPackTileInfoSms>(*tile);
    
    // Update usage count using the correct key type
    _tileUsageCount[tileKey] = usageCount;
    _tilesByKey[tileKey] = tilePtr.get();
    
    // Add to the main tile collection
    _hdData.Tiles.push_back(std::move(tilePtr));
    
    // Update counters
    if(tile->IsSprite) {
        _totalSpriteCount++;
        _uniqueSpriteCount++;
    } else {
        _totalBgCount++;
        _uniqueBgCount++;
    }
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Added " + std::string(tile->IsSprite ? "sprite" : "background") + 
                           " tile, total tiles: " + std::to_string(_hdData.Tiles.size()));
    }
}

uint32_t HdPackBuilderSms::GetVramBankId(uint32_t tileAddr) {
    // SMS VDP VRAM is organized in 16KB regions, divide into 4KB banks for organization
    return tileAddr / SmsHdPackConstants::SMS_VRAM_BANK_SIZE;
}

uint32_t HdPackBuilderSms::ExtractPixelFromBitplanes(uint8_t plane0, uint8_t plane1, uint8_t plane2, uint8_t plane3, int pixelX) {
    // SMS tiles are stored with the leftmost pixel in the most significant bit (bit 7)
    // This is the standard SMS 4bpp planar format extraction
    // Each bit from the 4 planes forms a color index for each pixel
    // Bit 7 of each plane corresponds to the leftmost pixel (x=0)
    // Bit 0 of each plane corresponds to the rightmost pixel (x=7)
    
    // Ensure pixelX is in valid range
    pixelX = std::min(std::max(pixelX, 0), 7);
    
    // FIXED: Correct SMS bit extraction - ensure proper bit order
    // The SMS uses a planar tile format where each bit from the 4 planes forms a color index
    // The bits are arranged from least significant (plane0) to most significant (plane3)
    uint8_t bit0 = (plane0 >> (7 - pixelX)) & 0x01;
    uint8_t bit1 = (plane1 >> (7 - pixelX)) & 0x01;
    uint8_t bit2 = (plane2 >> (7 - pixelX)) & 0x01;
    uint8_t bit3 = (plane3 >> (7 - pixelX)) & 0x01;
    
    // Combine the bits to form the color index
    uint8_t colorIndex = (bit3 << 3) | (bit2 << 2) | (bit1 << 1) | bit0;
    
    return colorIndex;
}

bool HdPackBuilderSms::ValidateTileData(const HdPackTileInfoSms* tile) const {
    if(!tile) {
        MessageManager::Log("[SMS HD Pack] ERROR: Null tile pointer");
        return false;
    }
    
    // Validate tile data size
    if(sizeof(tile->TileData) != SmsHdPackConstants::SMS_TILE_DATA_SIZE) {
        MessageManager::Log("[SMS HD Pack] ERROR: Invalid tile data size");
        return false;
    }
    
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

void HdPackBuilderSms::GenerateHdTile(HdPackTileInfoSms* tile) {
    // Validate input
    if(!ValidateTileData(tile)) {
        return;
    }
    
    int scale = _hdData.Scale;
    int width = SmsHdPackConstants::SMS_TILE_SIZE * scale;
    int height = SmsHdPackConstants::SMS_TILE_SIZE * scale;
    
    // Clear and resize the HD tile data buffer
    tile->HdTileData.clear();
    tile->HdTileData.resize(width * height, 0);
    
    // Get the tile data (32 bytes for 8x8 tile, 4bpp)
    uint8_t* tileData = tile->TileData;
    
    // Log debug information
    LogTileDebugInfo(tile);
    
    // DEBUGGING: Log the first few bytes of tile data
    std::stringstream tileDataSs;
    tileDataSs << "[SMS HD Pack] Tile data bytes: ";
    for(int i = 0; i < 8 && i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        tileDataSs << std::hex << std::setw(2) << std::setfill('0') << (int)tileData[i] << " ";
    }
    MessageManager::Log(tileDataSs.str());
    
    // SMS tiles are stored in 4 bitplanes, with each row taking 4 bytes
    // Each row has 4 bytes arranged as follows:
    // Byte 0: Bit plane 0 for the entire row (8 pixels)
    // Byte 1: Bit plane 1 for the entire row (8 pixels)
    // Byte 2: Bit plane 2 for the entire row (8 pixels)
    // Byte 3: Bit plane 3 for the entire row (8 pixels)
    
    // Process each pixel in the original 8x8 tile
    for(int y = 0; y < SmsHdPackConstants::SMS_TILE_SIZE; y++) {
        // Each row starts at y*4 in the tile data (4 bitplanes per row)
        int rowOffset = y * SmsHdPackConstants::SMS_BITPLANES;
        
        // Get the 4 bytes for this row (one byte per bitplane)
        uint8_t plane0 = tileData[rowOffset + 0];
        uint8_t plane1 = tileData[rowOffset + 1];
        uint8_t plane2 = tileData[rowOffset + 2];
        uint8_t plane3 = tileData[rowOffset + 3];
        
        // Debug log for bitplane data
        if(y < 2) { // Log first two rows for debugging
            std::stringstream ss;
            ss << "[SMS HD Pack] Tile row " << y << " bitplanes: "
               << std::hex << std::setw(2) << std::setfill('0') << (int)plane0 << " "
               << std::hex << std::setw(2) << std::setfill('0') << (int)plane1 << " "
               << std::hex << std::setw(2) << std::setfill('0') << (int)plane2 << " "
               << std::hex << std::setw(2) << std::setfill('0') << (int)plane3;
            MessageManager::Log(ss.str());
        }
        
        // Process each pixel in the current row
        for(int x = 0; x < SmsHdPackConstants::SMS_TILE_SIZE; x++) {
            // Extract pixel color index from bitplanes using our fixed extraction function
            uint8_t colorIndex = ExtractPixelFromBitplanes(plane0, plane1, plane2, plane3, x);
            
            // Debug log for pixel data
            if(y < 2 && x < 2) { // Log first few pixels for debugging
                std::stringstream ss;
                ss << "[SMS HD Pack] Pixel at (" << x << "," << y << ") has color index " 
                   << std::dec << (int)colorIndex;
                MessageManager::Log(ss.str());
            }
            
            // Get the color from the palette
            uint32_t color;
            
            // Use the actual SMS palette colors for accurate representation
            if(colorIndex == 0) {
                // Index 0 is typically transparent or background
                color = 0x00000000; // Transparent (alpha = 0)
            } else {
                // Use the actual SMS palette
                if(tile->IsSprite) {
                    // Use sprite palette (entries 16-31)
                    if(colorIndex < 16) {
                        color = _spritePalette[colorIndex];
                    } else {
                        color = 0xFF808080; // Gray (fallback)
                    }
                } else {
                    // Use background palette (entries 0-15)
                    if(colorIndex < 16) {
                        color = _bgPalette[colorIndex];
                    } else {
                        color = 0xFF808080; // Gray (fallback)
                    }
                }
                
                // Log palette usage for debugging
                if(y < 2 && x < 2) { // Log first few pixels for debugging
                    std::stringstream ss;
                    ss << "[SMS HD Pack] Using " << (tile->IsSprite ? "sprite" : "background") 
                       << " palette color " << std::dec << (int)colorIndex 
                       << " = 0x" << std::hex << std::setw(8) << std::setfill('0') << color;
                    MessageManager::Log(ss.str());
                }
            }
            
            // Add debug overlay if debug mode is enabled
            if(_options.DebugMode) {
                // Add a subtle grid pattern to help visualize pixel boundaries
                if(x == 0 || y == 0 || x == 7 || y == 7) {
                    // Make edge pixels slightly darker for tile boundary visualization
                    uint8_t r = color & 0xFF;
                    uint8_t g = (color >> 8) & 0xFF;
                    uint8_t b = (color >> 16) & 0xFF;
                    
                    // Darken by 20%
                    r = (uint8_t)(r * 0.8f);
                    g = (uint8_t)(g * 0.8f);
                    b = (uint8_t)(b * 0.8f);
                    
                    color = 0xFF000000 | (b << 16) | (g << 8) | r;
                }
            }
            
            // Fill the scaled tile with the color
            for(int sy = 0; sy < scale; sy++) {
                for(int sx = 0; sx < scale; sx++) {
                    // Calculate the correct index in the HD tile data buffer
                    int destX = x * scale + sx;
                    int destY = y * scale + sy;
                    int hdIndex = destY * width + destX;
                    
                    // Ensure we're within bounds
                    if(hdIndex >= 0 && hdIndex < tile->HdTileData.size()) {
                        tile->HdTileData[hdIndex] = color;
                    }
                }
            }
        }
    }
    
    // Log completion
    MessageManager::Log("[SMS HD Pack] Tile generation completed for tile at address 0x" + 
                       HexUtilities::ToHex(tile->TileIndex));
    
    // Apply debug overlay if enabled
    if(_options.HighlightSprites) {
        GenerateDebugOverlay(tile, tile->IsSprite);
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
    
    // Get VRAM data from memory manager
    SmsMemoryManager* memoryManager = _console->GetMemoryManager();
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
        int digit2 = colorIndex % 10;
        
        // Only draw first digit if not zero
        if(digit1 > 0) {
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
        int digit1 = colorIndex / 10;
        int digit2 = colorIndex % 10;
        
        // Only draw first digit if not zero
        if(digit1 > 0) {
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
    
    // Draw labels
    const int labelY1 = colorHeight - 10;
    const int labelY2 = height - 10;
    
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
    
    // Get VRAM data from memory manager
    SmsMemoryManager* memoryManager = _console->GetMemoryManager();
    uint8_t vramBuffer[0x4000] = {};
    // Read VRAM data using VDP ReadVram method
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
        uint8_t paletteOffset = (attributes & 0x10) ? 16 : 0;
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
        int digit1 = paletteIndex / 10;
        int digit2 = paletteIndex % 10;
        
        // Only draw first digit if not zero
        if(digit1 > 0) {
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
                
                // DEBUGGING: Use different palettes based on tile index to help identify issues
                uint32_t color;
                
                // Use a fixed palette for better visibility during debugging
                // This helps identify issues with the tile extraction logic
                if(colorIndex == 0) {
                    // Index 0 is typically transparent or background
                    color = 0xFF000000; // Black
                } else if(tileIndex < 256) {
                    // First 256 tiles - use a fixed debug palette for better visibility
                    switch(colorIndex) {
                        case 1: color = 0xFFFF0000; break; // Red
                        case 2: color = 0xFF00FF00; break; // Green
                        case 3: color = 0xFF0000FF; break; // Blue
                        case 4: color = 0xFFFFFF00; break; // Yellow
                        case 5: color = 0xFF00FFFF; break; // Cyan
                        case 6: color = 0xFFFF00FF; break; // Magenta
                        case 7: color = 0xFFFFFFFF; break; // White
                        case 8: color = 0xFFA0A0A0; break; // Light gray
                        case 9: color = 0xFF808080; break; // Medium gray
                        case 10: color = 0xFF404040; break; // Dark gray
                        case 11: color = 0xFFA00000; break; // Dark red
                        case 12: color = 0xFF00A000; break; // Dark green
                        case 13: color = 0xFF0000A0; break; // Dark blue
                        case 14: color = 0xFFA0A000; break; // Dark yellow
                        case 15: color = 0xFFA000A0; break; // Dark magenta
                        default: color = 0xFF808080; break; // Gray (fallback)
                    }
                } else {
                    // Second 256 tiles - use actual SMS palette for comparison
                    color = _bgPalette[colorIndex];
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
        int gridWidth = (int)ceil(sqrt(bgTiles.size()));
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

void HdPackBuilderSms::SaveHdPack() {
    MessageManager::Log("[SMS HD Pack] SaveHdPack() called");
    
    // Prevent multiple saves
    if(_hdPackSaved) {
        MessageManager::Log("[SMS HD Pack] HD Pack already saved, skipping duplicate save");
        return;
    }
    
    MessageManager::Log("[SMS HD Pack] Saving HD Pack...");
    
    if(_hdData.Tiles.empty()) {
        MessageManager::Log("[SMS HD Pack] No tiles to save - tile collection is empty");
        return;
    }
    
    MessageManager::Log("[SMS HD Pack] Found " + std::to_string(_hdData.Tiles.size()) + " tiles to save");
    
    // Create output directory if it doesn't exist
    std::filesystem::create_directories(_saveFolder);
    
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
    
    MessageManager::Log("[SMS HD Pack] Found " + std::to_string(spriteTiles.size()) + " sprite tiles and " + 
                      std::to_string(bgTiles.size()) + " background tiles");
    
    // Save sprite tiles
    vector<string> spriteTileSheets;
    int spriteSheetIndex = 0;
    for(size_t i = 0; i < spriteTiles.size(); i += 256) {
        // Process 256 tiles at a time (16x16 tile sheet)
        size_t count = std::min((size_t)256, spriteTiles.size() - i);
        vector<HdPackTileInfoSms*> sheetTiles(spriteTiles.begin() + i, spriteTiles.begin() + i + count);
        SaveTileSheet(sheetTiles, _saveFolder, spriteSheetIndex, true);
        
        // Generate sheet filename for manifest
        stringstream ss;
        ss << "tiles" << std::setw(3) << std::setfill('0') << (spriteSheetIndex + 1) << ".png";
        spriteTileSheets.push_back(ss.str());
        
        spriteSheetIndex++;
    }
    
    // Save background tiles
    vector<string> bgTileSheets;
    int bgSheetIndex = 0;
    for(size_t i = 0; i < bgTiles.size(); i += 256) {
        // Process 256 tiles at a time (16x16 tile sheet)
        size_t count = std::min((size_t)256, bgTiles.size() - i);
        vector<HdPackTileInfoSms*> sheetTiles(bgTiles.begin() + i, bgTiles.begin() + i + count);
        SaveTileSheet(sheetTiles, _saveFolder, bgSheetIndex, false);
        
        // Generate sheet filename for manifest
        stringstream ss;
        ss << "bgtiles" << std::setw(3) << std::setfill('0') << (bgSheetIndex + 1) << ".png";
        bgTileSheets.push_back(ss.str());
        
        bgSheetIndex++;
    }
    
    // Create manifest file
    try {
        // Normalize path separators for Windows
        string normalizedSaveFolder = _saveFolder;
        std::replace(normalizedSaveFolder.begin(), normalizedSaveFolder.end(), '/', '\\');
        
        string manifestPath = FolderUtilities::CombinePath(normalizedSaveFolder, "hires.txt");
        // Ensure manifest path also uses correct separators
        std::replace(manifestPath.begin(), manifestPath.end(), '/', '\\');
        
        MessageManager::Log("[SMS HD Pack] Creating manifest file: " + manifestPath);
        MessageManager::Log("[SMS HD Pack] Save folder (normalized): " + normalizedSaveFolder);
        
        // Ensure directory exists
        std::filesystem::create_directories(normalizedSaveFolder);
        
        // Verify directory was created
        if(!std::filesystem::exists(normalizedSaveFolder)) {
            MessageManager::Log("[SMS HD Pack] ERROR: Failed to create directory: " + normalizedSaveFolder);
            return;
        }
        
        ofstream manifestFile(manifestPath, std::ios::out | std::ios::trunc);
        
        if(manifestFile.is_open() && manifestFile.good()) {
            // Write header
            manifestFile << "#HD Pack for " << _romName << std::endl;
            manifestFile << "#Version " << _hdData.Version << std::endl;
            manifestFile << std::endl;
            
            // Write options
            manifestFile << "<scale>" << _options.Scale << "</scale>" << std::endl;
            manifestFile << std::endl;
            
            // Write ROM info
            manifestFile << "# ROM: " << _romName << std::endl;
            manifestFile << std::endl;
            
            // Write PNG image references for sprite tiles
            for(const auto& sheetName : spriteTileSheets) {
                manifestFile << "<img>" << sheetName << "</img>" << std::endl;
            }
            
            // Write PNG image references for background tiles
            for(const auto& sheetName : bgTileSheets) {
                manifestFile << "<img>" << sheetName << "</img>" << std::endl;
            }
            manifestFile << std::endl;
            
            // Write tile definitions
            manifestFile << "# Sprite Tiles" << std::endl;
            for(size_t i = 0; i < spriteTiles.size(); i++) {
                HdPackTileInfoSms* tile = spriteTiles[i];
                int sheetIndex = (int)(i / 256);
                int tileIndex = (int)(i % 256);
                
                // Write tile data in hex format
                manifestFile << "<tile>";
                for(int j = 0; j < 32; j++) {
                    manifestFile << std::hex << std::setw(2) << std::setfill('0') << (int)tile->TileData[j];
                }
                
                // Write palette colors, coordinates, and options
                manifestFile << "," << std::hex << std::setw(8) << std::setfill('0') << tile->PaletteColors;
                manifestFile << "," << spriteTileSheets[sheetIndex];
                manifestFile << "," << std::dec << tile->X << "," << tile->Y;
                manifestFile << ",1.0," << (tile->DefaultTile ? "Y" : "N");
                manifestFile << "</tile>" << std::endl;
            }
            
            manifestFile << std::endl << "# Background Tiles" << std::endl;
            for(size_t i = 0; i < bgTiles.size(); i++) {
                HdPackTileInfoSms* tile = bgTiles[i];
                int sheetIndex = (int)(i / 256);
                int tileIndex = (int)(i % 256);
                
                // Write tile data in hex format
                manifestFile << "<tile>";
                for(int j = 0; j < 32; j++) {
                    manifestFile << std::hex << std::setw(2) << std::setfill('0') << (int)tile->TileData[j];
                }
                
                // Write palette colors, coordinates, and options
                manifestFile << "," << std::hex << std::setw(8) << std::setfill('0') << tile->PaletteColors;
                manifestFile << "," << bgTileSheets[sheetIndex];
                manifestFile << "," << std::dec << tile->X << "," << tile->Y;
                manifestFile << ",1.0," << (tile->DefaultTile ? "Y" : "N");
                manifestFile << "</tile>" << std::endl;
            }
            
            // Ensure all data is written and flush the stream
            manifestFile.flush();
            manifestFile.close();
            
            // Verify the file was created successfully
            if(std::filesystem::exists(manifestPath)) {
                auto fileSize = std::filesystem::file_size(manifestPath);
                MessageManager::Log("[SMS HD Pack] Successfully created manifest file: " + manifestPath);
                MessageManager::Log("[SMS HD Pack] Manifest file size: " + std::to_string(fileSize) + " bytes");
                MessageManager::Log("[SMS HD Pack] Manifest contains " + std::to_string(_hdData.Tiles.size()) + " tiles");
                MessageManager::Log("[SMS HD Pack] Generated " + std::to_string(spriteTileSheets.size() + bgTileSheets.size()) + " PNG tile sheets");
                MessageManager::Log("[SMS HD Pack] SUCCESS: Complete HD pack created with hires.txt manifest!");
            } else {
                MessageManager::Log("[SMS HD Pack] ERROR: Manifest file was not created: " + manifestPath);
            }
        } else {
            MessageManager::Log("[SMS HD Pack] ERROR: Failed to open manifest file for writing: " + manifestPath);
            MessageManager::Log("[SMS HD Pack] File open state - is_open(): " + std::string(manifestFile.is_open() ? "true" : "false"));
            MessageManager::Log("[SMS HD Pack] File stream state - good(): " + std::string(manifestFile.good() ? "true" : "false"));
            MessageManager::Log("[SMS HD Pack] File stream state - fail(): " + std::string(manifestFile.fail() ? "true" : "false"));
            MessageManager::Log("[SMS HD Pack] File stream state - bad(): " + std::string(manifestFile.bad() ? "true" : "false"));
            MessageManager::Log("[SMS HD Pack] Check directory permissions and disk space");
            MessageManager::Log("[SMS HD Pack] Directory exists: " + std::string(std::filesystem::exists(normalizedSaveFolder) ? "true" : "false"));
        }
    } catch(const std::exception& e) {
        MessageManager::Log("[SMS HD Pack] ERROR: Exception during manifest creation: " + string(e.what()));
    }
    
    // Mark HD pack as saved to prevent duplicate saves
    _hdPackSaved = true;
    MessageManager::Log("[SMS HD Pack] SaveHdPack() completed");
}

void HdPackBuilderSms::DrawTile(HdPackTileInfoSms* tile, int tileNumber, uint32_t* pngBuffer, int pngWidth) {
    if(tile->HdTileData.empty()) {
        GenerateHdTile(tile);
    }
    
    int tileDimension = 8 * _hdData.Scale;
    int x = (tileNumber % 16) * tileDimension;
    int y = (tileNumber / 16) * tileDimension;
    
    // Update tile position for manifest
    tile->X = x;
    tile->Y = y;
    
    // Clear the destination area first to prevent artifacts
    for(int i = 0; i < tileDimension; i++) {
        for(int j = 0; j < tileDimension; j++) {
            pngBuffer[(y + i) * pngWidth + (x + j)] = 0xFF000000; // Black with alpha
        }
    }
    
    // Copy tile data to PNG buffer
    int tilePos = 0;
    for(int i = 0; i < tileDimension; i++) {
        for(int j = 0; j < tileDimension; j++) {
            if(tilePos < tile->HdTileData.size()) {
                pngBuffer[(y + i) * pngWidth + (x + j)] = tile->HdTileData[tilePos++];
            }
        }
    }
}

void HdPackBuilderSms::SaveTileSheet(const vector<HdPackTileInfoSms*>& tiles, const string& saveFolder, int sheetIndex, bool isSprite) {
    if(tiles.empty()) {
        return;
    }
    
    // Use a 16x16 grid layout for tiles (same as debug tile grid)
    int gridWidth = 16;
    int gridHeight = 16; // Maximum 256 tiles per sheet
    int tileSize = 8 * _hdData.Scale;
    int pngWidth = gridWidth * tileSize;
    int pngHeight = gridHeight * tileSize;
    
    // Initialize buffer with dark background for better visibility
    vector<uint32_t> pngBuffer(pngWidth * pngHeight, 0xFF303030); // Dark gray background (same as debug grid)
    
    // Draw tiles to the buffer using the same approach as SaveDebugTileGrid
    for(size_t i = 0; i < tiles.size() && i < 256; i++) {
        if(!tiles[i]) continue;
        
        // Calculate grid position
        int gridX = (int)(i % gridWidth);
        int gridY = (int)(i / gridWidth);
        
        // Update tile position for manifest
        tiles[i]->X = gridX * tileSize;
        tiles[i]->Y = gridY * tileSize;
        
        // Ensure tile data is generated
        if(tiles[i]->HdTileData.empty()) {
            this->GenerateHdTile(tiles[i]);
        }
        
        // Draw tile directly to buffer with proper positioning
        HdPackTileInfoSms* tile = tiles[i];
        
        // First clear the destination area to prevent artifacts
        for(int y = 0; y < tileSize; y++) {
            for(int x = 0; x < tileSize; x++) {
                int bufferX = gridX * tileSize + x;
                int bufferY = gridY * tileSize + y;
                
                if(bufferX < pngWidth && bufferY < pngHeight) {
                    pngBuffer[bufferY * pngWidth + bufferX] = 0xFF000000; // Black with alpha
                }
            }
        }
        
        // Now copy the tile data using the exact same indexing as in GenerateHdTile
        for(int y = 0; y < tileSize; y++) {
            for(int x = 0; x < tileSize; x++) {
                int bufferX = gridX * tileSize + x;
                int bufferY = gridY * tileSize + y;
                int hdIndex = y * tileSize + x; // Match the indexing in GenerateHdTile
                
                if(bufferX < pngWidth && bufferY < pngHeight && hdIndex < tile->HdTileData.size()) {
                    pngBuffer[bufferY * pngWidth + bufferX] = tile->HdTileData[hdIndex];
                }
            }
        }
    }
    
    // Generate filename based on tile type (sprite or background)
    stringstream ss;
    if(isSprite) {
        ss << "tiles" << std::setw(3) << std::setfill('0') << (sheetIndex + 1) << ".png";
    } else {
        ss << "bgtiles" << std::setw(3) << std::setfill('0') << (sheetIndex + 1) << ".png";
    }
    string filename = ss.str();
    
    string fullPath = FolderUtilities::CombinePath(saveFolder, filename);
    
    // Normalize path separators
    for(char& c : fullPath) {
        if(c == '/') {
#ifdef _WIN32
            c = '\\';
#endif
        }
    }
    
    MessageManager::Log("[SMS HD Pack] Saving " + string(isSprite ? "sprite" : "background") + " tile sheet: " + fullPath);
    
    bool success = PNGHelper::WritePNG(fullPath, pngBuffer.data(), pngWidth, pngHeight, 32);
    
    if(success) {
        MessageManager::Log("[SMS HD Pack] Successfully saved " + string(isSprite ? "sprite" : "background") + " tile sheet: " + filename);
    } else {
        MessageManager::Log("[SMS HD Pack] Failed to save " + string(isSprite ? "sprite" : "background") + " tile sheet: " + filename);
    }
}
