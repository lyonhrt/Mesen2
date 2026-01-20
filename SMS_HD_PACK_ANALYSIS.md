# SMS HD Pack System - Root Cause Analysis & Fix Plan

## Executive Summary
The SMS HD pack system has fundamental architectural issues that prevent it from working reliably. The NES system works because it captures tile data **at render time with complete context**, while SMS attempts to capture tiles **after the fact without proper VDP state**.

---

## Critical Issues Identified

### 1. **Missing Real-Time Tile Capture (ROOT CAUSE)**

**NES System (Working):**
- `HdNesPpu` extends `NesPpu` and overrides `DrawPixel()`
- Captures tile data **during actual rendering** with full context:
  - Exact tile address from PPU state
  - Palette colors from current PPU palette RAM
  - Sprite/background priority
  - Mirroring flags
  - Pixel-perfect positioning
- Stores complete `HdPpuPixelInfo` for every pixel rendered
- Has access to CHR-ROM/RAM data via mapper at render time

**SMS System (Broken):**
- `SmsVdp` has HD pack hooks but **doesn't capture during rendering**
- `HdPackBuilderSms` tries to reconstruct tiles **after rendering**
- No equivalent to `HdNesPpu::DrawPixel()` capturing per-pixel tile info
- Tile data captured via `ProcessSmsSprite()` API calls, not during VDP rendering
- Missing critical VDP state (scroll position, exact tile addresses, palette state)

### 2. **Tile Data Format Confusion**

**Issue:** SMS tiles use planar format (4 consecutive 8-byte bitplanes), but the capture/processing pipeline has been inconsistent.

**Recent Fix Applied:**
```cpp
// CORRECT (Planar format):
uint8_t plane0 = tileData[y];        // Bitplane 0, row y
uint8_t plane1 = tileData[8 + y];   // Bitplane 1, row y  
uint8_t plane2 = tileData[16 + y];  // Bitplane 2, row y
uint8_t plane3 = tileData[24 + y];  // Bitplane 3, row y
```

**Status:** This fix is correct but insufficient - the real problem is **when and how** tile data is captured.

### 3. **No Per-Pixel Tile Information**

**NES Has:**
```cpp
struct HdPpuPixelInfo {
    HdPpuTileInfo Tile;           // Background tile at this pixel
    HdPpuTileInfo Sprite[4];      // Up to 4 sprites at this pixel
    uint8_t SpriteCount;
    // ... complete rendering context
};
HdPpuPixelInfo ScreenTiles[256*240];  // Full frame buffer
```

**SMS Missing:**
- No per-pixel tile tracking
- No frame buffer with tile information
- Cannot reconstruct which tiles were actually rendered where
- HD replacement has no context for proper tile matching

### 4. **Incomplete VDP Integration**

**SMS VDP Issues:**
```cpp
// In SmsVdp.cpp - HD pack hooks exist but are incomplete:
- GetPixelColor() has HD sprite tracking but no background tile capture
- No equivalent to NES's StoreTileInformation()
- HD data structures (_spriteShifters[i].HdRowIndex) exist but underutilized
- Missing tile address tracking during background rendering
```

---

## Why NES HD Packs Work Perfectly

### Architecture Flow:

1. **Capture Phase (During Gameplay):**
   ```
   Game Renders Frame
   ↓
   HdNesPpu::DrawPixel() called 61,440 times (256×240)
   ↓
   For each pixel:
     - Read current tile from PPU state
     - Copy 16-byte CHR data from mapper
     - Record palette colors from PPU RAM
     - Store complete HdPpuPixelInfo
   ↓
   HdPackBuilder receives complete frame data
   ↓
   Tiles extracted with perfect context
   ```

2. **Replacement Phase (With HD Pack):**
   ```
   Game Renders Frame
   ↓
   HdNesPpu::DrawPixel() captures tile info
   ↓
   HdNesPack::GetPixel() looks up HD replacement
   ↓
   Matches tile by:
     - TileIndex (CHR address)
     - PaletteColors (exact 4-color palette)
     - Conditions (frame#, memory watches, etc.)
   ↓
   Draws HD tile with proper mirroring/positioning
   ```

