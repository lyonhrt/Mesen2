#include "pch.h"
#include "SMS/HdPacks/SmsHdPackApi.h"
#include "SMS/HdPacks/HdPackBuilderSms.h"
#include "SMS/HdPacks/SmsHdTileDumper.h"
#include "SMS/SmsConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include <memory>
#include "SMS/HdPacks/HdPackDebug.h"

namespace SmsHdPackApi {
    // Global state for HD pack dumping
    static std::unique_ptr<HdPackBuilderSms> g_hdPackBuilder;
    static std::unique_ptr<SmsHdTileDumper> g_hdTileDumper;
    static bool g_isDumping = false;
    static uint64_t g_bgSeen = 0;
    static uint64_t g_sprSeen = 0;

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

    // Auto-start hook when an SMS game is loaded (no longer auto-starts dumping; waits for explicit Start Recording)
    void AutoStartOnGameLoaded(Emulator* emu) {
        if(emu->GetConsoleType() != ConsoleType::Sms) {
            return;
        }
        
        // Sync dumper enable flag from settings, but do not start recording automatically
        SmsConfig& smsConfig = emu->GetSettings()->GetSmsConfig();
        SmsHdTileDumper::SetDumpingEnabled(smsConfig.EnableHdTileDumping);
        
        // Ensure we're not considered dumping until the user explicitly starts it
        g_isDumping = false;
        
        MessageManager::Log("[SMS HD Pack] Ready. Use Start Recording to begin tile dumping.");
    }

    // Save and stop dumping when the game is powered off
    void SaveOnPowerOff(Emulator* emu) {
        if(!g_isDumping) {
            return;
        }
        
        HDLOG_TAG("Session", "Power off: saving. BG seen=" + std::to_string(g_bgSeen) + ", SPR seen=" + std::to_string(g_sprSeen));

        if(g_hdPackBuilder) {
            MessageManager::Log("[SMS HD Pack] Saving tiles on power off...");
            g_hdPackBuilder->SaveHdPack();
            g_hdPackBuilder.reset();
            MessageManager::Log("[SMS HD Pack] Tiles saved successfully.");
        }
        
        if(g_hdTileDumper) {
            MessageManager::Log("[SMS HD Tile] Saving manifest on power off...");
            g_hdTileDumper->SaveManifest();
            g_hdTileDumper.reset();
            MessageManager::Log("[SMS HD Tile] Manifest saved successfully.");
        }
        
        g_isDumping = false;
        g_bgSeen = g_sprSeen = 0;
    }

    // Explicit start recording entry point (called from UI shortcut)
    void StartRecording(Emulator* emu, HdPackBuilderOptions options) {
        if(emu->GetConsoleType() != ConsoleType::Sms) {
            return;
        }
        
        // Create builder if needed
        if(!g_hdPackBuilder) {
            auto console = emu->GetConsole();
            SmsConsole* smsConsole = static_cast<SmsConsole*>(console.get());
            g_hdPackBuilder = std::make_unique<HdPackBuilderSms>(emu, smsConsole, options);
        }
        
        // Initialize tile dumper if enabled and not created yet
        if(SmsHdTileDumper::IsDumpingEnabled() && !g_hdTileDumper) {
            RomInfo romInfo = emu->GetRomInfo();
            g_hdTileDumper = std::make_unique<SmsHdTileDumper>(emu, romInfo.RomFile.GetSha1Hash());
            g_hdTileDumper->Initialize(options.SaveFolder);
        }
        
        // Ensure builder is actively recording
        if(!g_hdPackBuilder->IsRecording()) {
            g_hdPackBuilder->StartRecording();
        }
        
        g_isDumping = true;
        g_bgSeen = g_sprSeen = 0;
        
        MessageManager::Log("[SMS HD Pack] Recording started. Saving to: " + options.SaveFolder);
        HDLOG_TAG("Session", "Recording started: folder=" + options.SaveFolder);
    }

