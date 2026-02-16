#include "pch.h"
#include "SMS/HdPacks/HdBuilderSmsVdp.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsCpu.h"
#include "SMS/SmsControlManager.h"
#include "SMS/SmsMemoryManager.h"
#include "SMS/SmsVdp.h"
#include "Shared/MessageManager.h"
#include "Utilities/HexUtilities.h"
#include <algorithm>
#include <fstream>

using namespace SmsHdPackSharedConstants;

static void LogToFile([[maybe_unused]] const std::string& msg) {
    // Debug logging disabled for release builds
    // To enable: uncomment below and use a platform-appropriate path
    /*
    static std::ofstream logFile("hd_debug_log.txt", std::ios::app);
    if(logFile.is_open()) {
        logFile << msg << std::endl;
    }
    */
}

namespace {
    constexpr size_t SmsPaletteRamSize = 0x40;

    void CapturePaletteBlock(CapturedPalette& dest, const uint8_t* paletteRam, uint8_t startAddr, uint8_t bytesPerEntry, PaletteFormat format)
    {
        dest.Reset();
        dest.EntryCount = TilePaletteEntryCount;
        dest.BytesPerEntry = bytesPerEntry;
        dest.Format = format;

        size_t bytesToCopy = static_cast<size_t>(dest.EntryCount) * dest.BytesPerEntry;
        size_t maxBytes = startAddr < SmsPaletteRamSize ? SmsPaletteRamSize - startAddr : 0;
        bytesToCopy = std::min(bytesToCopy, maxBytes);
        if(bytesToCopy > 0) {
            memcpy(dest.Data, paletteRam + startAddr, bytesToCopy);
        }
    }

    void CaptureSgPalette(CapturedPalette& dest, uint8_t bgColor, uint8_t fgColor)
    {
        dest.Reset();
        dest.EntryCount = 2;
        dest.BytesPerEntry = 1;
        dest.Format = PaletteFormat::Sg1000;
        dest.Data[0] = bgColor & 0x0F;
        dest.Data[1] = fgColor & 0x0F;
    }

    void CaptureSgSpritePalette(CapturedPalette& dest, uint8_t color)
    {
        dest.Reset();
        dest.EntryCount = 1;
        dest.BytesPerEntry = 1;
        dest.Format = PaletteFormat::Sg1000;
        dest.Data[0] = color & 0x0F;
    }
}

HdBuilderSmsVdp::HdBuilderSmsVdp()
{
    LogToFile("[HD Capture] HdBuilderSmsVdp constructed!");
    MessageManager::Log("[HD Capture] HdBuilderSmsVdp constructed!");
}

void HdBuilderSmsVdp::Init(Emulator* emu, SmsConsole* console, SmsCpu* cpu, SmsControlManager* controlManager, SmsMemoryManager* memoryManager, bool enableHdCapture)
{
    LogToFile("[HD Capture] HdBuilderSmsVdp::Init called! enableHdCapture=" + std::to_string(enableHdCapture));
    SmsVdp::Init(emu, console, cpu, controlManager, memoryManager);
    _hdCaptureEnabled = enableHdCapture;
    MessageManager::Log("[HD Capture] HdBuilderSmsVdp::Init called! enableHdCapture=" + std::to_string(enableHdCapture));
}

void HdBuilderSmsVdp::SetHdBuffers(HdScreenInfoSms* frontBuffer, HdScreenInfoSms* backBuffer)
{
    LogToFile("[HD Capture] SetHdBuffers called");
    _readyBuffer = frontBuffer;
    _captureBuffer = backBuffer;
    MessageManager::Log("[HD Capture] SetHdBuffers called: front=" + std::to_string((uint64_t)frontBuffer) + ", back=" + std::to_string((uint64_t)backBuffer));
}

void HdBuilderSmsVdp::SetHdCaptureEnabled(bool enabled)
{
    _hdCaptureEnabled = enabled;
    LogToFile("[HD Capture] SetHdCaptureEnabled called: enabled=" + std::to_string(enabled));
    MessageManager::Log("[HD Capture] SetHdCaptureEnabled called: enabled=" + std::to_string(enabled));
}

HdScreenInfoSms* HdBuilderSmsVdp::GetCompletedFrame()
{
    return _readyBuffer;
}

