# HDSMS Implementation Plan for Mesen2

**Date:** 2025-06-19 14:36:28
**Author:** lyonhrt
**Project:** High Definition Sega Master System Graphics for Mesen2

## 1. Project Overview

The HDSMS project aims to implement high-definition graphics support for Sega Master System games in the Mesen2 emulator, following a similar approach to HDNES but adapted for SMS hardware specifics. This feature will allow users to replace original low-resolution SMS graphics with high-resolution alternatives while maintaining accurate emulation.

## 2. Background

### 2.1 Mesen2 SMS Implementation

Mesen2 already features a complete SMS emulation with the following components:
- Z80 CPU emulation (`SmsCpu.cpp/h`)
- VDP (Video Display Processor) emulation (`SmsVdp.cpp/h`)
- Memory management (`SmsMemoryManager.cpp/h`)
- Audio components (`SmsPsg.cpp/h`, `SmsFmAudio.cpp/h`)
- Input handling (`SmsControlManager.cpp/h`, `Input/` directory)

### 2.2 HDNES Format Reference

Mesen already supports HD packs for NES through the HDNES format, which allows:
- Replacement of original graphics with high-resolution alternatives
- Condition-based rendering to handle different game states
- Custom palettes and visual effects
- Background image replacements
- Audio replacements (BGM and SFX)

We'll use the existing HDNES format as a reference and adapt it for SMS hardware specifics. The [HDNES-POPEYE pack by LiQuiDzGit](https://github.com/LiQuiDzGit/HDNES-POPEYE) will serve as a reference implementation.

## 3. Technical Architecture

### 3.1 HD Pack Structure

Based on the HDNES format, our HDSMS structure will be:

```
HDSMS_PACK/
├── hdsms.json           # Pack metadata and configuration (similar to hires.txt in HDNES)
├── Tileset/             # PNG files containing HD tile graphics
│   ├── Tileset0.png     # First tileset
│   ├── Tileset1.png     # Second tileset
│   └── ...              
├── Backgrounds/         # Background image replacements
│   ├── background1.png  # Custom backgrounds for specific conditions
│   └── ...              
├── Audio/               # Audio replacements
│   ├── BGM/             # Background music tracks
│   ├── SFX/             # Sound effect replacements
└── Palettes/            # Custom palette definitions (optional)
```

### 3.2 Core Components

#### 3.2.1 HD Pack Manager

```cpp
class SmsHdPackManager
{
public:
    void LoadHdPack(string filepath);
    bool HasHdPack();
    bool IsEnabled();
    void SetEnabled(bool enabled);
    void ApplyCustomBackground();
    
    // Tile and sprite replacement functions
    HdTile* GetHdTile(uint32_t hash, vector<int>& activeConditions);
    bool HasHdReplacement(uint32_t hash);
    
    // Condition management
    void EvaluateConditions(SmsConsole* console);
    bool IsConditionActive(int conditionId);
    
private:
    // Hash maps for tiles, sprites, backgrounds
    unordered_map<uint32_t, vector<unique_ptr<HdTile>>> _tilesByHash;
    vector<unique_ptr<HdBackground>> _backgrounds;
    
    // Audio replacement related
    unordered_map<uint16_t, string> _bgmFiles;
    unordered_map<uint16_t, string> _sfxFiles;
    
    // Condition management
    vector<HdPackCondition> _conditions;
    bitset<1024> _activeConditions;  // Bitset for quick condition checking
    
    // Pack configuration
    int _scale = 4;
    bool _hdPackLoaded = false;
    bool _enabled = false;
    
    // Helper functions for loading
    void LoadTileReplacements(json& hdPackDefinition);
    void LoadBackgrounds(json& hdPackDefinition);
    void LoadConditions(json& hdPackDefinition);
    void LoadOptions(json& hdPackDefinition);
    void LoadAudioTracks(json& hdPackDefinition);
};
```

#### 3.2.2 HD Tiles and Backgrounds

