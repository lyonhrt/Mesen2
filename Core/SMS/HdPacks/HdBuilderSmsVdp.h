#pragma once
#include "SMS/SmsVdp.h"
#include "SMS/HdPacks/HdDataSms.h"

class HdBuilderSmsVdp final : public SmsVdp
{
public:
    HdBuilderSmsVdp();
    ~HdBuilderSmsVdp() override = default;

    void Init(Emulator* emu, SmsConsole* console, SmsCpu* cpu, SmsControlManager* controlManager, SmsMemoryManager* memoryManager, bool enableHdCapture);

    void SetHdBuffers(HdScreenInfoSms* frontBuffer, HdScreenInfoSms* backBuffer);
    HdScreenInfoSms* GetCompletedFrame();
    HdScreenInfoSms* SwapBuffersOnFrameEnd();
    void SetHdCaptureEnabled(bool enabled);
    bool IsHdCaptureEnabled() const { return _hdCaptureEnabled; }
    
    // Override to capture tile data during rendering (SMS/GG Mode 4)
    void DrawPixel() override;
    void LoadBgTilesSms() override;
    void LoadSpriteTilesSms() override;
    void ProcessEndOfScanline() override;
    
    // Override for SG-1000 capture support (TMS9918 modes)
    void LoadBgTilesSg() override;
    void LoadSpriteTilesSg() override;
    
    // VRAM scan: capture ALL tiles referenced by nametable and sprite table
    // Runs at frame end to ensure animated/transitional tiles aren't missed
    void ScanVramTiles();
    void ScanVramTilesSg();

private:
    bool _hdCaptureEnabled = false;
    HdScreenInfoSms* _captureBuffer = nullptr;
    HdScreenInfoSms* _readyBuffer = nullptr;
    
    // Temporary storage for current tile being loaded
    HdSmsTileInfo _currentBgTile;
    
    // Double-buffered sprite tiles: sprites loaded during scanline N are used for scanline N+1
    HdSmsTileInfo _loadingSpriteTiles[8];  // Being loaded during current scanline's hblank
    HdSmsTileInfo _activeSpriteTiles[8];   // Used by DrawPixel for current scanline
    int16_t _loadingSpriteX[8];            // Sprite X positions being loaded
    int16_t _activeSpriteX[8];             // Sprite X positions for current scanline
    uint8_t _loadingSpriteCount = 0;
    uint8_t _activeSpriteCount = 0;
};