HdScreenInfoSms* HdBuilderSmsVdp::SwapBuffersOnFrameEnd()
{
    if(!_hdCaptureEnabled || !_captureBuffer || !_readyBuffer) {
        static int nullCount = 0;
        if(++nullCount <= 5) {
            MessageManager::Log("[HD Capture] SwapBuffers: skipped (enabled=" + std::to_string(_hdCaptureEnabled) +
                ", capture=" + std::to_string((uint64_t)_captureBuffer) +
                ", ready=" + std::to_string((uint64_t)_readyBuffer) + ")");
        }
        return nullptr;
    }

    // VRAM scan disabled: it was causing black sprites (tiles captured before palette init),
    // excessive tile count (all 512 slots scanned every frame), and checkerboard artifacts.
    // Pixel-by-pixel capture handles all visible tiles correctly.
    // TODO: Revisit with a more targeted approach for title/intro tiles if needed.
    // ScanVramTiles();

    // Count pixels with data in the buffer we're about to swap out (the one that was being written to)
    static int swapCount = 0;
    swapCount++;
    if(swapCount <= 10) {
        int bgCount = 0, sprCount = 0;
        for(uint32_t i = 0; i < _captureBuffer->ScreenTiles.size(); i++) {
            if(_captureBuffer->ScreenTiles[i].Background.TileIndex >= 0) bgCount++;
            if(_captureBuffer->ScreenTiles[i].SpriteCount > 0) sprCount++;
        }
        MessageManager::Log("[HD Capture] SwapBuffers #" + std::to_string(swapCount) +
            ": captureBuffer has bgPixels=" + std::to_string(bgCount) +
            ", sprPixels=" + std::to_string(sprCount) +
            ", scanline=" + std::to_string(_state.Scanline));
    }

    std::swap(_captureBuffer, _readyBuffer);
    _captureBuffer->Reset();
    _readyBuffer->FrameNumber++;
    return _readyBuffer;
}

void HdBuilderSmsVdp::LoadBgTilesSms()
{
    // Save _pixelsAvailable BEFORE calling parent — parent's case 6 increments it by 8,
    // but we need the pre-increment value to read _bgPalette/_bgPriority at the correct bit position.
    uint8_t pixelsAvailableBeforeParent = _pixelsAvailable;
    
    // Call parent to load tile data into shifters
    SmsVdp::LoadBgTilesSms();
    
    if(!_hdCaptureEnabled || !_captureBuffer) {
        return;
    }
    
    // Capture happens after cycle 6 when tile data is fully loaded
    uint16_t cycle = _state.Cycle;
    if((cycle & 0x07) == 6) {
        // Diagnostic: log first few tile captures to verify VDP-level capture is working
        static int _bgCaptureCount = 0;
        if(++_bgCaptureCount <= 5) {
            MessageManager::Log("[VDP BgCapture #" + std::to_string(_bgCaptureCount) + 
                "] scanline=" + std::to_string(_state.Scanline) +
                " cycle=" + std::to_string(cycle) +
                " tileIdx=" + std::to_string(_bgTileIndexRaw) +
                " pixAvail=" + std::to_string(pixelsAvailableBeforeParent) +
                " model=" + std::to_string((int)_model) +
                " renderEnabled=" + std::to_string(_state.RenderingEnabled));
        }
        
        // Background tile was just loaded, capture it
        _currentBgTile.Reset();
        _currentBgTile.TileIndex = (int32_t)_bgTileIndexRaw;
        _currentBgTile.TileAddr = _bgTileAddr & ~3; // Align to tile boundary
        _currentBgTile.IsSprite = false;
        _currentBgTile.IsVramTile = true;
        _currentBgTile.IsSg1000Mode = false; // SMS Mode 4
        _currentBgTile.HorizontalMirroring = _bgHorizontalMirror;
        // Use pre-increment _pixelsAvailable to read palette/priority at the correct bit position
        // (parent already incremented _pixelsAvailable by 8 in case 6)
        _currentBgTile.BackgroundPriority = (_bgPriority >> (16 - pixelsAvailableBeforeParent)) & 1;
        _currentBgTile.PaletteIndex = (_bgPalette >> (16 - pixelsAvailableBeforeParent)) & 1;
        
        // Read complete 32-byte tile data
        uint16_t baseTileAddr = (_bgTileIndexRaw * 32) & 0x3FFF;
        for(int i = 0; i < 32; i++) {
            _currentBgTile.TileData[i] = _videoRam[(baseTileAddr + i) & 0x3FFF];
        }
        
        // NES parity: Pack actual palette colors from CRAM (matches SmsVdp::LoadBgTilesSms)
        // For Game Gear: palette is 12-bit (2 bytes per color), so palBase is doubled
        // For SMS: palette is 6-bit (1 byte per color)
        if(_model == SmsModel::GameGear) {
            // Game Gear: 2 bytes per color, palBase is color index * 2
            uint8_t palBase = _currentBgTile.PaletteIndex ? 0x20 : 0x00;
            CapturePaletteBlock(_currentBgTile.CapturedPalette, _paletteRam, palBase, 2, PaletteFormat::GameGear);
            // Legacy packed colors (first 4 entries) for backwards compatibility
            _currentBgTile.PaletteColors =
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 2] << 8) |
                ((uint32_t)_paletteRam[palBase + 4] << 16) |
                ((uint32_t)_paletteRam[palBase + 6] << 24);
        } else {
            // SMS: 1 byte per color
            uint8_t palBase = _currentBgTile.PaletteIndex ? 0x10 : 0x00;
            CapturePaletteBlock(_currentBgTile.CapturedPalette, _paletteRam, palBase, 1, PaletteFormat::Sms);
            _currentBgTile.PaletteColors =
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 1] << 8) |
                ((uint32_t)_paletteRam[palBase + 2] << 16) |
                ((uint32_t)_paletteRam[palBase + 3] << 24);
        }
    }
}