```cpp
struct HdTile
{
    uint32_t originalHash;
    vector<uint32_t> pixelData;  // ARGB format
    uint32_t width;  // Typically 8 * scale factor
    uint32_t height; // Typically 8 * scale factor
    vector<int> conditionIds;
    int priority;
    float brightness;
    bool isDefault;
};

struct HdBackground
{
    string filename;
    vector<uint32_t> pixelData;  // ARGB format
    uint32_t width;
    uint32_t height;
    vector<int> conditionIds;
    float brightness;
    float horizontalScrollRatio;
    float verticalScrollRatio;
    int priorityLevel;  // 0-39 priority levels as in HDNES
    int leftOffset;
    int topOffset;
};
```

#### 3.2.3 SMS-Specific Condition System

Based on the HDNES condition system detailed in the documentation, but adapted for SMS hardware:

```cpp
enum class HdPackConditionType
{
    // Built-in sprite conditions
    HorizontalMirror,
    VerticalMirror,
    BackgroundPriority,
    
    // Memory conditions
    MemoryCheck,
    MemoryCheckConstant,
    PpuMemoryCheck,         // SMS VDP memory
    PpuMemoryCheckConstant, // SMS VDP memory with constant
    
    // Tile/Sprite conditions
    TileAtPosition,
    SpriteAtPosition,
    TileNearby,
    SpriteNearby,
    
    // SMS VDP specific conditions
    VdpRegisterEquals,
    VdpModeEquals,
    
    // Animation/frame conditions
    FrameRange,
    
    // Logical operators
    And,
    Or,
    Not
};

struct HdPackCondition
{
    int id;
    string name;
    HdPackConditionType type;
    
    // Different parameters based on condition type
    uint16_t address;        // For memory conditions
    uint8_t value;           // For comparison
    uint8_t mask;            // For masked operations
    string operatorString;   // For comparisons: "==", "!=", ">", "<", ">=", "<="
    
    // For position-based conditions
    int x;
    int y;
    uint32_t tileData;       // Tile pattern data or index
    uint32_t paletteData;    // Palette data
    
    // For composite conditions
    vector<int> childConditionIds;
    
    // For frameRange
    int divisorValue;
    int compareValue;
    
    bool Evaluate(SmsConsole* console);
};
```

### 3.3 Integration with SmsVdp

```cpp
// Extension to SmsVdp class
class SmsVdp
{
public:
    // Existing methods...
    
    // New methods for HD rendering
    void EnableHdRenderer(bool enabled);
    void SetHdPackManager(shared_ptr<SmsHdPackManager> hdPackManager);
    void RenderScanlineWithHdPack(uint32_t scanline);
    
    // Helper methods for hash generation
    uint32_t GetTileHash(uint16_t tileIndex, uint8_t palette, uint8_t flags);
    uint32_t GetSpriteHash(uint8_t spriteIndex);
    
    // Methods for conditions
    uint8_t GetTileAt(int x, int y);
    uint8_t GetSpriteAt(int x, int y);
    uint8_t GetVdpRegister(uint8_t reg);
    uint8_t GetVdpMode();
    
private:
    shared_ptr<SmsHdPackManager> _hdPackManager;
    bool _hdModeEnabled = false;
    
    void RenderHdBackground(uint32_t scanline);
    void RenderHdBackgroundLayer(uint32_t scanline, int priorityStart, int priorityEnd);
    void RenderHdSprites(uint32_t scanline);
};
```

## 4. File Format (hdsms.json)

Based on the HDNES `hires.txt` format, but adapted to JSON for better structure and readability:

