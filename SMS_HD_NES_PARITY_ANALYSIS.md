# SMS HD Pack vs NES HD Pack - Parity Analysis

## Executive Summary
Analyzed NES HD pack implementation to identify gaps in SMS HD pack system. The SMS implementation is **partially aligned** with NES patterns but missing several critical features for multi-system support (Game Gear, SG-1000) and advanced tile handling.

---

## ✅ **What SMS Has Correctly Implemented**

### Core Architecture
- ✅ **Frame-based processing** - Uses `ProcessFrame()` to consume `HdScreenInfoSms`
- ✅ **Tile usage counting** - Tracks frequency via `_tileUsageCount`
- ✅ **Tile deduplication** - Uses `HdTileKeySms` hash for uniqueness
- ✅ **HD tile generation** - Generates scaled tiles with filters
- ✅ **Manifest generation** - Creates `hires.txt` with tile entries
- ✅ **Per-pixel metadata** - Stores background + 4 sprites per pixel

### Tile Metadata
- ✅ **Palette tracking** - `PaletteColors` field
- ✅ **Tile data storage** - 32-byte SMS pattern data
- ✅ **Mirroring flags** - `HorizontalMirroring`, `VerticalMirroring`
- ✅ **Priority flag** - `BackgroundPriority`
- ✅ **VRAM bank grouping** - `VramBankId` calculation

---

## ❌ **Critical Missing Features**

### 1. **Transparency Detection**
**NES Has:**
```cpp
bool TransparencyRequired;      // Set when BG+sprite overlap
bool HasTransparentPixels;      // Tile has any transparent pixels
bool IsFullyTransparent;        // Tile is 100% transparent
```

**SMS Missing:**
- No `TransparencyRequired` flag propagation from `ProcessFrame`
- No `HasTransparentPixels` detection in `GenerateHdTile`
- No `IsFullyTransparent` check for optimization
- No `UpdateFlags()` method to analyze tile transparency

**Impact:** HD tiles won't handle sprite overlays correctly (BG tiles behind sprites need transparency).

---

### 2. **Blank Tile Grouping**
**NES Has:**
```cpp
bool Blank;                     // Tile is solid color
uint32_t _blankTileIndex;       // Groups all blank tiles together
int _blankTilePalette;          // Palette for blank tile group
```

**SMS Missing:**
- No `Blank` flag in `HdPackTileInfoSms`
- No blank tile detection logic
- No grouping of blank tiles together (wastes sheet space)

**Impact:** HD packs will contain many duplicate blank tiles instead of reusing one.

---

### 3. **Default Tile Fallback**
**NES Has:**
```cpp
result = _tileUsageCount.find(tile.GetKey(false));
if(result == _tileUsageCount.end()) {
    // Try again with defaultKey (palette-agnostic)
    result = _tileUsageCount.find(tile.GetKey(true));
}
```

**SMS Missing:**
- `ProcessTileNesStyle` doesn't try default key lookup
- No palette-agnostic tile matching

**Impact:** Tiles with identical patterns but different palettes won't share base HD artwork.

---

### 4. **Overscan Filtering**
**NES Has:**
```cpp
if(_options.IgnoreOverscan) {
    OverscanDimensions overscan = _emu->GetSettings()->GetOverscan();
    if(x < overscan.Left || y < overscan.Top || ...) {
        return; // Skip tiles in overscan area
    }
}
```

**SMS Missing:**
- No overscan boundary check in `ProcessFrame`
- All tiles recorded regardless of visibility

**Impact:** HD packs include off-screen tiles that are never visible.

---

### 5. **Game Gear / SG-1000 Detection**
**NES Has:**
```cpp
PpuModel ppuModel;  // Different PPU variants handled
```

**SMS Missing:**
- `ProcessTileNesStyle` doesn't check `SmsModel`
- No conditional logic for Game Gear 12-bit palette vs SMS 6-bit
- No SG-1000 mode handling

**Current Code:**
```cpp
// SMS/HdPacks/HdPackBuilderSms.cpp:667
SmsModel model = _console->GetModel();
if(model == SmsModel::GameGear) {
    // Game Gear palette handling
}
```
This check exists in `UpdatePalette()` but **not in frame processing path**.

**Impact:** 
- Game Gear HD packs may use wrong palette format
- SG-1000 games not properly supported

---

### 6. **Palette Snapshot Stability**
**NES Has:**
```cpp
uint32_t _palette[512];  // Full NES palette pre-calculated
```

**SMS Has:**
```cpp
uint32_t _palette[32];   // SMS palette (16 BG + 16 sprite)
```

**Issue:** SMS palette is updated via `UpdatePalette()` but there's no guarantee it's stable when `ProcessFrame` runs. If palette changes mid-frame, tiles may use wrong colors.

**Risk:** Medium - depends on game timing.

---

## 🔧 **Recommended Fixes**

### Priority 1: Transparency Support
Add to `HdPackTileInfoSms`:
```cpp
bool TransparencyRequired = false;
bool HasTransparentPixels = false;
bool IsFullyTransparent = false;
```

Modify `ProcessFrame` to detect sprite overlaps:
```cpp
const HdSmsTileInfo& bgInfo = pixel.Background;
bool hasSpriteOverBg = (pixel.SpriteCount > 0);

// Pass transparency flag
ProcessTileNesStyle(key, bgInfo.TileAddr, false, hasSpriteOverBg);
```

