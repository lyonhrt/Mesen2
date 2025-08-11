#pragma once
#include "pch.h"

class Emulator;
class SmsConsole;
struct HdPackBuilderOptions;

// Simple API for SMS HD pack dumping - auto-start and save on power-off
namespace SmsHdPackApi {
    // Auto-start function called when SMS game is loaded
    void AutoStartOnGameLoaded(Emulator* emu);
    
    // Save and stop dumping when the game is powered off
    void SaveOnPowerOff(Emulator* emu);
    
    // Explicit start/stop recording controls (called by Start/Stop Recording shortcuts)
    void StartRecording(Emulator* emu, HdPackBuilderOptions options);
    void StopRecording(Emulator* emu);
    
    // Real VDP integration functions - called from SmsVdp during rendering
    void ProcessSmsBackgroundTile(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr, 
                                 uint8_t* tileData, uint32_t paletteColors, bool hMirror, bool vMirror, bool priority);
    void ProcessSmsSprite(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr,
                         uint8_t* tileData, uint32_t paletteColors);
                         
    // Check if currently dumping
    bool IsCurrentlyDumping();

    // Enable/disable HD tile dumping
    void SetHdTileDumpingEnabled(bool enabled);

    // Check if HD tile dumping is enabled
    bool IsHdTileDumpingEnabled();
}