```json
{
  "info": {
    "ver": 107,
    "title": "Phantasy Star HD",
    "author": "lyonhrt",
    "version": "1.0.0",
    "description": "HD graphics for Phantasy Star (SMS)",
    "romSha1": "53CA7541404E51C248D10FAB198154F9970479B9",
    "scale": 4,
    "overscan": [8, 8, 8, 8]
  },
  "options": [
    "disableSpriteLimit",
    "disableOriginalTiles"
  ],
  "patches": [
    {
      "filename": "PhantasyStar_Patch.ips",
      "sha1": "26aec27ef0fc1a6fd282937b918ebdd1fb480891"
    }
  ],
  "images": [
    "Tileset/Tileset0.png",
    "Tileset/Tileset1.png"
  ],
  "conditions": [
    {
      "id": 1,
      "name": "InBattle",
      "type": "memoryCheckConstant",
      "address": "C000",
      "operator": "==",
      "value": "01"
    },
    {
      "id": 2,
      "name": "InTown",
      "type": "memoryCheckConstant",
      "address": "C000",
      "operator": "==",
      "value": "02"
    },
    {
      "id": 3,
      "name": "PlayerFacingDown",
      "type": "tileAtPosition",
      "x": 16,
      "y": 16,
      "tileData": "1A",
      "paletteData": "0F100017"
    },
    {
      "id": 4,
      "name": "ComplexCondition",
      "type": "and",
      "conditions": [2, 3]
    },
    {
      "id": 5,
      "name": "AnimatedTile",
      "type": "frameRange",
      "divisorValue": 60,
      "compareValue": 30
    }
  ],
  "tiles": [
    {
      "hash": "A1B2C3D4",
      "imgIndex": 0,
      "x": 0,
      "y": 0,
      "conditions": [2],
      "brightness": 1.0,
      "isDefault": false,
      "priority": 1
    },
    {
      "hash": "A1B2C3D4",
      "imgIndex": 0,
      "x": 16,
      "y": 0,
      "conditions": [4],
      "brightness": 1.0,
      "isDefault": false,
      "priority": 2
    },
    {
      "hash": "E5F6A7B8",
      "imgIndex": 1,
      "x": 0,
      "y": 16,
      "brightness": 1.0,
      "isDefault": true
    }
  ],
  "backgrounds": [
    {
      "filename": "Backgrounds/town_bg.png",
      "conditions": [2],
      "brightness": 1.0,
      "horizontalScrollRatio": 0.5,
      "verticalScrollRatio": 0.0,
      "priorityLevel": 10,
      "leftOffset": 0,
      "topOffset": 0
    },
    {
      "filename": "Backgrounds/battle_bg.png",
      "conditions": [1],
      "brightness": 1.0,
      "horizontalScrollRatio": 0.0,
      "verticalScrollRatio": 0.0,
      "priorityLevel": 0,
      "leftOffset": 0,
      "topOffset": 0
    }
  ],
  "palettes": [
    {
      "name": "TownPalette",
      "palette": [
        "#000000", "#336699", "#66CCFF", "#FFFFFF",
        "#000000", "#993333", "#CC6666", "#FFFFFF",
        "#000000", "#339933", "#66CC66", "#FFFFFF",
        "#000000", "#999933", "#CCCC66", "#FFFFFF"
      ],
      "conditions": [2]
    }
  ],
  "bgm": [
    {
      "album": 0,
      "track": 0,
      "filename": "Audio/BGM/town_theme.ogg",
      "conditions": [2]
    },
    {
      "album": 0,
      "track": 1,
      "filename": "Audio/BGM/battle_theme.ogg",
      "conditions": [1]
    }
  ],
  "sfx": [
    {
      "album": 0,
      "track": 0,
      "filename": "Audio/SFX/menu_select.ogg"
    },
    {
      "album": 0,
      "track": 1,
      "filename": "Audio/SFX/attack.ogg"
    }
  ]
}
```

## 5. Condition System Implementation

### 5.1 Condition Creation and Evaluation

