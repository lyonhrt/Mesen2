#include "pch.h"
#include "SMS/HdPacks/SmsHdTileDumper.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsVdp.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/Serializer.h"
#include "Utilities/HexUtilities.h"
#include <fstream>
#include <filesystem>

// Static initialization
bool SmsHdTileDumper::_dumpingEnabled = false;

SmsHdTileDumper::SmsHdTileDumper(Emulator* emu, const string& romHash)
    : _emu(emu), _romHash(romHash)
{
    _console = dynamic_cast<SmsConsole*>(emu->GetConsole().get());
    if (!_console) {
        MessageManager::Log("[SMS HD Dumper] Error: Could not get SMS console");
        return;
    }
}

SmsHdTileDumper::~SmsHdTileDumper()
{
    // Save the manifest when the dumper is destroyed
    SaveManifest();
}

void SmsHdTileDumper::Initialize(const string& outputFolder)
{
    _outputFolder = outputFolder;
    
    // Create the output folder structure
    _graphicsFolder = FolderUtilities::CombinePath(_outputFolder, "Graphics");
    FolderUtilities::CreateFolder(_outputFolder);
    FolderUtilities::CreateFolder(_graphicsFolder);
    
    _manifestPath = FolderUtilities::CombinePath(_outputFolder, "hdsms.txt");
    
    MessageManager::Log("[SMS HD Dumper] Initialized with output folder: " + _outputFolder);
}

bool SmsHdTileDumper::IsDumpingEnabled()
{
    return _dumpingEnabled;
}

void SmsHdTileDumper::SetDumpingEnabled(bool enabled)
{
    _dumpingEnabled = enabled;
    MessageManager::Log(enabled ? 
        "[SMS HD Dumper] Tile dumping enabled" : 
        "[SMS HD Dumper] Tile dumping disabled");
}

string SmsHdTileDumper::GenerateTileHash(uint8_t* patternData, int paletteIndex, bool hFlip, bool vFlip)
{
    // Create a unique identifier for this tile + palette + flip combination
    // Since we can't use Serializer as intended, we'll create a simple hash
    uint32_t hash = 0;
    
    // Hash the tile pattern data (32 bytes)
    for (int i = 0; i < 32; i++) {
        hash = (hash << 3) ^ (hash >> 29) ^ patternData[i];
    }
    
    // Add palette and flip information to the hash
    hash = (hash << 3) ^ (hash >> 29) ^ paletteIndex;
    hash = (hash << 1) ^ (hash >> 31) ^ (hFlip ? 1 : 0);
    hash = (hash << 1) ^ (hash >> 31) ^ (vFlip ? 1 : 0);
    
    // Convert to hex string
    return HexUtilities::ToHex((uint32_t)hash);
}

