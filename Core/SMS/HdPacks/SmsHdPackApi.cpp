#include "pch.h"
#include "SMS/HdPacks/SmsHdPackApi.h"
#include "SMS/HdPacks/HdPackBuilderSms.h"
#include "SMS/SmsConsole.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include <memory>

namespace SmsHdPackApi {
    // Global state for HD pack dumping
    static std::unique_ptr<HdPackBuilderSms> g_hdPackBuilder;
    static bool g_isDumping = false;

    // Helper function to sanitize ROM names for filesystem use
    string SanitizeRomName(const string& romName) {
        string clean = romName;
        
        // Replace problematic characters with safe alternatives
        for(char& c : clean) {
            if(c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
                c = '_';
            } else if(c == ',' || c == ':' || c == ';') {
                c = '_';
            } else if(c == '"' || c == '\'' || c == '`') {
                c = '_';
            } else if(c == '<' || c == '>' || c == '|' || c == '?' || c == '*') {
                c = '_';
            } else if(c == '/' || c == '\\') {
                c = '_';
            } else if(c == ' ') {
                c = '_';
            }
        }
        
        // Limit length to avoid path too long errors
        if(clean.length() > 80) {
            clean = clean.substr(0, 80);
        }
        
        // Remove trailing dots, spaces, and underscores
        while(!clean.empty() && (clean.back() == '.' || clean.back() == ' ' || clean.back() == '_')) {
            clean.pop_back();
        }
        
        // Ensure it's not empty
        if(clean.empty()) {
            clean = "Unknown_SMS_Game";
        }
        
        return clean;
    }

    // Auto-start function called when SMS game is loaded
    void AutoStartOnGameLoaded(Emulator* emu) {
        if(emu->GetConsoleType() != ConsoleType::Sms) {
            return;
        }
        
        if(g_isDumping) {
            return; // Already dumping
        }
        
        // Set up HD pack builder options for SMS
        HdPackBuilderOptions options = {};
        
        string hdPackFolder = FolderUtilities::GetHdPackFolder();
        RomInfo romInfo = emu->GetRomInfo();
        string romName = FolderUtilities::GetFilename(romInfo.RomFile.GetFileName(), false);
        string cleanRomName = SanitizeRomName(romName);
        string saveFolder = FolderUtilities::CombinePath(hdPackFolder, cleanRomName);
        
        options.SaveFolder = saveFolder;
        options.Scale = 1;
        options.VramBankSize = 0x1000;
        options.GroupBlankTiles = true;
        options.SortByUsageFrequency = false;
        options.IgnoreOverscan = false;
        
        // Get the SMS console
        auto console = emu->GetConsole();
        SmsConsole* smsConsole = static_cast<SmsConsole*>(console.get());
        
        // Create the HD pack builder
        g_hdPackBuilder = std::make_unique<HdPackBuilderSms>(emu, smsConsole, options);
        g_isDumping = true;
        
        MessageManager::Log("[SMS HD Pack] Auto-started tile dumping for: " + romName);
        MessageManager::Log("[SMS HD Pack] Will save to: " + saveFolder);
    }

    // Save and stop dumping when the game is powered off
    void SaveOnPowerOff(Emulator* emu) {
        if(!g_isDumping || !g_hdPackBuilder) {
            return;
        }
        
        MessageManager::Log("[SMS HD Pack] Saving tiles on power off...");
        g_hdPackBuilder->SaveHdPack();
        g_hdPackBuilder.reset();
        g_isDumping = false;
        MessageManager::Log("[SMS HD Pack] Tiles saved successfully.");
    }

    // Function to be called from SMS VDP during tile rendering
    void ProcessSmsBackgroundTile(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr, 
                                 uint8_t* tileData, uint32_t paletteColors, bool hMirror, bool vMirror, bool priority) {
        
        if(!g_isDumping || !g_hdPackBuilder) {
            return;
        }
        
        // Create tile key from the SMS tile data
        HdTileKeySms tileKey = {};
        tileKey.TileIndex = tileAddr / 32;
        tileKey.IsVramTile = true;
        tileKey.PaletteColors = paletteColors;
        
        // Copy the 32-byte SMS tile data
        memcpy(tileKey.TileData, tileData, 32);
        
        // Process the tile
        uint32_t bankHash = tileAddr / 0x1000;
        g_hdPackBuilder->ProcessTile(x, y, tileAddr, tileKey, false, bankHash, false);
    }

    // Function to be called from SMS VDP during sprite rendering  
    void ProcessSmsSprite(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr,
                         uint8_t* tileData, uint32_t paletteColors) {
        if(!g_isDumping || !g_hdPackBuilder) {
            return;
        }
        
        // Create tile key from the SMS sprite data
        HdTileKeySms tileKey = {};
        tileKey.TileIndex = tileAddr / 32;
        tileKey.IsVramTile = true;
        tileKey.PaletteColors = paletteColors;
        
        // Copy the 32-byte SMS tile data
        memcpy(tileKey.TileData, tileData, 32);
        
        // Process as sprite tile
        uint32_t bankHash = tileAddr / 0x1000;
        g_hdPackBuilder->ProcessTile(x, y, tileAddr, tileKey, true, bankHash, false);
    }

    // Check if currently dumping
    bool IsCurrentlyDumping() {
        return g_isDumping;
    }
}