```cpp
bool SmsHdPackManager::EvaluateConditions(SmsConsole* console)
{
    _activeConditions.reset(); // Clear all conditions
    
    for(size_t i = 0; i < _conditions.size(); i++) {
        bool result = EvaluateCondition(i, console);
        if(result) {
            _activeConditions.set(i);
        }
    }
    
    return true;
}

bool SmsHdPackManager::EvaluateCondition(size_t index, SmsConsole* console)
{
    const HdPackCondition& condition = _conditions[index];
    
    switch(condition.type) {
        case HdPackConditionType::HorizontalMirror:
            return console->GetPpu()->IsCurrentPixelHorizontallyMirrored();
            
        case HdPackConditionType::MemoryCheckConstant: {
            uint8_t value = console->GetMemoryManager()->Read(condition.address);
            if(condition.mask != 0) {
                value &= condition.mask;
            }
            
            if(condition.operatorString == "==") return value == condition.value;
            if(condition.operatorString == "!=") return value != condition.value;
            if(condition.operatorString == ">") return value > condition.value;
            if(condition.operatorString == "<") return value < condition.value;
            if(condition.operatorString == ">=") return value >= condition.value;
            if(condition.operatorString == "<=") return value <= condition.value;
            return false;
        }
        
        case HdPackConditionType::TileAtPosition: {
            uint8_t tileAt = console->GetVdp()->GetTileAt(condition.x, condition.y);
            return tileAt == condition.tileData;
        }
        
        case HdPackConditionType::FrameRange: {
            uint32_t frameCount = console->GetFrameCount();
            return (frameCount % condition.divisorValue) >= condition.compareValue;
        }
        
        case HdPackConditionType::And: {
            for(int childId : condition.childConditionIds) {
                if(!EvaluateCondition(childId, console)) {
                    return false;
                }
            }
            return true;
        }
        
        case HdPackConditionType::Or: {
            for(int childId : condition.childConditionIds) {
                if(EvaluateCondition(childId, console)) {
                    return true;
                }
            }
            return false;
        }
        
        case HdPackConditionType::Not:
            return !EvaluateCondition(condition.childConditionIds[0], console);
            
        // Additional SMS-specific conditions
        case HdPackConditionType::VdpModeEquals:
            return console->GetVdp()->GetVdpMode() == condition.value;
            
        case HdPackConditionType::VdpRegisterEquals:
            return console->GetVdp()->GetVdpRegister(condition.address) == condition.value;
            
        default:
            return false;
    }
}
```

### 5.2 Applying Conditions to Tiles and Backgrounds

```cpp
HdTile* SmsHdPackManager::GetHdTile(uint32_t hash, vector<int>& activeConditions)
{
    auto it = _tilesByHash.find(hash);
    if(it == _tilesByHash.end()) {
        return nullptr;
    }
    
    HdTile* bestMatch = nullptr;
    int bestPriority = -1;
    
    for(auto& tile : it->second) {
        // If the tile has no conditions, it's a potential match
        if(tile->conditionIds.empty()) {
            if(!bestMatch || tile->priority > bestPriority) {
                bestMatch = tile.get();
                bestPriority = tile->priority;
            }
            continue;
        }
        
        // Check if all conditions are met
        bool allConditionsMet = true;
        for(int conditionId : tile->conditionIds) {
            if(!_activeConditions[conditionId]) {
                allConditionsMet = false;
                break;
            }
        }
        
        if(allConditionsMet && (!bestMatch || tile->priority > bestPriority)) {
            bestMatch = tile.get();
            bestPriority = tile->priority;
        }
    }
    
    return bestMatch;
}
```

### 5.3 Background Rendering with Priority Levels

```cpp
void SmsVdp::RenderScanlineWithHdPack(uint32_t scanline)
{
    // Evaluate conditions for this frame/scanline
    _hdPackManager->EvaluateConditions(_console);
    
    // First render HD backgrounds with priority 0-9 (below everything)
    RenderHdBackgroundLayer(scanline, 0, 9);
    
    // Render background-priority sprites (normal SMS rendering)
    RenderBackgroundPrioritySprites(scanline);
    
    // Render HD backgrounds with priority 10-19 (between bg sprites and tiles)
    RenderHdBackgroundLayer(scanline, 10, 19);
    
    // Render background tiles (normal SMS rendering or HD tiles)
    RenderBackgroundTiles(scanline);
    
    // Render HD backgrounds with priority 20-29
    RenderHdBackgroundLayer(scanline, 20, 29);
    
    // Render foreground sprites (normal SMS rendering or HD sprites)
    RenderForegroundSprites(scanline);
    
    // Render HD backgrounds with priority 30-39 (above everything)
    RenderHdBackgroundLayer(scanline, 30, 39);
}
```

## 6. Implementation Strategy

