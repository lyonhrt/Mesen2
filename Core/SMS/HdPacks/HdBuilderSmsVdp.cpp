#include "pch.h"
#include "SMS/HdPacks/HdBuilderSmsVdp.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsCpu.h"
#include "SMS/SmsControlManager.h"
#include "SMS/SmsMemoryManager.h"
#include "SMS/SmsVdp.h"
#include "Shared/MessageManager.h"
#include "Utilities/HexUtilities.h"
#include <fstream>

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
        return nullptr;
    }

    std::swap(_captureBuffer, _readyBuffer);
    _captureBuffer->Reset();
    _readyBuffer->FrameNumber++;
    return _readyBuffer;
}

void HdBuilderSmsVdp::LoadBgTilesSms()
{
    // Call parent to load tile data into shifters
    SmsVdp::LoadBgTilesSms();
    
    // Debug: Log first few calls
    static int loadBgCallCount = 0;
    if(loadBgCallCount++ < 3) {
        LogToFile("[HD Capture] LoadBgTilesSms called: enabled=" + std::to_string(_hdCaptureEnabled) + 
                           ", hasBuffer=" + std::to_string(_captureBuffer != nullptr));
    }
    
    if(!_hdCaptureEnabled || !_captureBuffer) {
        return;
    }
    
    // Capture happens after cycle 6 when tile data is fully loaded
    uint16_t cycle = _state.Cycle;
    if((cycle & 0x07) == 6) {
        // Background tile was just loaded, capture it
        _currentBgTile.Reset();
        _currentBgTile.TileIndex = (int32_t)_bgTileIndexRaw;
        _currentBgTile.TileAddr = _bgTileAddr & ~3; // Align to tile boundary
        _currentBgTile.IsSprite = false;
        _currentBgTile.IsVramTile = true;
        _currentBgTile.HorizontalMirroring = _bgHorizontalMirror;
        _currentBgTile.BackgroundPriority = (_bgPriority & 0x800000) != 0;
        _currentBgTile.PaletteIndex = (_bgPalette & 0x800000) ? 1 : 0;
        
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
            _currentBgTile.PaletteColors = 
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 1] << 8) |
                ((uint32_t)_paletteRam[palBase + 2] << 16) |
                ((uint32_t)_paletteRam[palBase + 3] << 24);
        } else {
            // SMS: 1 byte per color
            uint8_t palBase = _currentBgTile.PaletteIndex ? 0x10 : 0x00;
            _currentBgTile.PaletteColors = 
                ((uint32_t)_paletteRam[palBase + 0]) |
                ((uint32_t)_paletteRam[palBase + 1] << 8) |
                ((uint32_t)_paletteRam[palBase + 2] << 16) |
                ((uint32_t)_paletteRam[palBase + 3] << 24);
        }
        
        // Debug: Verify tile data is not all zeros
        static int bgLogCount = 0;
        if(bgLogCount++ < 5) {
            bool allZero = true;
            for(int i = 0; i < 32; i++) {
                if(_currentBgTile.TileData[i] != 0) {
                    allZero = false;
                    break;
                }
            }
            LogToFile("[HD Capture] BG tile loaded: tileIndex=" + std::to_string(_currentBgTile.TileIndex) + 
                               ", addr=0x" + HexUtilities::ToHex(_currentBgTile.TileAddr) + 
                               ", palIdx=" + std::to_string(_currentBgTile.PaletteIndex) + 
                               ", allZero=" + std::string(allZero ? "YES" : "NO") + 
                               ", scanline=" + std::to_string(_state.Scanline));
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
    
    // Debug: Log first few calls
    static int loadSpriteCallCount = 0;
    if(loadSpriteCallCount++ < 3) {
        LogToFile("[HD Capture] LoadSpriteTilesSms called: enabled=" + std::to_string(_hdCaptureEnabled) + 
                           ", hasBuffer=" + std::to_string(_captureBuffer != nullptr));
    }
    
    if(!_hdCaptureEnabled || !_captureBuffer) {
        return;
    }
    
    // At cycle 0, reset sprite capture for this scanline's sprites
    // These sprites will be used for the NEXT scanline's DrawPixel calls
    if(cycle == 0) {
        for(uint8_t i = 0; i < 8; i++) {
            _currentSpriteTiles[i].Reset();
        }
        _currentSpriteCount = 0;
    }
    
    // Use the sprite index we captured before calling parent
    int sprIndex = sprIndexBeforeParent;
    
    if(sprIndex >= 0 && sprIndex < 8) {
        // Capture sprite tile data
        // For tall sprites (16 pixels), we capture based on which row we're rendering
        uint8_t spriteRow = _spriteShifters[sprIndex].SpriteRow;
        uint16_t baseTileAddr = (_spriteShifters[sprIndex].RawTileIndex * 32) & 0x3FFF;
        uint16_t tileAddr = baseTileAddr;
        
        // For tall sprites, if we're in the bottom half, capture the second tile
        if(_state.UseLargeSprites && spriteRow >= 8) {
            tileAddr = (baseTileAddr + 32) & 0x3FFF;
        }
        
        HdSmsTileInfo& spriteTile = _currentSpriteTiles[sprIndex];
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
            spriteTile.PaletteColors = 
                ((uint32_t)_paletteRam[0x20]) |
                ((uint32_t)_paletteRam[0x21] << 8) |
                ((uint32_t)_paletteRam[0x22] << 16) |
                ((uint32_t)_paletteRam[0x23] << 24);
        } else {
            // SMS: 1 byte per color, sprite palette at 0x10-0x1F
            spriteTile.PaletteColors = 
                ((uint32_t)_paletteRam[0x10]) |
                ((uint32_t)_paletteRam[0x11] << 8) |
                ((uint32_t)_paletteRam[0x12] << 16) |
                ((uint32_t)_paletteRam[0x13] << 24);
        }
        
        if(sprIndex >= _currentSpriteCount) {
            _currentSpriteCount = sprIndex + 1;
        }
        
        // Debug: Log sprite capture
        static int logCount = 0;
        if(logCount++ < 20) {
            LogToFile("[HD Capture] Sprite captured: cycle=" + std::to_string(cycle) +
                               ", sprIndex=" + std::to_string(sprIndex) + 
                               ", tileIndex=" + std::to_string(spriteTile.TileIndex) + 
                               ", addr=0x" + HexUtilities::ToHex(spriteTile.TileAddr) + 
                               ", row=" + std::to_string(spriteRow) +
                               ", X=" + std::to_string(_spriteShifters[sprIndex].SpriteX) +
                               ", _currentSpriteCount=" + std::to_string(_currentSpriteCount));
        }
    }
}