void HdBuilderSmsVdp::LoadSpriteTilesSms()
{
    // Sprites are loaded at cycles 264-325
    uint16_t cycle = _state.Cycle - 264;
    
    // IMPORTANT: Capture sprite index BEFORE calling parent, because parent increments _spriteCount
    // at cycles 10, 22, 44, 56 which would give us wrong indices
    int sprIndexBeforeParent = -1;
    if(cycle == 6 || cycle == 18 || cycle == 40 || cycle == 52) {
        sprIndexBeforeParent = _spriteCount;  // Sprite N
    } else if(cycle == 10 || cycle == 22 || cycle == 44 || cycle == 56) {
        sprIndexBeforeParent = _spriteCount + 1;  // Sprite N+1 (before parent increments by 2)
    }
    
    // Call parent to load sprite tile data (this may increment _spriteCount)
    SmsVdp::LoadSpriteTilesSms();
    
    if(!_hdCaptureEnabled || !_captureBuffer) {
        return;
    }
    
    // At cycle 0, reset sprite capture for this scanline's sprites
    // These sprites will be used for the NEXT scanline's DrawPixel calls
    if(cycle == 0) {
        for(uint8_t i = 0; i < 8; i++) {
            _loadingSpriteTiles[i].Reset();
            _loadingSpriteX[i] = 0;
        }
        _loadingSpriteCount = 0;
    }
    
    // Use the sprite index we captured before calling parent
    int sprIndex = sprIndexBeforeParent;
    
    if(sprIndex >= 0 && sprIndex < 8) {
        // Capture sprite tile data
        // For tall sprites (16 pixels), we capture based on which row we're rendering
        uint8_t spriteRow = _spriteShifters[sprIndex].SpriteRow;
        uint16_t tileIndex = _spriteShifters[sprIndex].RawTileIndex;
        
        // For tall sprites, if we're in the bottom half, use the next tile
        if(_state.UseLargeSprites && spriteRow >= 8) {
            tileIndex = (tileIndex + 1) & 0xFF;
        }
        
        // Calculate tile address using pattern selector base (matches VDP's calculation)
        uint16_t patternBase = _state.SpritePatternSelector & 0x2000;
        uint16_t tileAddr = patternBase | (tileIndex << 5);
        
        HdSmsTileInfo& spriteTile = _loadingSpriteTiles[sprIndex];
        spriteTile.Reset();
        spriteTile.TileIndex = (int32_t)(tileAddr / 32);  // Tile index for the current tile
        spriteTile.TileAddr = tileAddr;
        spriteTile.IsSprite = true;
        spriteTile.IsVramTile = true;
        spriteTile.PaletteIndex = 1; // Sprites always use high palette
        
        // Read complete 32-byte tile data from VRAM
        for(int i = 0; i < 32; i++) {
            spriteTile.TileData[i] = _videoRam[(tileAddr + i) & 0x3FFF];
        }
        
        // NES parity: Pack actual sprite palette colors from CRAM (high palette)
        // For Game Gear: palette is 12-bit (2 bytes per color), sprite palette starts at 0x20
        // For SMS: palette is 6-bit (1 byte per color), sprite palette starts at 0x10
        if(_model == SmsModel::GameGear) {
            // Game Gear: 2 bytes per color, sprite palette at 0x20-0x3F
            CapturePaletteBlock(spriteTile.CapturedPalette, _paletteRam, 0x20, 2, PaletteFormat::GameGear);
            spriteTile.PaletteColors =
                ((uint32_t)_paletteRam[0x20]) |
                ((uint32_t)_paletteRam[0x22] << 8) |
                ((uint32_t)_paletteRam[0x24] << 16) |
                ((uint32_t)_paletteRam[0x26] << 24);
        } else {
            // SMS: 1 byte per color, sprite palette at 0x10-0x1F
            CapturePaletteBlock(spriteTile.CapturedPalette, _paletteRam, 0x10, 1, PaletteFormat::Sms);
            spriteTile.PaletteColors =
                ((uint32_t)_paletteRam[0x10]) |
                ((uint32_t)_paletteRam[0x11] << 8) |
                ((uint32_t)_paletteRam[0x12] << 16) |
                ((uint32_t)_paletteRam[0x13] << 24);
        }
        
        // Capture sprite X position for double-buffering
        _loadingSpriteX[sprIndex] = (int16_t)_spriteShifters[sprIndex].SpriteX;
        
        if(sprIndex >= (int)_loadingSpriteCount) {
            _loadingSpriteCount = sprIndex + 1;
        }
    }
}