### 6.1 Phase 1: Core Infrastructure (2-3 weeks)

1. Create basic `SmsHdPackManager` class
2. Implement JSON parsing for HD pack definition
3. Implement hash generation for tiles
4. Add hooks into SmsVdp rendering pipeline

### 6.2 Phase 2: Tile Replacement System (2 weeks)

1. Implement basic tile replacement rendering
2. Add support for different scale factors
3. Handle brightness adjustments
4. Support different VDP modes (SMS1 vs SMS2)

### 6.3 Phase 3: Condition System (2 weeks)

1. Implement condition evaluation framework
2. Add support for basic conditions (memory, position, etc.)
3. Implement logical operators (AND, OR, NOT)
4. Add SMS-specific conditions (VDP registers, etc.)

### 6.4 Phase 4: Advanced Features (2-3 weeks)

1. Implement background image support
2. Add priority level rendering
3. Support scroll ratios for parallax effects
4. Implement palette replacements

### 6.5 Phase 5: Audio Replacement (1 week)

1. Implement BGM replacement system
2. Add SFX replacement capabilities
3. Create audio API similar to HDNES

### 6.6 Phase 6: UI Integration and Tools (2 weeks)

1. Create HD pack builder for SMS games
2. Add UI for HD pack selection
3. Implement pack installation/uninstallation support
4. Add configuration options in emulator settings

## 7. Technical Challenges and Solutions

### 7.1 SMS VDP vs NES PPU Differences

#### Challenge
The SMS VDP's tile-based rendering differs from NES PPU, with different color depths, tile sizes, and sprite handling.

**Solution:**
- Implement SMS-specific hash generation that accounts for VDP modes
- Support both 4-bit and 3-bit color modes
- Handle SMS-specific sprite priorities and sizes

### 7.2 Memory Mapping Differences

#### Challenge
SMS memory layout differs significantly from NES, affecting memory-based conditions.

**Solution:**
- Create SMS-specific memory access helpers
- Provide clear documentation for memory addresses
- Include common memory locations for popular games

### 7.3 Performance Considerations

#### Challenge
HD rendering at high scale factors can be very demanding.

**Solution:**
- Implement scanline-based rendering and caching
- Add renderer optimizations specific to SMS graphics
- Support hardware acceleration where possible

## 8. Hash Generation Strategy

The hash generation is critical for matching original SMS tiles with HD replacements:

```cpp
uint32_t SmsVdp::GetTileHash(uint16_t tileIndex, uint8_t palette, uint8_t flags)
{
    // Start with a base value (FNV-1a hash algorithm)
    uint32_t hash = 0x811C9DC5;
    
    // Incorporate VDP mode into hash (different modes might display tiles differently)
    uint8_t vdpMode = GetVdpMode();
    hash = (hash ^ vdpMode) * 0x01000193;
    
    // Incorporate palette and flags
    hash = (hash ^ palette) * 0x01000193;
    hash = (hash ^ flags) * 0x01000193;
    
    // Get tile pattern data (8x8 pixels, each row is 4 bytes in SMS)
    uint8_t tileData[32];
    GetPatternData(tileIndex, tileData);
    
    // Hash the tile data
    for(int i = 0; i < 32; i++) {
        hash = (hash ^ tileData[i]) * 0x01000193;
    }
    
    return hash;
}
```

## 9. SMS-Specific Adaptations

### 9.1 VDP Modes and Registers

```cpp
// Condition for checking VDP mode
{
  "id": 10,
  "name": "Mode4Enabled",
  "type": "vdpModeEquals",
  "mode": 4
}

// Condition for checking VDP register values
{
  "id": 11,
  "name": "LargeSpritesEnabled",
  "type": "vdpRegisterEquals",
  "register": 1,
  "value": "02",
  "mask": "02"
}
```

### 9.2 SMS Color Handling

```cpp
struct SmsHdPalette
{
    vector<uint32_t> colors;  // ARGB format
    vector<int> conditionIds;
    
    // SMS has 2 palettes of 16 colors each
    // First 16 are background palette, next 16 are sprite palette
};
```

### 9.3 Audio API Registers

