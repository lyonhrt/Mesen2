#include "pch.h"
#include "SMS/HdPacks/SmsHdDebugDump.h"
#include "Utilities/FolderUtilities.h"
#include "Shared/MessageManager.h"
#include <fstream>
#include <sstream>
#include <iomanip>

void SmsHdDebugDump::WriteDebugInfo(
    const std::string& saveFolder,
    const std::string& romName,
    uint32_t scale,
    uint32_t totalSpriteCount,
    uint32_t totalBgCount,
    uint32_t uniqueSpriteCount,
    uint32_t uniqueBgCount,
    size_t totalUniqueTiles,
    const uint32_t spritePalette[16],
    const uint32_t bgPalette[16]
)
{
    std::string debugPath = FolderUtilities::CombinePath(saveFolder, "debug_info.txt");
    std::ofstream debugFile(debugPath);

    if(!debugFile.is_open()) {
        MessageManager::Log("[SMS HD Pack] Error: Failed to create debug info file: " + debugPath);
        return;
    }

    debugFile << "SMS HD Pack Debug Information" << std::endl;
    debugFile << "===========================" << std::endl << std::endl;

    debugFile << "ROM: " << romName << std::endl;
    debugFile << "Scale: " << scale << std::endl << std::endl;

    debugFile << "Tile Statistics:" << std::endl;
    debugFile << "---------------" << std::endl;
    debugFile << "Total sprite tiles processed: " << totalSpriteCount << std::endl;
    debugFile << "Total background tiles processed: " << totalBgCount << std::endl;
    debugFile << "Unique sprite tiles: " << uniqueSpriteCount << std::endl;
    debugFile << "Unique background tiles: " << uniqueBgCount << std::endl;
    debugFile << "Total unique tiles: " << totalUniqueTiles << std::endl << std::endl;

    auto writePalette = [&debugFile](const char* title, const uint32_t pal[16]){
        debugFile << title << std::endl;
        for(int i = 0; i < 16; i++) {
            uint32_t color = pal[i];
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = (color) & 0xFF;
            debugFile << "Color " << std::dec << i << ": RGB("
                      << (int)r << "," << (int)g << "," << (int)b << ") - #"
                      << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)r
                      << std::setw(2) << (int)g
                      << std::setw(2) << (int)b
                      << std::nouppercase << std::dec << std::endl;
        }
        debugFile << std::endl;
    };

    debugFile << "Palette Information:" << std::endl;
    debugFile << "-------------------" << std::endl;

    writePalette("Sprite Palette:", spritePalette);

    bool palettesAreDifferent = false;
    for(int i = 0; i < 16; i++) {
        if(spritePalette[i] != bgPalette[i]) { palettesAreDifferent = true; break; }
    }

    if(palettesAreDifferent) {
        writePalette("Background Palette:", bgPalette);
    } else {
        debugFile << "Background Palette is identical to Sprite Palette" << std::endl;
    }

    debugFile.close();
    MessageManager::Log("[SMS HD Pack] Saved debug info: " + debugPath);
}