void SmsHdTileDumper::ConvertTileToRgba(uint8_t* patternData, int paletteIndex, bool hFlip, bool vFlip, uint32_t* outputBuffer)
{
    // Get the SMS palette
    const uint16_t* palette = GetSmsPalette();
    
    // SMS tiles are 8x8 with 4bpp (4 bits per pixel)
    // Each row uses 4 bytes (4 bitplanes)
    for (int y = 0; y < 8; y++) {
        int targetY = vFlip ? (7 - y) : y;
        
        // Get the 4 bytes for this row (4 bitplanes)
        uint8_t bp0 = patternData[y * 4];
        uint8_t bp1 = patternData[y * 4 + 1];
        uint8_t bp2 = patternData[y * 4 + 2];
        uint8_t bp3 = patternData[y * 4 + 3];
        
        for (int x = 0; x < 8; x++) {
            int targetX = hFlip ? (7 - x) : x;
            
            // Extract the color index (4 bits) for this pixel
            int shift = 7 - x;
            uint8_t colorIndex = 
                ((bp0 >> shift) & 0x01) |
                (((bp1 >> shift) & 0x01) << 1) |
                (((bp2 >> shift) & 0x01) << 2) |
                (((bp3 >> shift) & 0x01) << 3);
            
            // Apply palette offset
            colorIndex += (paletteIndex * 16);
            
            // Get the RGB555 color from the palette
            uint16_t rgb555 = palette[colorIndex & 0x1F];
            
            // Convert RGB555 to RGBA8888
            uint8_t r = ((rgb555 >> 10) & 0x1F) << 3;
            uint8_t g = ((rgb555 >> 5) & 0x1F) << 3;
            uint8_t b = (rgb555 & 0x1F) << 3;
            
            // Make color index 0 transparent
            uint8_t a = (colorIndex & 0x0F) == 0 ? 0 : 255;
            
            // Store the RGBA value
            outputBuffer[targetY * 8 + targetX] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

string SmsHdTileDumper::SaveTilePng(uint32_t* rgbaData, const string& hash)
{
    // Create the PNG filename
    string pngFilename = "tile_" + hash + ".png";
    string pngPath = FolderUtilities::CombinePath(_graphicsFolder, pngFilename);
    
    // Save the 8x8 tile as a PNG
    PNGHelper::WritePNG(pngPath, rgbaData, 8, 8);
    
    return pngFilename;
}

void SmsHdTileDumper::AddManifestEntry(int x, int y, int paletteIndex, bool hFlip, bool vFlip, bool isSprite, const string& pngFile)
{
    // Format: <tile x=80 y=128 palette=1 flip=H type=sprite image=tile_abc123.png />
    string entry = "<tile ";
    entry += "x=" + std::to_string(x) + " ";
    entry += "y=" + std::to_string(y) + " ";
    entry += "palette=" + std::to_string(paletteIndex) + " ";
    
    // Add flip flags if any
    if (hFlip && vFlip) {
        entry += "flip=HV ";
    } else if (hFlip) {
        entry += "flip=H ";
    } else if (vFlip) {
        entry += "flip=V ";
    }
    
    // Add tile type
    entry += "type=" + string(isSprite ? "sprite" : "bg") + " ";
    
    // Add image reference
    entry += "image=" + pngFile + " />";
    
    // Add to manifest entries
    _manifestEntries.push_back(entry);
}

void SmsHdTileDumper::DumpTile(uint8_t* patternData, int paletteIndex, bool hFlip, bool vFlip, int x, int y, bool isSprite)
{
    if (!_dumpingEnabled || !_console) {
        return;
    }
    
    // Generate a hash for this tile + palette + flip combination
    string tileHash = GenerateTileHash(patternData, paletteIndex, hFlip, vFlip);
    
    // Check if we've already dumped this tile
    auto it = _tileHashToPngMap.find(tileHash);
    if (it != _tileHashToPngMap.end()) {
        // We've already dumped this tile, just add a new manifest entry
        AddManifestEntry(x, y, paletteIndex, hFlip, vFlip, isSprite, it->second);
        return;
    }
    
    // Convert the tile to RGBA
    uint32_t rgbaData[64]; // 8x8 = 64 pixels
    ConvertTileToRgba(patternData, paletteIndex, hFlip, vFlip, rgbaData);
    
    // Save the tile as a PNG
    string pngFile = SaveTilePng(rgbaData, tileHash);
    
    // Add to our hash map
    _tileHashToPngMap[tileHash] = pngFile;
    
    // Add a manifest entry
    AddManifestEntry(x, y, paletteIndex, hFlip, vFlip, isSprite, pngFile);
}

void SmsHdTileDumper::SaveManifest()
{
    if (_manifestEntries.empty()) {
        return;
    }
    
    try {
        std::ofstream manifest(_manifestPath);
        if (!manifest) {
            MessageManager::Log("[SMS HD Dumper] Error: Could not create manifest file: " + _manifestPath);
            return;
        }
        
        // Write header
        manifest << "<!-- SMS HD Pack Manifest -->" << std::endl;
        manifest << "<hdsms>" << std::endl;
        
        // Write all tile entries
        for (const auto& entry : _manifestEntries) {
            manifest << "  " << entry << std::endl;
        }
        
        // Write footer
        manifest << "</hdsms>" << std::endl;
        
        manifest.close();
        
        MessageManager::Log("[SMS HD Dumper] Saved manifest with " + 
            std::to_string(_manifestEntries.size()) + " entries to: " + _manifestPath);
    }
    catch (const std::exception& ex) {
        MessageManager::Log("[SMS HD Dumper] Error saving manifest: " + string(ex.what()));
    }
}

const uint16_t* SmsHdTileDumper::GetSmsPalette()
{
    // Get the SMS VDP to access the palette
    SmsVdp* vdp = _console->GetVdp();
    if (!vdp) {
        static uint16_t defaultPalette[32] = {}; // Return empty palette if VDP not available
        return defaultPalette;
    }
    
    // Return the internal palette RAM
    return vdp->GetInternalPaletteRam();
}
