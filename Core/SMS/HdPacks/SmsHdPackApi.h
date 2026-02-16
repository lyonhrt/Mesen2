#pragma once
#include "pch.h"

class Emulator;
class SmsConsole;
struct HdPackBuilderOptions;
struct HdPackDataSms;

// Simple API for SMS HD pack dumping - auto-start and save on power-off
namespace SmsHdPackApi {
    // Auto-start function called when SMS game is loaded
    void AutoStartOnGameLoaded(Emulator* emu);
    
    // Save and stop dumping when the game is powered off
    void SaveOnPowerOff(Emulator* emu);
    
    // Explicit start/stop recording controls (called by Start/Stop Recording shortcuts)
    void StartRecording(Emulator* emu, HdPackBuilderOptions options);
    void StopRecording(Emulator* emu);
    
    // Frame-based capture integration (called when a frame is ready)
    void ProcessFrameIfReady(Emulator* emu);

    // Check if currently dumping
    bool IsCurrentlyDumping();

    // Called after a power cycle recreates the console - re-attaches builder to new console/VDP
    void OnConsoleRecreated(Emulator* emu);

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

    // Check if HD pack is loaded and ready for rendering
    bool IsHdPackLoaded();

    // Lookup replacement by VRAM tile index/palette group
    // palGroup: 0 = BG low palette, 1 = BG high or any sprite
    bool TryGetReplacementByIndex(uint32_t tileIndex, bool isSprite, uint8_t palGroup,
                                  const uint8_t* tileData32,
                                  int& imgIndex, uint16_t& srcX, uint16_t& srcY, uint32_t& scale);

    // Lookup replacement by canonical hash of (tileData[32], palGroup, isSprite, paletteColors)
    bool TryGetReplacementByHash(const uint8_t* tileData32, bool isSprite, uint8_t palGroup,
                                 int& imgIndex, uint16_t& srcX, uint16_t& srcY, uint32_t& scale,
                                 uint32_t paletteColors = 0);

    // Sample 8 pixels from a replacement row for a given tile row
    // - imgIndex/srcX/srcY/scale come from TryGetReplacement*
    // - rowWithinTile: 0..7 (the row inside the 8x8 tile)
    // - hMirror: horizontally mirror the 8 pixels when true
    // - outPixels8: returns 8 RGB555 pixels ready for the output buffer
    bool SampleReplacementRow8(int imgIndex, uint16_t srcX, uint16_t srcY, uint32_t scale,
                               uint8_t rowWithinTile, bool hMirror, uint16_t* outPixels8);
    
    // Get image data for direct pixel access
    // Returns pointer to ARGB pixel data and dimensions
    bool GetImageData(int imgIndex, const uint32_t*& outPixels, uint32_t& outWidth, uint32_t& outHeight);
    
    // Get fade brightness for a tile (255 = no fade, <255 = faded).
    // Call after TryGetReplacementByHash succeeds to check if brightness adjustment is needed.
    // When the current palette is dimmer than the base palette, returns a proportional brightness value.
    uint8_t GetFadeBrightness(const uint8_t* tileData32, bool isSprite, uint32_t paletteColors);

    // Get the HD pack scale
    uint32_t GetHdPackScale();
    
    // Get the loaded HD pack data (for use by SmsHdPack renderer)
    // Returns nullptr if no pack is loaded
    struct HdPackDataSms* GetHdPackData();
}