Add `UpdateFlags()` to `HdPackTileInfoSms`:
```cpp
void UpdateFlags() {
    Blank = true;
    HasTransparentPixels = false;
    IsFullyTransparent = true;
    for(size_t i = 0; i < HdTileData.size(); i++) {
        if(HdTileData[i] != HdTileData[0]) Blank = false;
        if((HdTileData[i] & 0xFF000000) != 0xFF000000) HasTransparentPixels = true;
        if(HdTileData[i] & 0xFF000000) IsFullyTransparent = false;
    }
}
```

---

### Priority 2: Game Gear / SG-1000 Support
Add model detection to `ProcessFrame`:
```cpp
void HdPackBuilderSms::ProcessFrame(HdScreenInfoSms* frameInfo)
{
    if(!_isRecording || !frameInfo) return;

    SmsModel model = _console->GetModel();
    bool isGameGear = (model == SmsModel::GameGear);
    bool isSg1000 = (model == SmsModel::Sg);

    // Different processing based on system
    if(isGameGear) {
        // 160x144 viewport, 12-bit palette
    } else if(isSg1000) {
        // SG-1000 specific handling
    } else {
        // Standard SMS 256x192
    }
}
```

---

### Priority 3: Blank Tile Grouping
Add to `HdPackBuilderSms.h`:
```cpp
uint32_t _blankTileIndex = 0;
uint8_t _blankTilePalette = 0;
```

Implement blank detection:
```cpp
bool IsBlankTile(const uint8_t* tileData) {
    for(int i = 1; i < 32; i++) {
        if(tileData[i] != tileData[0]) return false;
    }
    return true;
}
```

Use in `ProcessTileNesStyle`:
```cpp
if(_options.GroupBlankTiles && IsBlankTile(key.TileData)) {
    // Reuse existing blank tile
    key.TileIndex = _blankTileIndex;
    key.PaletteColors = _blankTilePalette;
}
```

---

### Priority 4: Overscan Filter
```cpp
void HdPackBuilderSms::ProcessFrame(HdScreenInfoSms* frameInfo)
{
    OverscanDimensions overscan = _emu->GetSettings()->GetOverscan();

    for(uint32_t y = 0; y < ScreenHeight; y++) {
        if(_options.IgnoreOverscan) {
            if(y < overscan.Top || (ScreenHeight - y - 1) < overscan.Bottom) {
                continue; // Skip overscan rows
            }
        }

        for(uint32_t x = 0; x < ScreenWidth; x++) {
            if(_options.IgnoreOverscan) {
                if(x < overscan.Left || (ScreenWidth - x - 1) < overscan.Right) {
                    continue; // Skip overscan columns
                }
            }
            // ... process pixel
        }
    }
}
```

---

### Priority 5: Default Tile Fallback
```cpp
void HdPackBuilderSms::ProcessTileNesStyle(HdTileKeySms& key, uint32_t tileAddr, bool isSprite)
{
    auto it = _tileUsageCount.find(key);
    if(it == _tileUsageCount.end()) {
        // Try default key (palette-agnostic)
        HdTileKeySms defaultKey = key;
        defaultKey.PaletteColors = 0xFFFFFFFF;
        it = _tileUsageCount.find(defaultKey);
        
        if(it != _tileUsageCount.end()) {
            // Found palette-agnostic match, increment usage
            it->second++;
            return;
        }
    }
    // ... rest of logic
}
```

---

## 📊 **Feature Comparison Matrix**

| Feature | NES | SMS | Notes |
|---------|-----|-----|-------|
| Frame-based processing | ✅ | ✅ | Both use per-frame capture |
| Tile usage counting | ✅ | ✅ | Both track frequency |
| Transparency detection | ✅ | ❌ | **Critical gap** |
| Blank tile grouping | ✅ | ❌ | Optimization missing |
| Default tile fallback | ✅ | ❌ | Reusability gap |
| Overscan filtering | ✅ | ❌ | Quality issue |
| Multi-system support | ✅ | ⚠️ | Partial (needs runtime checks) |
| CHR/VRAM bank grouping | ✅ | ✅ | SMS has VramBankId |
| Palette snapshot | ✅ | ✅ | Both store palette |
| Mirroring flags | ✅ | ✅ | Both track H/V mirror |
| Usage-based sorting | ✅ | ✅ | Both support |

---

## 🎯 **Next Steps**

1. **Implement transparency support** (Priority 1) - Fixes sprite overlay rendering
2. **Add Game Gear runtime detection** (Priority 2) - Ensures correct system handling
3. **Add blank tile grouping** (Priority 3) - Reduces HD pack size
4. **Add overscan filtering** (Priority 4) - Improves HD pack quality
5. **Runtime testing** - Verify fixes with actual SMS/GG games

---

## 📝 **Testing Checklist**

- [ ] SMS game with sprites over background (transparency test)
- [ ] Game Gear game (12-bit palette test)
- [ ] SG-1000 game (system detection test)
- [ ] Game with solid-color tiles (blank grouping test)
- [ ] Game with overscan visible (filtering test)
- [ ] CHR-RAM game (VRAM bank grouping test)

---

## 🔍 **Code References**

### NES Reference Implementation
- `Core/NES/HdPacks/HdPackBuilder.cpp` - Main tile processing
- `Core/NES/HdPacks/HdData.h` - Tile metadata structures
- `Core/NES/HdPacks/HdBuilderPpu.h` - PPU integration

### SMS Current Implementation
- `Core/SMS/HdPacks/HdPackBuilderSms.cpp` - Frame processing
- `Core/SMS/HdPacks/HdDataSms.h` - SMS tile structures
- `Core/SMS/HdPacks/HdBuilderSmsVdp.cpp` - VDP integration