void HdBuilderSmsVdp::DrawPixel()
{
    // Call parent to actually draw the pixel
    SmsVdp::DrawPixel();
    
    if(!_hdCaptureEnabled || !_captureBuffer) {
        return;
    }
    
    // Only capture during visible scanlines
    if(_state.Scanline >= 240) {
        return;
    }
    
    uint16_t x = GetVisiblePixelIndex();
    if(x >= 256) {
        return;
    }
    
    uint32_t pixelIndex = _state.Scanline * 256 + x;
    if(pixelIndex >= _captureBuffer->ScreenTiles.size()) {
        return;
    }
    
    HdSmsPixelInfo& pixel = _captureBuffer->ScreenTiles[pixelIndex];
    
    // When rendering is disabled (forced blank), LoadBgTilesSms is not called,
    // so _currentBgTile is stale. Capture tile data directly from nametable.
    if(!_state.RenderingEnabled && _state.UseMode4 && _currentBgTile.TileIndex < 0) {
        // Calculate which tile we're on based on pixel position and scroll
        uint8_t scrollX = _state.HorizontalScrollLatch;
        uint8_t scrollY = _state.VerticalScrollLatch;
        uint16_t scrolledX = (x + scrollX) & 0xFF;
        uint16_t scrolledY = (_state.Scanline + scrollY) % 224;
        
        uint8_t tileCol = scrolledX >> 3;
        uint8_t tileRow = scrolledY >> 3;
        
        // Read nametable entry
        uint16_t ntBase = _state.EffectiveNametableAddress;
        uint16_t ntAddr = ntBase + (tileCol + tileRow * 32) * 2;
        uint16_t ntData = _videoRam[ntAddr & 0x3FFF] | (_videoRam[(ntAddr + 1) & 0x3FFF] << 8);
        
        uint16_t tileIndex = ntData & 0x1FF;
        uint8_t paletteIdx = (ntData & 0x800) ? 1 : 0;
        bool hMirror = (ntData & 0x200) != 0;
        bool vMirror = (ntData & 0x400) != 0;
        bool priority = (ntData & 0x1000) != 0;
        
        // Populate _currentBgTile for this pixel
        _currentBgTile.Reset();
        _currentBgTile.TileIndex = (int32_t)tileIndex;
        _currentBgTile.TileAddr = (tileIndex * 32) & 0x3FFF;
        _currentBgTile.IsSprite = false;
        _currentBgTile.IsVramTile = true;
        _currentBgTile.IsSg1000Mode = false;
        _currentBgTile.HorizontalMirroring = hMirror;
        _currentBgTile.VerticalMirroring = vMirror;
        _currentBgTile.BackgroundPriority = priority;
        _currentBgTile.PaletteIndex = paletteIdx;
        
        // Read tile data
        uint16_t baseTileAddr = (tileIndex * 32) & 0x3FFF;
        for(int i = 0; i < 32; i++) {
            _currentBgTile.TileData[i] = _videoRam[(baseTileAddr + i) & 0x3FFF];
        }
        
        // Capture palette
        if(_model == SmsModel::GameGear) {
            uint8_t palBase = paletteIdx ? 0x20 : 0x00;
            CapturePaletteBlock(_currentBgTile.CapturedPalette, _paletteRam, palBase, 2, PaletteFormat::GameGear);
            _currentBgTile.PaletteColors =
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 2] << 8) |
                ((uint32_t)_paletteRam[palBase + 4] << 16) |
                ((uint32_t)_paletteRam[palBase + 6] << 24);
        } else {
            uint8_t palBase = paletteIdx ? 0x10 : 0x00;
            CapturePaletteBlock(_currentBgTile.CapturedPalette, _paletteRam, palBase, 1, PaletteFormat::Sms);
            _currentBgTile.PaletteColors =
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 1] << 8) |
                ((uint32_t)_paletteRam[palBase + 2] << 16) |
                ((uint32_t)_paletteRam[palBase + 3] << 24);
        }
    }
    
    // Store background tile for this pixel
    if(_currentBgTile.TileIndex >= 0) {
        pixel.Background = _currentBgTile;
    }
    
    // Store sprite tiles for this pixel
    pixel.SpriteCount = 0;

    // Use active sprite buffer (populated from previous scanline's loading)
    for(uint8_t i = 0; i < _activeSpriteCount && pixel.SpriteCount < 4; i++) {
        if(_activeSpriteTiles[i].TileIndex >= 0) {
            // Check if this sprite is actually visible at this X position
            // Use the BUFFERED sprite X position (captured during loading)
            // This is critical because _spriteShifters[i].SpriteX changes during sprite loading for the next scanline
            int16_t spriteX = _activeSpriteX[i];
            uint16_t spriteWidth = 8 << (uint8_t)IsZoomedSpriteAllowed(i);
            
            // Match VDP's sprite visibility check (handles negative X from shift)
            if((int16_t)x >= spriteX && (int16_t)x < spriteX + (int16_t)spriteWidth) {
                pixel.Sprites[pixel.SpriteCount++] = _activeSpriteTiles[i];
            }
        }
    }
    
    // Store scroll values
    pixel.ScrollX = _state.HorizontalScrollLatch;
    pixel.ScrollY = _state.VerticalScrollLatch;
}

