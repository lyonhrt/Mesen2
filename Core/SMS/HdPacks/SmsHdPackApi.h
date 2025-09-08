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

    // HD replacement (loader)
    // Load SMS HD pack (hires.txt + PNGs) if present in HdPacks/<RomName>/ on game load
    void LoadHdPackIfAvailable(Emulator* emu);
    // Enable/disable HD tile replacement (does not affect tile dumping)
    void SetHdReplacementEnabled(bool enabled);
    bool IsHdReplacementEnabled();

    // Lookup replacement by VRAM tile index/palette group
    // palGroup: 0 = BG low palette, 1 = BG high or any sprite
    bool TryGetReplacementByIndex(uint32_t tileIndex, bool isSprite, uint8_t palGroup,
                                  const uint8_t* tileData32,
                                  int& imgIndex, uint16_t& srcX, uint16_t& srcY, uint32_t& scale);

    // Lookup replacement by canonical hash of (tileData[32], palGroup, isSprite)
    bool TryGetReplacementByHash(const uint8_t* tileData32, bool isSprite, uint8_t palGroup,
                                 int& imgIndex, uint16_t& srcX, uint16_t& srcY, uint32_t& scale);

    // Sample 8 pixels from a replacement row for a given tile row
    // - imgIndex/srcX/srcY/scale come from TryGetReplacement*
    // - rowWithinTile: 0..7 (the row inside the 8x8 tile)
    // - hMirror: horizontally mirror the 8 pixels when true
    // - outPixels8: returns 8 RGB555 pixels ready for the output buffer
    bool SampleReplacementRow8(int imgIndex, uint16_t srcX, uint16_t srcY, uint32_t scale,
                               uint8_t rowWithinTile, bool hMirror, uint16_t* outPixels8);
}