Implement the same API as HDNES, but use unused SMS memory addresses:

```
// Write registers
$FFFC: Playback Options
$FFFD: Playback Control
$FFFE: BGM Volume
$FFFF: SFX Volume
$FFF8: Album Number
$FFF9: Play BGM Track
$FFFA: Play SFX Track

// Read registers
$FFFC: Status
$FFFD: Revision Number
$FFFE/$FFFF: Signature ("SEA" - SMS Enhanced Audio)
```

## 10. Integration with Mesen2

### 10.1 Key Integration Points

```cpp
// In SmsConsole.cpp
void SmsConsole::Initialize()
{
    // Existing initialization code...
    
    // Initialize HD pack manager
    _hdPackManager = make_shared<SmsHdPackManager>();
    
    // Set it on the VDP
    _vdp->SetHdPackManager(_hdPackManager);
}

// In SmsVdp.cpp
void SmsVdp::RenderScanline(uint32_t scanline)
{
    if(_hdPackManager && _hdPackManager->IsEnabled() && _hdPackManager->HasHdPack()) {
        RenderScanlineWithHdPack(scanline);
    } else {
        // Existing rendering code
        RenderBackgroundScanline(scanline);
        RenderSpritesScanline(scanline);
    }
}
```

### 10.2 UI Integration

Add a new tab in the Video Configuration section similar to the NES HD Pack options:

```cpp
// In EmulatorWindow.cpp
void EmulatorWindow::InitializeVideoOptions()
{
    // Existing code...
    
    // Add SMS HD Pack options
    QCheckBox* enableHdPacks = new QCheckBox("Enable HD Packs for SMS/GG");
    videoLayout->addWidget(enableHdPacks);
    
    connect(enableHdPacks, &QCheckBox::toggled, [=](bool checked) {
        EmulatorConfig::SetSmsHdPacksEnabled(checked);
        if(_console && _console->GetConsoleType() == ConsoleType::Sms) {
            dynamic_cast<SmsConsole*>(_console.get())->GetHdPackManager()->SetEnabled(checked);
        }
    });
    
    // Add more options for HD Packs
}
```

## 11. Testing Strategy

### 11.1 Unit Testing

1. Hash generation consistency
2. Condition evaluation correctness
3. JSON parsing and validation

### 11.2 Game-specific Testing

1. Tests with popular SMS games:
   - Phantasy Star
   - Wonder Boy III: The Dragon's Trap
   - Sonic the Hedgehog
   - Alex Kidd in Miracle World

2. Edge case testing:
   - Games that use undocumented VDP features
   - Games that frequently switch VDP modes
   - Games with unusual sprite handling

### 11.3 Performance Testing

1. Measure performance impact at different scale factors
2. Compare different rendering optimization strategies
3. Test on lower-end hardware to ensure accessibility

## 12. Timeline and Milestones

1. **Milestone 1 (1 month):** Core infrastructure and basic tile replacement
2. **Milestone 2 (2 months):** Condition system and background support
3. **Milestone 3 (3 months):** Audio replacement and UI integration
4. **Milestone 4 (4 months):** HD Pack Builder for SMS
5. **Milestone 5 (5 months):** Documentation and first public release
6. **Milestone 6 (6 months):** Refinements based on community feedback

## 13. Resources and References

1. **Mesen2 Source Code:**
   - https://github.com/SourMesen/Mesen2
   - Key files: SmsVdp.cpp, SmsVdp.h, SmsMemoryManager.cpp

2. **HDNES Documentation:**
   - https://github.com/SourMesen/Mesen/blob/master/Docs/content/hdpacks/_index.md
   - Comprehensive documentation on the HD pack format

3. **HDNES Examples:**
   - [HDNES-POPEYE](https://github.com/LiQuiDzGit/HDNES-POPEYE) by LiQuiDzGit
   - Structure and organization of HD assets

4. **SMS Technical Documentation:**
   - SMS VDP Technical Manual
   - Z80 Programming Reference

---

*This implementation plan is a living document and will be updated as the project progresses. All timelines are estimates and subject to change based on development progress and community feedback.*