    // Explicit stop recording entry point (called from UI shortcut)
    void StopRecording(Emulator* emu) {
        if(!g_isDumping) {
            return;
        }
        
        if(g_hdPackBuilder) {
            g_hdPackBuilder->StopRecording();
            g_hdPackBuilder.reset();
        }
        if(g_hdTileDumper) {
            g_hdTileDumper->SaveManifest();
            g_hdTileDumper.reset();
        }
        
        g_isDumping = false;
        HDLOG_TAG("Counters", "BG tiles seen=" + std::to_string(g_bgSeen) + ", SPR tiles seen=" + std::to_string(g_sprSeen));
        g_bgSeen = g_sprSeen = 0;
        MessageManager::Log("[SMS HD Pack] Recording stopped.");
    }

    // Function to be called from SMS VDP during tile rendering
    void ProcessSmsBackgroundTile(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr, 
                                 uint8_t* tileData, uint32_t paletteColors, bool hMirror, bool vMirror, bool priority) {
        
        if(!g_isDumping) {
            return;
        }
        
        // Process with HD pack builder if available
        if(g_hdPackBuilder) {
            // Create tile key from the SMS tile data
            HdTileKeySms tileKey = {};
            tileKey.TileIndex = tileAddr / 32;
            tileKey.IsVramTile = true;
            tileKey.IsSprite = false;
            tileKey.PaletteColors = paletteColors;
            
            // Copy the 32-byte SMS tile data
            memcpy(tileKey.TileData, tileData, 32);
            
            // Process the tile
            uint32_t bankHash = tileAddr / 0x1000;
            g_bgSeen++;
            g_hdPackBuilder->ProcessTile(x, y, tileAddr, tileKey, false, bankHash, false);
        }
        
        // Process with HD tile dumper if available
        if(g_hdTileDumper && SmsHdTileDumper::IsDumpingEnabled()) {
            // Extract palette index from paletteColors
            int paletteIndex = (paletteColors >> 16) & 0xFF;
            
            // Dump the tile
            g_hdTileDumper->DumpTile(tileData, paletteIndex, hMirror, vMirror, x, y, false);
        }
    }

    // Function to be called from SMS VDP during sprite rendering  
    void ProcessSmsSprite(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr,
                         uint8_t* tileData, uint32_t paletteColors) {
        if(!g_isDumping) {
            return;
        }
        
        // Process with HD pack builder if available
        if(g_hdPackBuilder) {
            // Create tile key from the SMS sprite data
            HdTileKeySms tileKey = {};
            tileKey.TileIndex = tileAddr / 32;
            tileKey.IsVramTile = true;
            tileKey.IsSprite = true;
            tileKey.PaletteColors = paletteColors;
            
            // Copy the 32-byte SMS tile data
            memcpy(tileKey.TileData, tileData, 32);
            
            // Process as sprite tile
            uint32_t bankHash = tileAddr / 0x1000;
            g_sprSeen++;
            g_hdPackBuilder->ProcessTile(x, y, tileAddr, tileKey, true, bankHash, false);
        }
        
        // Process with HD tile dumper if available
        if(g_hdTileDumper && SmsHdTileDumper::IsDumpingEnabled()) {
            // SMS sprites always use high palette (0x10-0x1F)
            int paletteIndex = 1; // Sprite palette index (1 = high palette)
            
            // Dump the tile
            g_hdTileDumper->DumpTile(tileData, paletteIndex, false, false, x, y, true);
        }
    }

    // Check if currently dumping
    bool IsCurrentlyDumping() {
        return g_isDumping;
    }
    
    // Enable/disable HD tile dumping
    void SetHdTileDumpingEnabled(bool enabled) {
        SmsHdTileDumper::SetDumpingEnabled(enabled);
    }
    
    // Check if HD tile dumping is enabled
    bool IsHdTileDumpingEnabled() {
        return SmsHdTileDumper::IsDumpingEnabled();
    }
}