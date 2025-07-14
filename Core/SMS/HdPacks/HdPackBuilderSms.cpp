#include "pch.h"
#include "SMS/HdPacks/HdPackBuilderSms.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsTypes.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include <filesystem>
#include <iomanip>
#include <sstream>

HdPackBuilderSms::HdPackBuilderSms(Emulator* emu, SmsConsole* console, HdPackBuilderOptions options) {
    _emu = emu;
    _console = console;
    _isChrRam = true;
    _options = options;
    _saveFolder = options.SaveFolder;
    
    // Get ROM name for save folder
    RomInfo romInfo = _emu->GetRomInfo();
    _romName = FolderUtilities::GetFilename(romInfo.RomFile.GetFileName(), false);
    
    // Initialize HD pack data
    _hdData.Scale = options.Scale;
    _hdData.Version = 100; // Use simple version number
    
    MessageManager::Log("[SMS HD Pack] Started tile dumping for: " + _romName);
    MessageManager::Log("[SMS HD Pack] Save location: " + _saveFolder);
}

HdPackBuilderSms::~HdPackBuilderSms() {
    SaveHdPack();
}

void HdPackBuilderSms::ProcessTile(uint32_t cycle, uint32_t scanline, uint32_t tileAddr, HdTileKeySms& tile, 
                                  bool isSprite, uint32_t bankHash, bool hasBgSprite) {
    
    // Add comprehensive debug logging
    static int totalProcessTileCalls = 0;
    totalProcessTileCalls++;
    
    MessageManager::Log("[SMS HD Pack] DEBUG: ProcessTile called #" + std::to_string(totalProcessTileCalls));
    MessageManager::Log("[SMS HD Pack] DEBUG: tileAddr=0x" + std::to_string(tileAddr) + 
                      " cycle=" + std::to_string(cycle) + 
                      " scanline=" + std::to_string(scanline) + 
                      " isSprite=" + std::string(isSprite ? "true" : "false"));
    
    // Track usage count
    _tileUsageCount[tile]++;
    
    MessageManager::Log("[SMS HD Pack] DEBUG: Current tile usage count: " + std::to_string(_tileUsageCount[tile]));
    
    // Check if we already have this tile
    auto existingTile = _tilesByKey.find(tile);
    if(existingTile == _tilesByKey.end()) {
        MessageManager::Log("[SMS HD Pack] DEBUG: New tile detected, creating HdPackTileInfoSms");
        
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
        hdTile->ChrBankId = GetChrBankId(tileAddr);
        hdTile->IsSprite = isSprite;
        
        _tilesByKey[tile] = hdTile;
        _hdData.Tiles.push_back(unique_ptr<HdPackTileInfoSms>(hdTile));
        
        MessageManager::Log("[SMS HD Pack] DEBUG: Added tile to collection. Total tiles now: " + std::to_string(_hdData.Tiles.size()));
        
        // Generate the HD tile image
        GenerateHdTile(hdTile);
        
        MessageManager::Log("[SMS HD Pack] DEBUG: Tile processing completed successfully");
    } else {
        MessageManager::Log("[SMS HD Pack] DEBUG: Tile already exists, skipping");
    }
}

void HdPackBuilderSms::AddTile(HdPackTileInfoSms* tile, uint32_t usageCount) {
    // Add tile to the pack data
}

uint32_t HdPackBuilderSms::GetChrBankId(uint32_t tileAddr) {
    // SMS VDP doesn't have banks like NES, but we can use the tile address
    return tileAddr / 0x1000; // 4KB chunks
}