void HdBuilderSmsVdp::ProcessEndOfScanline()
{
    // Call parent first
    SmsVdp::ProcessEndOfScanline();
    
    if(_hdCaptureEnabled) {
        _currentBgTile.Reset();
        
        // Swap sprite buffers: loading -> active
        // Sprites loaded during this scanline's hblank will be used for NEXT scanline's DrawPixel
        for(uint8_t i = 0; i < 8; i++) {
            _activeSpriteTiles[i] = _loadingSpriteTiles[i];
            _activeSpriteX[i] = _loadingSpriteX[i];
        }
        _activeSpriteCount = _loadingSpriteCount;
    }
}

// SG-1000 / TMS9918 Mode Capture Support
// ============================================================================

void HdBuilderSmsVdp::LoadBgTilesSg()
{
    // Call parent to load tile data
    SmsVdp::LoadBgTilesSg();
    
    if(!_hdCaptureEnabled || !_captureBuffer) {
        return;
    }
    
    // SG-1000 tiles are loaded at cycle & 0x07 == 6 (same as SMS)
    uint16_t cycle = _state.Cycle;
    if((cycle & 0x07) != 6) {
        return;
    }
    
    // Skip text mode for now (Mode 1) - it uses 6x8 characters
    if(_state.M1_Use224LineMode) {
        return;
    }
    
    // Capture background tile
    _currentBgTile.Reset();
    _currentBgTile.TileIndex = (int32_t)_bgTileIndex;
    _currentBgTile.IsSprite = false;
    _currentBgTile.IsVramTile = true;
    _currentBgTile.PaletteIndex = 0; // SG-1000 uses fixed palette
    _currentBgTile.IsSg1000Mode = true; // Mark as SG-1000 tile
    
    // Calculate base pattern address (without row offset) for full 8-byte tile read
    uint16_t patternAddr;
    if(_state.M3_Use240LineMode) {
        patternAddr = (_state.BgPatternTableAddress & 0x3800) + (_bgTileIndex * 8);
    } else if(_state.M2_AllowHeightChange) {
        uint16_t mask = ((_state.BgPatternTableAddress >> 3) | 0xFF) & 0x3FF;
        patternAddr = (_state.BgPatternTableAddress & 0x2000) | ((_bgTileIndex & mask) * 8);
    } else {
        patternAddr = (_state.BgPatternTableAddress & 0x3800) + (_bgTileIndex * 8);
    }
    
    // SG-1000 TileData layout (32 bytes):
    //   [0..7]   = 8-byte pattern data (1 bit per pixel, 8 rows)
    //   [8..31]  = zeroed (must match base VDP renderer for hash consistency)
    // Per-row color bytes are stored in CapturedPalette (not TileData) so they
    // don't affect the tile hash but are available for GenerateHdTile.
    
    // Read 8-byte tile pattern
    for(int i = 0; i < 8; i++) {
        _currentBgTile.TileData[i] = _videoRam[(patternAddr + i) & 0x3FFF];
    }
    // Zero remaining bytes (must match renderer's TileData layout for hash match)
    for(int i = 8; i < 32; i++) {
        _currentBgTile.TileData[i] = 0;
    }
    
    // Read all 8 row color bytes from the color table into CapturedPalette
    _currentBgTile.CapturedPalette.Reset();
    _currentBgTile.CapturedPalette.EntryCount = 8;
    _currentBgTile.CapturedPalette.BytesPerEntry = 1;
    _currentBgTile.CapturedPalette.Format = SmsHdPackSharedConstants::PaletteFormat::Sg1000;
    for(int row = 0; row < 8; row++) {
        uint8_t colorByte = 0;
        if(_state.M3_Use240LineMode) {
            colorByte = _videoRam[(patternAddr + row) & 0x3FFF];
        } else if(_state.M2_AllowHeightChange) {
            uint16_t colorMask = ((_state.ColorTableAddress >> 3) | 0x07) & 0x3FF;
            uint16_t colorAddr = (_state.ColorTableAddress & 0x2000) | ((_bgTileIndex & colorMask) << 3) + row;
            colorByte = _videoRam[colorAddr & 0x3FFF];
        } else {
            uint16_t colorAddr = (_state.ColorTableAddress & 0x3FC0) | ((_bgTileIndex >> 3) & 0x1F);
            colorByte = _videoRam[colorAddr & 0x3FFF];
        }
        _currentBgTile.CapturedPalette.Data[row] = colorByte;
    }
    
    // PaletteColors: compute hash of all 8 row colors for proper deduplication
    // SG-1000 Mode 2 has per-row colors, so tiles with same pattern but different
    // row colors must be treated as different tiles. Pack a simple hash into uint32_t.
    uint32_t colorHash = 0;
    for(int i = 0; i < 8; i++) {
        colorHash = (colorHash * 31) + _currentBgTile.CapturedPalette.Data[i];
    }
    _currentBgTile.PaletteColors = colorHash;
    _currentBgTile.TileAddr = patternAddr;
}

