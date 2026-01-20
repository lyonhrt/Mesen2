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

private:
    bool _hdCaptureEnabled = false;
    HdScreenInfoSms* _captureBuffer = nullptr;
    HdScreenInfoSms* _readyBuffer = nullptr;
    
    // Temporary storage for current tile being loaded
    HdSmsTileInfo _currentBgTile;
    HdSmsTileInfo _currentSpriteTiles[8]; // Max 8 sprites
    uint8_t _currentSpriteCount = 0;
};