void HdPackBuilderSms::GenerateHdTile(HdPackTileInfoSms* tile) {
    // Generate the upscaled tile image data
    uint32_t tileSize = tile->Width * tile->Height;
    tile->HdTileData.resize(tileSize);
    
    // SMS uses 4-planar bitplane format: 32 bytes per tile
    // Format: 8 rows, each row has 4 bytes (one for each bitplane)
    // Byte layout per row: [plane0, plane1, plane2, plane3]
    // Each bitplane byte contains 8 pixels (1 bit per pixel)
    
    for(uint32_t y = 0; y < 8; y++) {
        for(uint32_t x = 0; x < 8; x++) {
            // Extract 4-bit pixel from SMS 4-planar tile data
            uint32_t rowOffset = y * 4; // 4 bytes per row
            uint8_t bitPos = 7 - x;      // MSB = leftmost pixel
            
            // Extract one bit from each of the 4 bitplanes
            uint8_t pixel = 0;
            pixel |= ((tile->TileData[rowOffset + 0] >> bitPos) & 1) << 0; // Plane 0 -> bit 0
            pixel |= ((tile->TileData[rowOffset + 1] >> bitPos) & 1) << 1; // Plane 1 -> bit 1  
            pixel |= ((tile->TileData[rowOffset + 2] >> bitPos) & 1) << 2; // Plane 2 -> bit 2
            pixel |= ((tile->TileData[rowOffset + 3] >> bitPos) & 1) << 3; // Plane 3 -> bit 3
            
            // Convert to RGBA color
            uint32_t color = 0xFF000000; // Default alpha
            if(pixel > 0) {
                // Generate a unique color for each palette value for debugging
                // This creates a rainbow pattern to help identify different pixels
                switch(pixel & 0xF) {
                    case 0x1: color = 0xFF0000FF; break; // Blue
                    case 0x2: color = 0xFF00FF00; break; // Green  
                    case 0x3: color = 0xFF00FFFF; break; // Cyan
                    case 0x4: color = 0xFFFF0000; break; // Red
                    case 0x5: color = 0xFFFF00FF; break; // Magenta
                    case 0x6: color = 0xFFFFFF00; break; // Yellow
                    case 0x7: color = 0xFFFFFFFF; break; // White
                    case 0x8: color = 0xFF808080; break; // Gray
                    case 0x9: color = 0xFF8080FF; break; // Light Blue
                    case 0xA: color = 0xFF80FF80; break; // Light Green
                    case 0xB: color = 0xFF80FFFF; break; // Light Cyan
                    case 0xC: color = 0xFFFF8080; break; // Light Red
                    case 0xD: color = 0xFFFF80FF; break; // Light Magenta
                    case 0xE: color = 0xFFFFFF80; break; // Light Yellow
                    case 0xF: color = 0xFFC0C0C0; break; // Light Gray
                    default:  color = 0xFF000000; break; // Black (should not happen)
                }
            }
            
            // Scale up the pixel for HD output
            for(uint32_t sy = 0; sy < _options.Scale; sy++) {
                for(uint32_t sx = 0; sx < _options.Scale; sx++) {
                    uint32_t scaledX = x * _options.Scale + sx;
                    uint32_t scaledY = y * _options.Scale + sy;
                    tile->HdTileData[scaledY * tile->Width + scaledX] = color;
                }
            }
        }
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

void HdPackBuilderSms::SaveHdPack() {
    MessageManager::Log("[SMS HD Pack] DEBUG: SaveHdPack called");
    MessageManager::Log("[SMS HD Pack] DEBUG: _hdData.Tiles.size() = " + std::to_string(_hdData.Tiles.size()));
    
    if(_hdData.Tiles.empty()) {
        MessageManager::Log("[SMS HD Pack] No tiles to save - tile collection is empty");
        return;
    }
    
    MessageManager::Log("[SMS HD Pack] DEBUG: Proceeding with save - found " + std::to_string(_hdData.Tiles.size()) + " tiles");
    
    // Create save directory
    string normalizedSaveFolder = _saveFolder;
    for(char& c : normalizedSaveFolder) {
        if(c == '/') {
#ifdef _WIN32
            c = '\\';
#endif
        }
    }
    
    FolderUtilities::CreateFolder(normalizedSaveFolder);
    MessageManager::Log("[SMS HD Pack] Created directory: " + normalizedSaveFolder);
    
    // Organize tiles into sheets (256 tiles per sheet)
    vector<vector<HdPackTileInfoSms*>> tileSheets;
    vector<HdPackTileInfoSms*> currentSheet;
    
    for(auto& tile : _hdData.Tiles) {
        currentSheet.push_back(tile.get());
        
        if(currentSheet.size() >= 256) {
            tileSheets.push_back(currentSheet);
            currentSheet.clear();
        }
    }
    
    // Add remaining tiles if any
    if(!currentSheet.empty()) {
        tileSheets.push_back(currentSheet);
    }
    
    MessageManager::Log("[SMS HD Pack] Generated " + std::to_string(tileSheets.size()) + " tile sheets");
    
    // Save PNG tile sheets
    for(size_t i = 0; i < tileSheets.size(); i++) {
        SaveTileSheet(tileSheets[i], normalizedSaveFolder, (int)i);
    }
    
    // Create manifest file
    string manifestPath = FolderUtilities::CombinePath(normalizedSaveFolder, "hires.txt");
    for(char& c : manifestPath) {
        if(c == '/') {
#ifdef _WIN32
            c = '\\';
#endif
        }
    }
    
    MessageManager::Log("[SMS HD Pack] Creating manifest: " + manifestPath);
    
    try {
        ofstream manifestFile(manifestPath, ios::out);
        
        if(manifestFile.is_open()) {
            // Write header
            manifestFile << "<ver>" << _hdData.Version << std::endl;
            manifestFile << "<scale>" << _hdData.Scale << std::endl;
            manifestFile << "# SMS HD Pack - Generated by Mesen2" << std::endl;
            manifestFile << "# ROM: " << _romName << std::endl;
            manifestFile << std::endl;
            
            // Write PNG image references
            for(size_t i = 0; i < tileSheets.size(); i++) {
                stringstream ss;
                ss << "<img>tiles" << std::setw(3) << std::setfill('0') << (i + 1) << ".png" << std::endl;
                manifestFile << ss.str();
            }
            manifestFile << std::endl;
            
            // Write tile entries with PNG references
            int currentSheetIndex = 0;
            int tileInSheet = 0;
            
            for(auto& tile : _hdData.Tiles) {
                // Calculate which PNG sheet this tile belongs to
                int sheetIndex = (currentSheetIndex * 256 + tileInSheet) / 256;
                int positionInSheet = (currentSheetIndex * 256 + tileInSheet) % 256;
                
                // Update tile position based on its location in the PNG
                int tileDimension = 8 * _hdData.Scale;
                tile->X = (positionInSheet % 16) * tileDimension;
                tile->Y = (positionInSheet / 16) * tileDimension;
                
                // Write tile entry: <tile>pngIndex,tileDataHex,paletteColors,x,y,brightness,flags
                manifestFile << "<tile>" << sheetIndex << ",";
                
                // Write 32-byte tile data as hex
                for(int i = 0; i < 32; i++) {
                    manifestFile << std::hex << std::setw(2) << std::setfill('0') << (int)tile->TileData[i];
                }
                
                manifestFile << "," << std::hex << std::setw(8) << std::setfill('0') << tile->PaletteColors;
                manifestFile << "," << std::dec << tile->X << "," << tile->Y << ",1.0,N" << std::endl;
                
                tileInSheet++;
                if(tileInSheet >= 256) {
                    currentSheetIndex++;
                    tileInSheet = 0;
                }
            }
            
            manifestFile.close();
            MessageManager::Log("[SMS HD Pack] Successfully created manifest with " + std::to_string(_hdData.Tiles.size()) + " tiles");
            MessageManager::Log("[SMS HD Pack] Generated " + std::to_string(tileSheets.size()) + " PNG tile sheets");
            MessageManager::Log("[SMS HD Pack] SUCCESS: Complete HD pack created with PNG tiles!");
        } else {
            MessageManager::Log("[SMS HD Pack] ERROR: Failed to create manifest file: " + manifestPath);
        }
    } catch(const std::exception& e) {
        MessageManager::Log("[SMS HD Pack] ERROR: Exception during manifest creation: " + string(e.what()));
    }
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
    
    // Copy tile data to PNG buffer
    int pngPos = y * pngWidth + x;
    int tilePos = 0;
    for(int i = 0; i < tileDimension; i++) {
        for(int j = 0; j < tileDimension; j++) {
            pngBuffer[pngPos] = tile->HdTileData[tilePos++];
            pngPos++;
        }
        pngPos += pngWidth - tileDimension;
    }
}

void HdPackBuilderSms::SaveTileSheet(const vector<HdPackTileInfoSms*>& tiles, const string& saveFolder, int sheetIndex) {
    if(tiles.empty()) {
        return;
    }
    
    int tileDimension = 8 * _hdData.Scale;
    int pngDimension = 16 * tileDimension; // 16x16 grid of tiles
    int pngBufferSize = pngDimension * pngDimension;
    
    vector<uint32_t> pngBuffer(pngBufferSize, 0xFF000000); // Initialize to black with alpha
    
    // Draw tiles to the buffer
    for(size_t i = 0; i < tiles.size() && i < 256; i++) {
        if(tiles[i]) {
            DrawTile(tiles[i], (int)i, pngBuffer.data(), pngDimension);
        }
    }
    
    // Generate filename
    stringstream ss;
    ss << "tiles" << std::setw(3) << std::setfill('0') << (sheetIndex + 1) << ".png";
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
    
    MessageManager::Log("[SMS HD Pack] Saving tile sheet: " + fullPath);
    
    bool success = PNGHelper::WritePNG(fullPath, pngBuffer.data(), pngDimension, pngDimension, 32);
    
    if(success) {
        MessageManager::Log("[SMS HD Pack] Successfully saved tile sheet: " + filename);
    } else {
        MessageManager::Log("[SMS HD Pack] Failed to save tile sheet: " + filename);
    }
}