void HdBuilderSmsVdp::LoadSpriteTilesSg()
{
    // Call parent to load sprite data
    SmsVdp::LoadSpriteTilesSg();
    
    if(!_hdCaptureEnabled || !_captureBuffer) {
        return;
    }
    
    // Skip text mode - no sprites
    if(_state.M1_Use224LineMode) {
        return;
    }
    
    uint16_t cycle = _state.Cycle - 264;
    
    // Reset sprite capture at start of sprite loading phase
    if(cycle == 0) {
        for(uint8_t i = 0; i < 8; i++) {
            _loadingSpriteTiles[i].Reset();
            _loadingSpriteX[i] = 0;
        }
        _loadingSpriteCount = 0;
    }
    
    // SG-1000 sprites are captured at cycles 10, 22, 44, 56 (after tile data is loaded)
    // Each sprite takes 12 cycles, and we capture 4 sprites
    int sprIndex = -1;
    if(cycle == 10) sprIndex = 0;
    else if(cycle == 22) sprIndex = 1;
    else if(cycle == 44) sprIndex = 2;
    else if(cycle == 56) sprIndex = 3;
    
    if(sprIndex < 0 || sprIndex >= 4) {
        return;
    }
    
    // Capture sprite tile data
    HdSmsTileInfo& spriteTile = _loadingSpriteTiles[sprIndex];
    spriteTile.Reset();
    spriteTile.IsSprite = true;
    spriteTile.IsVramTile = true;
    spriteTile.PaletteIndex = 1; // Sprites use "high" palette conceptually
    spriteTile.IsSg1000Mode = true; // Mark as SG-1000 sprite
    
    // Calculate base pattern address (without row offset) for proper hash matching
    // TileAddr includes row offset, but we need the base address for all 8 rows
    uint16_t patternBase = _state.SpritePatternSelector & 0x3800;
    uint16_t tileIndex = _spriteShifters[sprIndex].RawTileIndex;
    uint16_t tileBaseAddr = patternBase | (tileIndex << 3);
    
    // SG-1000 sprites are 8 bytes (or 32 bytes for 16x16)
    // Read the full 8-byte pattern (all rows)
    for(int i = 0; i < 8; i++) {
        spriteTile.TileData[i] = _videoRam[(tileBaseAddr + i) & 0x3FFF];
    }
    // Zero remaining bytes
    for(int i = 8; i < 32; i++) {
        spriteTile.TileData[i] = 0;
    }
    
    // Sprite color is stored in TileData[1] by the parent (attribute byte)
    // The color is in the low 4 bits
    uint8_t spriteColor = _spriteShifters[sprIndex].TileData[1] & 0x0F;
    spriteTile.PaletteColors = spriteColor;
    spriteTile.TileAddr = tileBaseAddr;
    spriteTile.TileIndex = (int32_t)tileIndex;
    
    // Capture SG-1000 sprite palette (single color)
    CaptureSgSpritePalette(spriteTile.CapturedPalette, spriteColor);
    
    // Capture sprite X position for double-buffering
    _loadingSpriteX[sprIndex] = (int16_t)_spriteShifters[sprIndex].SpriteX;
    
    if(sprIndex >= (int)_loadingSpriteCount) {
        _loadingSpriteCount = sprIndex + 1;
    }
}