### Key Success Factors:

1. **Timing:** Capture happens **during rendering**, not after
2. **Context:** Full PPU state available (addresses, palettes, scroll)
3. **Accuracy:** Pixel-perfect tile-to-screen mapping
4. **Simplicity:** One clear capture point (`DrawPixel()`)

---

## SMS HD Pack Fix Plan

### Phase 1: Implement Real-Time Capture (CRITICAL)

**Create `HdBuilderSmsVdp` class** (analogous to `HdNesPpu`):

```cpp
class HdBuilderSmsVdp : public SmsVdp {
private:
    HdPackBuilderSms* _hdPackBuilder = nullptr;
    HdScreenInfoSms* _screenInfo[2] = {};
    HdScreenInfoSms* _currentInfo = nullptr;
    
public:
    // Override GetPixelColor() to capture tile data
    uint16_t GetPixelColor() override {
        uint16_t color = SmsVdp::GetPixelColor();
        
        if(_hdPackBuilder && _hdPackBuilder->IsRecording()) {
            CaptureTileInfo();  // NEW: Capture during rendering
        }
        
        return color;
    }
    
    void CaptureTileInfo() {
        uint16_t xPos = GetVisiblePixelIndex();
        uint16_t yPos = _state.Scanline;
        
        // Capture background tile
        if(_state.RenderingEnabled) {
            uint16_t tileAddr = GetCurrentBackgroundTileAddress();
            uint8_t tileData[32];
            ReadVramTile(tileAddr, tileData);
            
            HdSmsTileInfo tileInfo;
            tileInfo.TileAddr = tileAddr;
            memcpy(tileInfo.TileData, tileData, 32);
            tileInfo.PaletteColors = GetCurrentPaletteColors();
            tileInfo.ScrollX = _state.ScrollX;
            tileInfo.ScrollY = _state.ScrollY;
            
            _currentInfo->ScreenTiles[yPos * 256 + xPos] = tileInfo;
        }
        
        // Capture sprite tiles (already partially done)
        // ... existing sprite capture logic ...
    }
};
```

### Phase 2: Add Per-Pixel Tile Tracking

```cpp
struct HdSmsTileInfo {
    uint32_t TileAddr;
    uint8_t TileData[32];       // Full 32-byte SMS tile
    uint32_t PaletteColors;     // Encoded palette selection
    uint8_t ScrollX, ScrollY;
    uint8_t OffsetX, OffsetY;
    bool HorizontalMirror;
    bool VerticalMirror;
    bool IsSprite;
};

struct HdScreenInfoSms {
    HdSmsTileInfo ScreenTiles[256*240];  // Per-pixel tile info
    uint64_t FrameNumber;
    // ... other frame metadata ...
};
```

### Phase 3: Integrate with Existing Builder

```cpp
// In HdPackBuilderSms:
void ProcessFrameData(HdScreenInfoSms* frameInfo) {
    // Process captured frame data
    for(int y = 0; y < 240; y++) {
        for(int x = 0; x < 256; x++) {
            HdSmsTileInfo& tileInfo = frameInfo->ScreenTiles[y * 256 + x];
            
            if(tileInfo.TileAddr != 0) {
                // Create tile key from captured data
                HdTileKeySms key;
                key.TileIndex = tileInfo.TileAddr;
                memcpy(key.TileData, tileInfo.TileData, 32);
                key.PaletteColors = tileInfo.PaletteColors;
                key.IsSprite = tileInfo.IsSprite;
                
                // Use simplified NES-style processing
                ProcessTileNesStyle(key, tileInfo.TileAddr, tileInfo.IsSprite);
            }
        }
    }
}
```

### Phase 4: Fix Video Filter for Replacement