void HdBuilderSmsVdp::DrawPixel()
{
    // Call parent to actually draw the pixel
    SmsVdp::DrawPixel();
    
    // Debug: Log first few calls
    static int drawPixelCallCount = 0;
    if(drawPixelCallCount++ < 3) {
        LogToFile("[HD Capture] DrawPixel called: enabled=" + std::to_string(_hdCaptureEnabled) + 
                           ", hasBuffer=" + std::to_string(_captureBuffer != nullptr) +
                           ", scanline=" + std::to_string(_state.Scanline));
    }
    
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
    
    // Store background tile for this pixel
    if(_currentBgTile.TileIndex >= 0) {
        pixel.Background = _currentBgTile;
    }
    
    // Store sprite tiles for this pixel
    pixel.SpriteCount = 0;
    static int spritePixelLogCount = 0;

    // Use the VDP's actual sprite count for this scanline, but clamp to hardware limit (8)
    uint8_t maxSprites = _spriteCount;
    if(maxSprites > 8) {
        maxSprites = 8;
    }

    // Debug: Log sprite count once per scanline to confirm visibility
    static int lastLoggedSpriteScanline = -1;
    if(_state.Scanline != lastLoggedSpriteScanline && _state.Scanline < 20) {
        if(maxSprites > 0) {
            LogToFile("[HD Capture] Scanline " + std::to_string(_state.Scanline) + 
                               " has " + std::to_string(maxSprites) + " sprites. " +
                               "First sprite X=" + std::to_string(_spriteShifters[0].SpriteX) + 
                               ", TileIdx=" + std::to_string(_currentSpriteTiles[0].TileIndex));
        }
        lastLoggedSpriteScanline = _state.Scanline;
    }

    for(uint8_t i = 0; i < maxSprites && pixel.SpriteCount < 4; i++) {
        if(_currentSpriteTiles[i].TileIndex >= 0) {
            // Check if this sprite is actually visible at this X position
            uint16_t spriteX = _spriteShifters[i].SpriteX;
            uint16_t spriteWidth = 8 << (uint8_t)IsZoomedSpriteAllowed(i);
            if(x >= spriteX && x < spriteX + spriteWidth) {
                pixel.Sprites[pixel.SpriteCount++] = _currentSpriteTiles[i];
                if(spritePixelLogCount++ < 5) {
                    LogToFile("[HD Capture] Sprite pixel stored: sprIndex=" + std::to_string(i) + 
                                       ", tileIndex=" + std::to_string(_currentSpriteTiles[i].TileIndex) + 
                                       ", x=" + std::to_string(x) + ", y=" + std::to_string(_state.Scanline));
                }
            }
        }
    }
    
    // Debug: Log sprite count and pixel storage per scanline
    static int lastLoggedScanline = -1;
    static int pixelsWithSprites = 0;
    if(_state.Scanline != lastLoggedScanline) {
        if(_currentSpriteCount > 0 && _state.Scanline < 20) {
            MessageManager::Log("[HD Capture] Scanline " + std::to_string(_state.Scanline) + 
                               ": _currentSpriteCount=" + std::to_string(_currentSpriteCount) + 
                               ", pixelsWithSprites=" + std::to_string(pixelsWithSprites));
        }
        lastLoggedScanline = _state.Scanline;
        pixelsWithSprites = 0;
    }
    if(pixel.SpriteCount > 0) {
        pixelsWithSprites++;
    }
    
    // Store scroll values
    pixel.ScrollX = _state.HorizontalScrollLatch;
    pixel.ScrollY = _state.VerticalScrollLatch;
}

