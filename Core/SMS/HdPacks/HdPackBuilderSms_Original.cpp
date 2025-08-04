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
#include <map>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <set>
#include <fstream>

HdPackBuilderSms::HdPackBuilderSms(Emulator* emu, SmsConsole* console, HdPackBuilderOptions options) {
    _emu = emu;
    _console = console;
    _vdp = _console->GetVdp();
    _options = options;
    _saveFolder = options.SaveFolder;
    
    // Get ROM name for save folder
    RomInfo romInfo = _emu->GetRomInfo();
    _romName = FolderUtilities::GetFilename(romInfo.RomFile.GetFileName(), false);
    
    // Initialize HD pack data and palette
    InitializeHdPackData();
    UpdatePalette();
    
    if (_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Started for: " + _romName);
    }
}

HdPackBuilderSms::~HdPackBuilderSms() {
    SaveHdPack();
    
    if (_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Saved " + std::to_string(_hdData.Tiles.size()) + " tiles");
    }
}

void HdPackBuilderSms::InitializeHdPackData() {
    _hdData.Scale = _options.Scale;
    _tileUsageCount.clear();
    _tilesByKey.clear();
}



void HdPackBuilderSms::UpdatePalette() {
    if (!_vdp) {
        // Use default palette if VDP not available
        for (int i = 0; i < 32; i++) {
            _smsPalette[i] = 0xFF000000 | (i * 8 << 16) | (i * 8 << 8) | (i * 8);
        }
        return;
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
        } else {
            // SMS uses 6-bit RGB stored in palette RAM (--BBGGRR format)
            uint8_t* paletteRam = _vdp->GetPaletteRam();
            if(paletteRam) {
                for(int i = 0; i < 16; i++) {
                    // Background palette (entries 0-15)
                    uint8_t smsColor = paletteRam[i];
                    
                    // Use our enhanced color conversion function for more accurate colors
                    _bgPalette[i] = ConvertSmsColor(smsColor);
                    
                    // Sprite palette (entries 16-31)
                    smsColor = paletteRam[16 + i];
                    
                    // Use our enhanced color conversion function for more accurate colors
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
                
                // Log palette data for debugging
                if(_options.DebugMode) {
                    for(int i = 0; i < 16; i++) {
                        MessageManager::Log("[SMS HD Pack] BG Palette [" + std::to_string(i) + "] = 0x" + 
                                          HexUtilities::ToHex(paletteRam[i]) + " -> 0x" + 
                                          HexUtilities::ToHex(_bgPalette[i]));
                    }
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
        } else {
            MessageManager::Log("[SMS HD Pack] Warning: Could not access SG palette, using defaults");
            InitializeDefaultSmsPalette();
        }
    }
    
    if(_options.DebugMode && _options.ShowPaletteInfo) {
        LogPaletteInfo();
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
    // Skip if VDP is not available
    if(!_vdp) {
        return;
    }
    
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
    
    // Track usage count
    _tileUsageCount[tile]++;
    
    // Check if we already have this tile
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
        // In SMS, palette index is typically 0 or 1 (low/high palette)
        hdTile->PaletteIndex = (tile.PaletteColors & 0x08) ? 1 : 0;
        
        // Log tile information if debug mode is enabled
        if(_options.VerboseLogging) {
            std::stringstream ss;
            ss << "[SMS HD Pack] Processing " << (isSprite ? "sprite" : "background") << " tile at (" 
               << cycle << "," << scanline << "), VRAM addr: 0x" << std::hex << tileAddr
               << ", palette: " << (int)hdTile->PaletteIndex
               << ", bank hash: 0x" << std::hex << bankHash;
            MessageManager::Log(ss.str());
        }
        
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
    
    // Ensure the tile has HD data generated for visual comparison
    if(tile->HdTileData.empty()) {
        this->GenerateHdTile(tile);
    }
    
    // Create a custom key based on the visual appearance
    std::string visualKey;
    
    // First, add the raw tile data to the key
    for(int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        visualKey += std::to_string(tile->TileData[i]) + ",";
    }
    
    // Add palette information
    visualKey += "P" + std::to_string(tile->PaletteColors) + ",";
    
    // Add sprite/background flag
    visualKey += tile->IsSprite ? "S," : "B,";
    
    // Calculate a hash of the visual key
    std::hash<std::string> hasher;
    uint32_t visualHash = static_cast<uint32_t>(hasher(visualKey));
    
    // Create a tile key for tracking usage
    HdTileKeySms tileKey;
    // Copy relevant data from tile to create the key
    tileKey.TileIndex = tile->TileIndex;
    tileKey.PaletteColors = tile->PaletteColors;
    
    // Store visual hash in the first 4 bytes of TileData for deduplication purposes
    memcpy(tileKey.TileData, &visualHash, sizeof(visualHash));
    
    // Set IsVramTile to true to force using TileData comparison in operator==
    tileKey.IsVramTile = true;
    
    // Check if we already have this tile
    auto existingTile = _tilesByKey.find(tileKey);
    if(existingTile == _tilesByKey.end()) {
        // Create a unique_ptr for the tile and add it to the collection
        auto tilePtr = std::make_unique<HdPackTileInfoSms>(*tile);
        
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
    // SMS tiles are stored with the leftmost pixel in the most significant bit (bit 7)
    
    // Calculate the bit mask for this pixel (7-pixelX to flip the order)
    uint8_t bitMask = 1 << (7 - pixelX);
    
    // Extract the bit from each bitplane using the mask
    uint8_t bit0 = (plane0 & bitMask) ? 1 : 0;
    uint8_t bit1 = (plane1 & bitMask) ? 1 : 0;
    uint8_t bit2 = (plane2 & bitMask) ? 1 : 0;
    uint8_t bit3 = (plane3 & bitMask) ? 1 : 0;
    
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
    if(!tile || !_vdp) {
        MessageManager::Log("[SMS HD Pack] ERROR: Invalid tile or VDP not available");
        return;
    }
    
    // Always log this for debugging the blank tile issue
    MessageManager::Log("[SMS HD Pack] DEBUG: Generating HD tile at address 0x" + 
                       HexUtilities::ToHex(tile->TileIndex) + 
                       ", IsSprite: " + std::string(tile->IsSprite ? "true" : "false") + 
                       ", PaletteIndex: " + std::to_string(tile->PaletteIndex));
    
    int scale = _hdData.Scale;
    int width = SmsHdPackConstants::SMS_TILE_SIZE * scale;
    int height = SmsHdPackConstants::SMS_TILE_SIZE * scale;
    
    // Clear and resize the HD tile data buffer
    tile->HdTileData.clear();
    tile->HdTileData.resize(width * height, 0);
    
    // Read the tile data directly from VRAM
    // Each SMS tile is 32 bytes (8x8 pixels, 4bpp)
    uint8_t tileData[SmsHdPackConstants::SMS_TILE_DATA_SIZE] = {0};
    
    // Calculate the actual VRAM address for this tile
    uint16_t tileAddr = tile->TileIndex * SmsHdPackConstants::SMS_TILE_DATA_SIZE;
    
    // Read the tile data from VRAM
    for(int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        tileData[i] = _vdp->DebugReadVram(tileAddr + i);
    }
    
    // Store the tile data in the tile object
    memcpy(tile->TileData, tileData, SmsHdPackConstants::SMS_TILE_DATA_SIZE);
    
    // Log debug information - always log for debugging blank tile issue
    std::stringstream tileDataSs;
    tileDataSs << "[SMS HD Pack] Tile data bytes for tile 0x" << std::hex << tileAddr << ": ";
    for(int i = 0; i < 8 && i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        tileDataSs << std::hex << std::setw(2) << std::setfill('0') << (int)tileData[i] << " ";
    }
    MessageManager::Log(tileDataSs.str());
    
    // Check if tile data is all zeros (which would result in blank tiles)
    bool allZeros = true;
    for(int i = 0; i < SmsHdPackConstants::SMS_TILE_DATA_SIZE; i++) {
        if(tileData[i] != 0) {
            allZeros = false;
            break;
        }
    }
    
    if(allZeros) {
        MessageManager::Log("[SMS HD Pack] WARNING: Tile data is all zeros for tile at address 0x" + 
                           HexUtilities::ToHex(tileAddr) + ", this will result in a blank tile");
    }
    
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
                // Get the current VDP state to access the most up-to-date palette
                SmsVdpState state = _vdp->GetState();
                uint8_t* paletteRam = _vdp->GetPaletteRam();
                
                if(!paletteRam) {
                    MessageManager::Log("[SMS HD Pack] ERROR: Cannot access palette RAM");
                    color = 0xFFFF00FF; // Magenta (error indicator)
                } else {
                    // For Castle of Illusion and other SMS games, we need to use the
                    // actual palette RAM values directly from the VDP
                    uint8_t paletteIndex;
                    
                    if(tile->IsSprite) {
                        // For sprites, use the sprite palette (second half of palette RAM)
                        // Ensure we stay within bounds (sprite palette is indices 16-31)
                        paletteIndex = 16 + (colorIndex % 16);
                    } else {
                        // For background tiles, use the background palette (first half of palette RAM)
                        // Ensure we stay within bounds (background palette is indices 0-15)
                        paletteIndex = colorIndex % 16;
                    }
                    
                    // Get the raw SMS color value directly from palette RAM
                    uint8_t smsColor = paletteRam[paletteIndex];
                    
                    // Convert the SMS color to ARGB format
                    color = ConvertSmsColor(smsColor);
                    
                    // FIX: Ensure alpha channel is fully opaque for non-transparent pixels
                    // This fixes the black tile rendering issue
                    color = (color & 0x00FFFFFF) | 0xFF000000;
                    
                    // Debug log for palette color
                    if(_options.DebugMode && _options.ShowPaletteInfo && y < 2 && x < 2) {
                        std::stringstream ss;
                        ss << "[SMS HD Pack] Using palette RAM index " << (int)paletteIndex 
                           << " for color index " << (int)colorIndex
                           << ", SMS color=0x" << std::hex << std::setw(2) << std::setfill('0') << (int)smsColor
                           << " -> ARGB=0x" << std::hex << std::setw(8) << std::setfill('0') << color;
                        MessageManager::Log(ss.str());
                    }
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
    
    // Verify palette usage for this tile
    if(_options.DebugMode || _options.VerboseLogging) {
        uint8_t* paletteRam = _vdp->GetPaletteRam();
        if(paletteRam) {
            VerifyPaletteUsage(tile, paletteRam);
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
        
    MessageManager::Log("[SMS HD Pack] Found " + std::to_string(_hdData.Tiles.size()) + " tiles to save");
        
    // Create output directory if it doesn't exist
    std::filesystem::create_directory(_saveFolder);
    
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
    
    MessageManager::Log("[SMS HD Pack] Separated tiles: " + std::to_string(bgTiles.size()) + 
                       " background tiles, " + std::to_string(spriteTiles.size()) + " sprite tiles");
    
    // Process background tiles in chunks of 256 (16x16 grid)
    vector<string> tileSheets;
    
    // Save background tiles
    for(size_t i = 0; i < bgTiles.size(); i += 256) {
        size_t count = std::min((size_t)256, bgTiles.size() - i);
        std::vector<HdPackTileInfoSms*> sheetTiles;
        
        // Extract pointers for this chunk
        for(size_t j = 0; j < count; j++) {
            sheetTiles.push_back(bgTiles[i + j]);
        }
                
        // Generate descriptive sheet filename for background tiles
        std::stringstream ss;
        ss << "BGTILES_" << std::setw(3) << std::setfill('0') << i/256 << ".png";
        std::string sheetName = ss.str();
                
        // Save the background tile sheet
        SaveTileSheet(sheetTiles, _saveFolder, sheetName, false);
        tileSheets.push_back(sheetName);
    }
    
    // Save sprite tiles
    for(size_t i = 0; i < spriteTiles.size(); i += 256) {
        size_t count = std::min((size_t)256, spriteTiles.size() - i);
        std::vector<HdPackTileInfoSms*> sheetTiles;
        
        // Extract pointers for this chunk
        for(size_t j = 0; j < count; j++) {
            sheetTiles.push_back(spriteTiles[i + j]);
        }
                
        // Generate descriptive sheet filename for sprite tiles
        std::stringstream ss;
        ss << "SPRITES_" << std::setw(3) << std::setfill('0') << i/256 << ".png";
        std::string sheetName = ss.str();
                
        // Save the sprite tile sheet
        SaveTileSheet(sheetTiles, _saveFolder, sheetName, true);
        tileSheets.push_back(sheetName);
    }
    
    // Create and write manifest file
    string manifestPath = FolderUtilities::CombinePath(_saveFolder, "hires.txt");
    std::ofstream manifestFile(manifestPath);
    
    if(manifestFile.is_open()) {
        // Write header
        manifestFile << "#HD Pack for " << _romName << std::endl;
        manifestFile << "#Version 1.0" << std::endl;
        manifestFile << std::endl;
        
        // Write options
        manifestFile << "<scale>" << _options.Scale << "</scale>" << std::endl;
        manifestFile << std::endl;
        
        // Write PNG image references
        for(const auto& sheet : tileSheets) {
            manifestFile << "<img>" << sheet << "</img>" << std::endl;
        }
        
        manifestFile.close();
        MessageManager::Log("[SMS HD Pack] Manifest file created: " + manifestPath);
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
    if(tile->HdTileData.size() < expectedSize) {
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
            
            if(srcIndex < tile->HdTileData.size() && 
               dstIndex >= 0 && 
               dstIndex < pngWidth * (pngWidth / tilesPerRow * tileSize)) {
                pngBuffer[dstIndex] = tile->HdTileData[srcIndex];
            }
        }
    }
}

void HdPackBuilderSms::SaveTileSheet(const vector<HdPackTileInfoSms*>& inputTiles, const string& saveFolder, const string& filename, bool isSprite)
{
    if(inputTiles.empty()) {
        MessageManager::Log("[SMS HD Pack] No " + string(isSprite ? "sprite" : "background") + " tiles to save");
        return;
    }
    
    // Enhanced multi-stage deduplication system
    vector<HdPackTileInfoSms*> tiles;
    std::unordered_set<uint64_t> tileHashes;
    std::unordered_map<uint64_t, HdPackTileInfoSms*> hashToTile;
    
    for(size_t i = 0; i < inputTiles.size(); i++) {
        HdPackTileInfoSms* tile = inputTiles[i];
        if(!tile) continue;
        
        if(tile->HdTileData.empty()) {
            GenerateHdTile(tile);
        }
        if(tile->HdTileData.empty()) continue;
        
        // Filter out empty/black tiles with different criteria for sprites vs backgrounds
        bool isEmpty = true;
        int nonTransparentPixels = 0;
        int coloredPixels = 0;
        
        for(uint32_t color : tile->HdTileData) {
            if((color & 0xFF000000) != 0) { // Not transparent
                nonTransparentPixels++;
                // Check if it's not just black
                uint32_t rgb = color & 0xFFFFFF;
                if(rgb != 0x000000) {
                    coloredPixels++;
                    isEmpty = false;
                }
            }
        }
        
        // Different filtering criteria for sprites vs backgrounds
        bool shouldSkip = false;
        if(isSprite) {
            // Sprites: require at least 3 non-transparent pixels with some color
            shouldSkip = (isEmpty || nonTransparentPixels < 3);
        } else {
            // Backgrounds: more lenient - allow simple patterns, even if mostly black
            // Only skip if completely transparent or has no meaningful content
            shouldSkip = (nonTransparentPixels == 0 || (isEmpty && nonTransparentPixels < 8));
        }
        
        if(shouldSkip) {
            if(_options.DebugMode) {
                MessageManager::Log("[SMS HD Pack] Skipping empty tile (" + 
                                   string(isSprite ? "sprite" : "background") + "): TileIndex=" + 
                                   std::to_string(tile->TileIndex) + 
                                   ", NonTransparent=" + std::to_string(nonTransparentPixels) + 
                                   ", Colored=" + std::to_string(coloredPixels));
            }
            continue;
        }
        
        bool isDuplicate = false;
        
        // Pure visual hash-based deduplication - only visual content matters
        uint64_t pureVisualHash = 0;
        
        // Create a hash based ONLY on visual appearance, ignoring all metadata
        if(tile->HdTileData.size() >= 64) {
            // Hash EVERY pixel's RGB values only (ignore alpha, indices, metadata)
            uint64_t visualContentHash = 0;
            for(size_t i = 0; i < tile->HdTileData.size(); i++) {
                // Extract only RGB components, normalize any variations
                uint32_t pixel = tile->HdTileData[i] & 0xFFFFFF; // RGB only
                
                // For SMS, color 0 should be treated as transparent/black consistently
                if(pixel == 0x000000) {
                    pixel = 0; // Normalize black/transparent
                }
                
                visualContentHash = visualContentHash * 31 + pixel;
                // Use a different multiplier every 8 pixels to avoid patterns
                if(i % 8 == 7) visualContentHash = visualContentHash * 37;
            }
            
            // ONLY use visual content - NO tile indices or metadata!
            pureVisualHash = visualContentHash;
            
            // Check if this exact visual hash already exists
            if(tileHashes.find(pureVisualHash) != tileHashes.end()) {
                HdPackTileInfoSms* existingTile = hashToTile[pureVisualHash];
                if(existingTile && existingTile->HdTileData.size() == tile->HdTileData.size()) {
                    // With our comprehensive hash, this should be a guaranteed duplicate
                    // But let's still verify the first few pixels to be absolutely certain
                    bool identical = true;
                    size_t checkPixels = std::min((size_t)16, tile->HdTileData.size());
                    for(size_t k = 0; k < checkPixels; k++) {
                        if(tile->HdTileData[k] != existingTile->HdTileData[k]) {
                            identical = false;
                            break;
                        }
                    }
                    if(identical) {
                        isDuplicate = true;
                        if(_options.DebugMode) {
                            MessageManager::Log("[SMS HD Pack] ELIMINATED visually identical tile: TileIndex=" + 
                                               std::to_string(tile->TileIndex) + 
                                               ", OriginalTileIndex=" + std::to_string(existingTile->TileIndex) + 
                                               ", VisualHash=" + std::to_string(pureVisualHash));
                        }
                    } else {
                        // Hash collision - very rare but possible
                        if(_options.DebugMode) {
                            MessageManager::Log("[SMS HD Pack] Hash collision detected - keeping both tiles");
                        }
                    }
                }
            }
        }
        
        // Fallback: Pixel-by-pixel comparison for any remaining edge cases
        if(!isDuplicate) {
            for(auto& existingTile : tiles) {
                if(!existingTile) continue;
                if(tile->HdTileData.size() == existingTile->HdTileData.size()) {
                    bool visuallyIdentical = true;
                    for(size_t k = 0; k < tile->HdTileData.size(); k++) {
                        uint32_t pixel1 = tile->HdTileData[k] & 0xFFFFFF;
                        uint32_t pixel2 = existingTile->HdTileData[k] & 0xFFFFFF;
                        
                        // Normalize black/transparent pixels
                        if(pixel1 == 0x000000) pixel1 = 0;
                        if(pixel2 == 0x000000) pixel2 = 0;
                        
                        if(pixel1 != pixel2) {
                            visuallyIdentical = false;
                            break;
                        }
                    }
                    if(visuallyIdentical) {
                        isDuplicate = true;
                        if(_options.DebugMode) {
                            MessageManager::Log("[SMS HD Pack] ELIMINATED via pixel comparison: TileIndex=" + 
                                               std::to_string(tile->TileIndex) + 
                                               ", OriginalTileIndex=" + std::to_string(existingTile->TileIndex));
                        }
                        break;
                    }
                }
            }
        }
        
        if(!isDuplicate) {
            tiles.push_back(tile);
            if(pureVisualHash != 0) {
                tileHashes.insert(pureVisualHash);
                hashToTile[pureVisualHash] = tile;
            }
        }
    }
    
    int duplicatesEliminated = inputTiles.size() - tiles.size();
    float reductionPercentage = inputTiles.size() > 0 ? (float)duplicatesEliminated / inputTiles.size() * 100.0f : 0.0f;
    
    MessageManager::Log("[SMS HD Pack] DEDUPLICATION RESULTS for " + string(isSprite ? "sprite" : "background") + " sheet:");
    MessageManager::Log("[SMS HD Pack] Input tiles: " + std::to_string(inputTiles.size()));
    MessageManager::Log("[SMS HD Pack] Unique tiles: " + std::to_string(tiles.size()));
    MessageManager::Log("[SMS HD Pack] Duplicates eliminated: " + std::to_string(duplicatesEliminated) + 
                       " (" + std::to_string((int)reductionPercentage) + "% reduction)");
    
    // Additional debug logging
    if(_options.DebugMode && tiles.size() > 0) {
        MessageManager::Log("[SMS HD Pack] Sample tile info: TileIndex=" + std::to_string(tiles[0]->TileIndex) + 
                           ", PaletteColors=" + std::to_string(tiles[0]->PaletteColors) + 
                           ", HdDataSize=" + std::to_string(tiles[0]->HdTileData.size()));
    }
    
    // If no valid tiles remain after filtering, don't create empty sheets
    if(tiles.empty()) {
        MessageManager::Log("[SMS HD Pack] No valid tiles remaining after filtering for " + 
                           string(isSprite ? "sprite" : "background") + " sheet - skipping");
        return;
    }
    
    // First, ensure all tiles have HD data generated and do basic sorting
    for(auto& tile : tiles) {
        if(tile && tile->HdTileData.empty()) {
            this->GenerateHdTile(tile);
        }
    }
    
    // Simple sorting by visual characteristics for consistent layout
    std::sort(tiles.begin(), tiles.end(), [this](HdPackTileInfoSms* a, HdPackTileInfoSms* b) {
        if(!a) return false;
        if(!b) return true;
        
        // Sort by palette first
        if(a->PaletteIndex != b->PaletteIndex) {
            return a->PaletteIndex < b->PaletteIndex;
        }
        
        // Then by visual hash for consistent grouping
        uint32_t hashA = this->GetTileVisualHash(a);
        uint32_t hashB = this->GetTileVisualHash(b);
        
        if(hashA != hashB) {
            return hashA < hashB;
        }
        
        return a->TileIndex < b->TileIndex;
    });
    
    // Use fixed 16x16 grids to minimize file count and maximize tile packing
    const int TILES_PER_SHEET = 16 * 16; // 256 tiles per sheet
    const int gridWidth = 16;
    const int gridHeight = 16;
    
    // Calculate how many sheets we need
    int totalSheets = (tiles.size() + TILES_PER_SHEET - 1) / TILES_PER_SHEET;
    
    if(_options.DebugMode) {
        MessageManager::Log("[SMS HD Pack] Using 16x16 grid layout for maximum packing:");
        MessageManager::Log("[SMS HD Pack] Total tiles: " + std::to_string(tiles.size()));
        MessageManager::Log("[SMS HD Pack] Sheets needed: " + std::to_string(totalSheets));
        MessageManager::Log("[SMS HD Pack] Tiles per sheet: up to " + std::to_string(TILES_PER_SHEET));
    }
    // Process tiles in batches of 256 (16x16 sheets) to minimize file count
    for(int sheetIndex = 0; sheetIndex < totalSheets; sheetIndex++) {
        int startTile = sheetIndex * TILES_PER_SHEET;
        int endTile = std::min((int)tiles.size(), startTile + TILES_PER_SHEET);
        int tilesInThisSheet = endTile - startTile;
        
        int tileSize = 8 * _hdData.Scale;
        int pngWidth = gridWidth * tileSize;
        int pngHeight = gridHeight * tileSize;
        
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] Creating sheet " + std::to_string(sheetIndex + 1) + "/" + std::to_string(totalSheets) + 
                               " for " + string(isSprite ? "sprite" : "background") + " tiles: " + 
                               std::to_string(pngWidth) + "x" + std::to_string(pngHeight) + 
                               " pixels, " + std::to_string(tilesInThisSheet) + " tiles");
        }
        
        // Initialize buffer with transparent background
        vector<uint32_t> pngBuffer(pngWidth * pngHeight, 0x00000000); // Transparent background
    
        // Get the tiles for this sheet (already sorted and deduplicated)
        // Get the tiles for this sheet (already sorted and deduplicated)
        vector<HdPackTileInfoSms*> sheetTiles;
        for(int i = startTile; i < endTile; i++) {
            if(tiles[i]) {
                sheetTiles.push_back(tiles[i]);
            }
        }
        
        // Draw tiles directly to the buffer in a clean 16x16 grid
        int actualTilesDrawn = 0;
        
        for(int i = 0; i < tilesInThisSheet; i++) {
            if(i >= sheetTiles.size() || !sheetTiles[i]) continue;
            
            // Double-check tile has valid data before drawing
            if(sheetTiles[i]->HdTileData.empty()) {
                if(_options.DebugMode) {
                    MessageManager::Log("[SMS HD Pack] Skipping tile with empty HD data at position " + std::to_string(i));
                }
                continue;
            }
            
            // Calculate position in the 16x16 grid
            int gridX = actualTilesDrawn % gridWidth;
            int gridY = actualTilesDrawn / gridWidth;
            
            // Calculate pixel position in the PNG
            int tileX = gridX * tileSize;
            int tileY = gridY * tileSize;
            
            // Update tile position for manifest (relative to this sheet)
            sheetTiles[i]->X = tileX;
            sheetTiles[i]->Y = tileY;
            
            actualTilesDrawn++;
            
            // Copy tile data to PNG buffer
            int tilePos = 0;
            for(int y = 0; y < tileSize; y++) {
                for(int x = 0; x < tileSize; x++) {
                    if(tilePos < sheetTiles[i]->HdTileData.size()) {
                        pngBuffer[(tileY + y) * pngWidth + (tileX + x)] = sheetTiles[i]->HdTileData[tilePos++];
                    }
                }
            }
        }
        
        // Generate filename with sheet number if multiple sheets
        string sheetFilename = filename;
        if(totalSheets > 1) {
            // Insert sheet number before file extension
            size_t dotPos = filename.find_last_of('.');
            if(dotPos != string::npos) {
                sheetFilename = filename.substr(0, dotPos) + "_" + std::to_string(sheetIndex + 1) + filename.substr(dotPos);
            } else {
                sheetFilename = filename + "_" + std::to_string(sheetIndex + 1);
            }
        }
        
        // Save PNG
        string fullPath = FolderUtilities::CombinePath(saveFolder, sheetFilename);
        
        // Normalize path separators
        for(char& c : fullPath) {
            if(c == '/') {
#ifdef _WIN32
                c = '\\';
#endif
            }
        }
        
        if(_options.DebugMode) {
            MessageManager::Log("[SMS HD Pack] Saving " + string(isSprite ? "sprite" : "background") + " tile sheet " + 
                               std::to_string(sheetIndex + 1) + "/" + std::to_string(totalSheets) + ": " + fullPath);
        }
        
        bool success = PNGHelper::WritePNG(fullPath, pngBuffer.data(), pngWidth, pngHeight, 32);
        
        if(success) {
            MessageManager::Log("[SMS HD Pack] Successfully saved " + string(isSprite ? "sprite" : "background") + " tile sheet: " + sheetFilename + 
                               " (" + std::to_string(actualTilesDrawn) + " tiles)");
        } else {
            MessageManager::Log("[SMS HD Pack] Failed to save " + string(isSprite ? "sprite" : "background") + " tile sheet: " + sheetFilename);
        }
    } // End of sheet loop
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

void HdPackBuilderSms::VerifyPaletteUsage(HdPackTileInfoSms* tile, uint8_t* paletteRam)
{
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