```cpp
// In SmsHdVideoFilter.cpp:
void SmsHdVideoFilter::ApplyFilter(uint16_t* vdpOutputBuffer) {
    if(!_hdData || _hdData->Tiles.empty()) {
        // No HD pack loaded, pass through
        BasicPassThrough(vdpOutputBuffer);
        return;
    }
    
    // Get captured frame info from HD VDP
    HdScreenInfoSms* frameInfo = _hdVdp->GetFrameInfo();
    
    uint32_t* outputBuffer = GetOutputBuffer();
    uint32_t scale = _hdData->Scale;
    
    for(uint32_t y = 0; y < 240; y++) {
        for(uint32_t x = 0; x < 256; x++) {
            HdSmsTileInfo& tileInfo = frameInfo->ScreenTiles[y * 256 + x];
            
            // Look up HD replacement
            HdPackTileInfoSms* hdTile = FindHdTile(tileInfo);
            
            if(hdTile) {
                // Draw HD tile
                DrawHdTile(hdTile, outputBuffer, x * scale, y * scale);
            } else {
                // Draw original pixel
                DrawOriginalPixel(vdpOutputBuffer[y * 256 + x], outputBuffer, x * scale, y * scale);
            }
        }
    }
}
```

---

## Implementation Priority

### Immediate (Fix Corruption):
1. ✅ **DONE:** Fix planar tile format in `ProcessTilePixels()`
2. ⚠️ **IN PROGRESS:** Add `UseNesStylePipeline` option for simplified capture
3. ❌ **BLOCKED:** Need real-time VDP capture

### Short Term (Core Functionality):
1. Create `HdBuilderSmsVdp` class extending `SmsVdp`
2. Implement `CaptureTileInfo()` during `GetPixelColor()`
3. Add `HdScreenInfoSms` frame buffer
4. Route captured data to `HdPackBuilderSms`

### Medium Term (Full HD Replacement):
1. Implement `SmsHdVideoFilter::ApplyFilter()` properly
2. Add tile lookup/matching logic
3. Implement HD tile drawing with mirroring
4. Add condition system support

### Long Term (Polish):
1. Optimize capture performance
2. Add incremental pack loading (already partially done)
3. Implement background replacement
4. Add audio replacement support

---

## Comparison Table

| Feature | NES (Working) | SMS (Current) | SMS (Needed) |
|---------|---------------|---------------|--------------|
| Real-time capture | ✅ HdNesPpu::DrawPixel() | ❌ Post-render API | ✅ HdBuilderSmsVdp |
| Per-pixel tile info | ✅ HdPpuPixelInfo[61440] | ❌ None | ✅ HdSmsTileInfo[61440] |
| Tile data format | ✅ 16-byte interleaved | ✅ 32-byte planar (fixed) | ✅ Already correct |
| Palette capture | ✅ During render | ⚠️ Partial | ✅ Need full capture |
| VDP state access | ✅ Full PPU state | ⚠️ Limited | ✅ Need scroll, addresses |
| HD replacement | ✅ HdNesPack | ❌ Stub only | ❌ Not implemented |
| Tile matching | ✅ TileIndex + Palette | ⚠️ Hash-based | ✅ Need proper matching |

---

## Root Cause Summary

**The SMS HD pack system fails because:**

1. **No real-time capture** - Tiles are captured after rendering without VDP context
2. **Missing per-pixel tracking** - Cannot map tiles to screen positions accurately  
3. **Incomplete VDP integration** - HD hooks exist but don't capture necessary data
4. **No replacement pipeline** - Video filter is a stub, doesn't apply HD tiles

**The NES system works because:**

1. **Real-time capture** - `HdNesPpu::DrawPixel()` captures during rendering
2. **Complete context** - Full PPU state, CHR data, palettes available
3. **Per-pixel accuracy** - Every pixel knows its source tile
4. **Working replacement** - `HdNesPack` properly matches and draws HD tiles

---

## Next Steps

1. **Immediate:** Complete the `UseNesStylePipeline` option to stabilize capture
2. **Critical:** Implement `HdBuilderSmsVdp` for real-time capture
3. **Essential:** Add `HdScreenInfoSms` frame buffer
4. **Required:** Implement proper `SmsHdVideoFilter` replacement logic

**Estimated Effort:** 2-3 days for core functionality, 1 week for full feature parity with NES.