// ============================================================================
// VRAM Scan: Capture ALL tiles in the full 16KB VRAM pattern space.
// This scans all 512 tile slots (indices 0-511), not just tiles referenced
// by the nametable or sprite table. This ensures animated intro tiles,
// transition tiles, and tiles loaded for upcoming frames are captured even
// if they aren't currently displayed.
//
// Populates ExtraBgTiles/ExtraSpriteTiles vectors (processed separately by
// ProcessFrame) so pixel-by-pixel capture data is never disturbed.
// ============================================================================

void HdBuilderSmsVdp::ScanVramTiles()
{
    if(!_hdCaptureEnabled || !_captureBuffer || !_state.UseMode4) {
        return; // Only Mode 4 (SMS/GG) for now
    }

    // --- Build a set of tile indices referenced by the nametable ---
    // so we can assign the correct palette from the nametable entry.
    // Tiles NOT in the nametable get captured with both palettes.
    std::unordered_map<uint16_t, uint8_t> ntPaletteMap; // tileIndex -> paletteIdx
    uint16_t ntBase = _state.EffectiveNametableAddress;
    uint16_t ntMask = _state.NametableAddressMask;
    uint8_t visibleRows = _state.VisibleScanlineCount / 8;

    for(uint8_t row = 0; row < visibleRows; row++) {
        for(uint8_t col = 0; col < 32; col++) {
            uint16_t ntAddr = (ntBase + (col + row * 32) * 2) & ntMask;
            uint16_t ntData = _videoRam[ntAddr & 0x3FFF] | (_videoRam[(ntAddr + 1) & 0x3FFF] << 8);
            uint16_t tileIndex = ntData & 0x1FF;
            uint8_t paletteIdx = (ntData & 0x800) ? 1 : 0;
            ntPaletteMap[tileIndex] = paletteIdx;
        }
    }

    // --- Build a set of tile indices referenced by the sprite table ---
    std::unordered_set<uint16_t> spriteTileIndices;
    uint16_t spriteAddr = _state.SpriteTableAddress & 0x3F00;
    uint16_t patternBase = _state.SpritePatternSelector & 0x2000;

    for(int i = 0; i < 64; i++) {
        uint8_t spriteY = _videoRam[(spriteAddr + i) & 0x3FFF];
        if(spriteY == 0xD0 && _state.VisibleScanlineCount <= 192) {
            break;
        }
        uint16_t attrAddr = spriteAddr + 0x80 + i * 2;
        uint8_t sprTileIndex = _videoRam[(attrAddr + 1) & 0x3FFF];
        if(_state.UseLargeSprites) {
            sprTileIndex &= ~0x01;
        }
        int tilesToCapture = _state.UseLargeSprites ? 2 : 1;
        for(int t = 0; t < tilesToCapture; t++) {
            uint16_t curTileIndex = (sprTileIndex + t) & 0xFF;
            uint16_t globalIdx = (uint16_t)((patternBase | (curTileIndex << 5)) / 32);
            spriteTileIndices.insert(globalIdx);
        }
    }

    // Helper lambda to create and push a tile entry
    auto pushTile = [&](uint16_t tileIndex, uint8_t paletteIdx, bool isSprite) {
        uint16_t baseTileAddr = (tileIndex * 32) & 0x3FFF;

        // Note: We capture ALL tiles including blank/solid color tiles.
        // Blank tiles with all-zero pattern data render as solid palette color 0,
        // and must be captured for proper HD replacement.

        HdSmsTileInfo tile;
        tile.Reset();
        tile.TileIndex = (int32_t)tileIndex;
        tile.TileAddr = baseTileAddr;
        tile.IsSprite = isSprite;
        tile.IsVramTile = true;
        tile.IsSg1000Mode = false;
        tile.PaletteIndex = paletteIdx;

        for(int i = 0; i < 32; i++) {
            tile.TileData[i] = _videoRam[(baseTileAddr + i) & 0x3FFF];
        }

        if(_model == SmsModel::GameGear) {
            uint8_t palBase = paletteIdx ? 0x20 : 0x00;
            CapturePaletteBlock(tile.CapturedPalette, _paletteRam, palBase, 2, PaletteFormat::GameGear);
            tile.PaletteColors =
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 2] << 8) |
                ((uint32_t)_paletteRam[palBase + 4] << 16) |
                ((uint32_t)_paletteRam[palBase + 6] << 24);
        } else {
            uint8_t palBase = paletteIdx ? 0x10 : 0x00;
            CapturePaletteBlock(tile.CapturedPalette, _paletteRam, palBase, 1, PaletteFormat::Sms);
            tile.PaletteColors =
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 1] << 8) |
                ((uint32_t)_paletteRam[palBase + 2] << 16) |
                ((uint32_t)_paletteRam[palBase + 3] << 24);
        }

        if(isSprite) {
            _captureBuffer->ExtraSpriteTiles.push_back(tile);
        } else {
            _captureBuffer->ExtraBgTiles.push_back(tile);
        }
    };

    // --- Scan ALL 512 tile slots in the 16KB VRAM pattern space ---
    for(uint16_t tileIndex = 0; tileIndex < 512; tileIndex++) {
        // If this tile is referenced by the sprite table, capture as sprite
        if(spriteTileIndices.count(tileIndex)) {
            pushTile(tileIndex, 1, true);
        }

        // If this tile is referenced by the nametable, capture as BG with correct palette
        auto ntIt = ntPaletteMap.find(tileIndex);
        if(ntIt != ntPaletteMap.end()) {
            pushTile(tileIndex, ntIt->second, false);
        }

        // For tiles NOT referenced by either table, capture as BG with palette 0 only
        // This catches tiles loaded for upcoming animation frames without doubling tile count
        if(!spriteTileIndices.count(tileIndex) && ntPaletteMap.find(tileIndex) == ntPaletteMap.end()) {
            pushTile(tileIndex, 0, false);
        }
    }
}