void HdBuilderSmsVdp::ProcessEndOfScanline()
{
    // Call parent first
    SmsVdp::ProcessEndOfScanline();
    
    // Only reset background tile - sprites are reset at start of sprite loading phase
    // This is because sprites loaded during hblank are used for the NEXT scanline
    if(_hdCaptureEnabled) {
        _currentBgTile.Reset();
        // DO NOT reset _currentSpriteTiles here - they're needed for the next scanline's DrawPixel
    }
}

// ============================================================================
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
    
    // SG-1000 tiles are 8 bytes (1 bit per pixel, 8 rows)
    // We need to expand this to 32-byte format for compatibility
    // Store the 8-byte pattern data in the first 8 bytes, zero the rest
    uint16_t patternAddr;
    if(_state.M3_Use240LineMode) {
        // Mode 3 - Multicolor
        uint8_t tilemapRow = (_state.Scanline / 8);
        patternAddr = (_state.BgPatternTableAddress & 0x3800) + (_bgTileIndex * 8);
    } else if(_state.M2_AllowHeightChange) {
        // Mode 2 - Graphic 2
        uint8_t tilemapRow = (_state.Scanline / 8);
        uint16_t tileIdx = _bgTileIndex + ((tilemapRow & 0x18) << 5);
        uint16_t mask = ((_state.BgPatternTableAddress >> 3) | 0xFF) & 0x3FF;
        patternAddr = (_state.BgPatternTableAddress & 0x2000) + ((tileIdx & mask) * 8);
    } else {
        // Mode 0 - Graphic 1
        patternAddr = (_state.BgPatternTableAddress & 0x3800) + (_bgTileIndex * 8);
    }
    
    // Read 8-byte tile pattern and store in TileData
    // For SG-1000, we store the raw 8-byte pattern in the first 8 bytes
    // and use a special flag or format indicator
    for(int i = 0; i < 8; i++) {
        _currentBgTile.TileData[i] = _videoRam[(patternAddr + i) & 0x3FFF];
    }
    // Zero the remaining bytes to indicate SG-1000 format
    for(int i = 8; i < 32; i++) {
        _currentBgTile.TileData[i] = 0;
    }
    
    // Store color table info in PaletteColors
    // For Mode 2, each tile row can have different colors
    // For Mode 0, colors are shared per 8 tiles
    uint8_t colorByte = 0;
    if(_state.M3_Use240LineMode) {
        // Mode 3 - color is the pattern data itself
        colorByte = _bgPatternData;
    } else if(_state.M2_AllowHeightChange) {
        // Mode 2 - per-row colors
        uint8_t tileRow = (_state.Scanline & 0x07);
        uint8_t tilemapRow = (_state.Scanline / 8);
        uint16_t tileIdx = _bgTileIndex + ((tilemapRow & 0x18) << 5);
        uint16_t mask = ((_state.ColorTableAddress >> 3) | 0x07) & 0x3FF;
        uint16_t colorAddr = (_state.ColorTableAddress & 0x2000) | ((tileIdx & mask) << 3) + tileRow;
        colorByte = _videoRam[colorAddr & 0x3FFF];
    } else {
        // Mode 0 - shared colors per 8 tiles
        uint16_t colorAddr = (_state.ColorTableAddress & 0x3FC0) | ((_bgTileIndex >> 3) & 0x1F);
        colorByte = _videoRam[colorAddr & 0x3FFF];
    }
    
    // Pack color info: FG color in high nibble, BG color in low nibble
    _currentBgTile.PaletteColors = colorByte;
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
            _currentSpriteTiles[i].Reset();
        }
        _currentSpriteCount = 0;
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
    HdSmsTileInfo& spriteTile = _currentSpriteTiles[sprIndex];
    spriteTile.Reset();
    spriteTile.IsSprite = true;
    spriteTile.IsVramTile = true;
    spriteTile.PaletteIndex = 1; // Sprites use "high" palette conceptually
    
    // Get sprite tile address from shifter
    uint16_t tileAddr = _spriteShifters[sprIndex].TileAddr;
    
    // SG-1000 sprites are 8 bytes (or 32 bytes for 16x16)
    // Read the pattern data
    for(int i = 0; i < 8; i++) {
        spriteTile.TileData[i] = _videoRam[(tileAddr + i) & 0x3FFF];
    }
    // Zero remaining bytes
    for(int i = 8; i < 32; i++) {
        spriteTile.TileData[i] = 0;
    }
    
    // Sprite color is stored in TileData[1] by the parent (attribute byte)
    // The color is in the low 4 bits
    uint8_t spriteColor = _spriteShifters[sprIndex].TileData[1] & 0x0F;
    spriteTile.PaletteColors = spriteColor;
    spriteTile.TileAddr = tileAddr;
    spriteTile.TileIndex = (int32_t)(tileAddr / 8);
    
    if(sprIndex >= _currentSpriteCount) {
        _currentSpriteCount = sprIndex + 1;
    }
}