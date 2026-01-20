# SMS HD Pack Debug Checklist

## What to Test

### 1. Start Recording
- Load an SMS ROM with visible sprites (e.g., Sonic, Alex Kidd, Wonder Boy)
- Tools → HD Pack Builder → Start Recording (choose 4x scale)
- Watch console output

### 2. Expected Debug Messages

#### Background Tile Capture (first 5 tiles):
```
[HD Capture] BG tile loaded: tileIndex=X, addr=0xXXXX, palIdx=0/1, allZero=YES/NO, scanline=Y
```
- **allZero=NO** means tile has data (good!)
- **allZero=YES** means blank tile (expected for some)

#### Sprite Tile Capture (first 20 sprites):
```
[HD Capture] Sprite captured: cycle=6/18/40/52, sprIndex=0-7, tileIndex=X, addr=0xXXXX, X=position, _currentSpriteCount=1-8
```
- Should see this if sprites are on screen
- **sprIndex** should be 0-7 (max 8 sprites per scanline)
- **_currentSpriteCount** shows how many sprites loaded

#### Scanline Summary (first 20 scanlines):
```
[HD Capture] Scanline Y: _currentSpriteCount=X, pixelsWithSprites=Y
```
- **_currentSpriteCount > 0** means sprites were loaded
- **pixelsWithSprites > 0** means sprites were stored in pixels

#### Sprite Pixel Storage (first 5 occurrences):
```
[HD Capture] Sprite pixel stored: sprIndex=X, tileIndex=Y, x=Z, y=W
```
- Confirms sprites are being written to frame buffer

#### Frame Processing (first 3 frames):
```
[SMS HD Pack] Frame 1: bgPixels=61440, spritePixels=1280
[SMS HD Pack] Frame 1 processed: bgTiles=150, spriteTiles=8, totalUniqueTiles=158
```
- **bgPixels** should be ~61440 (256×240) if all pixels have BG
- **spritePixels > 0** means sprites are in the frame buffer
- **spriteTiles > 0** means sprites were processed

### 3. Stop Recording
- Stop recording after 5-10 seconds
- Check console output:

```
[SMS HD Pack] Stopped recording - saving HD pack data
[SMS HD Pack] Total tiles captured: 250
[SMS HD Pack] Background tiles: 200
[SMS HD Pack] Separated tiles: 200 background tiles, 50 sprite tiles
[SMS HD Pack] Found 250 tiles to save
```

### 4. Check Output Files
Navigate to the HD pack folder (usually in Documents/Mesen/HdPacks/[RomName])

**Expected files:**
- `hires.txt` - Manifest file
- `BGTILES_000.png` - Background tile sheet
- `SPRITES_000.png` - Sprite tile sheet (THIS IS THE KEY!)

**If SPRITES_000.png is missing:**
- No sprites were captured
- Check console for sprite capture messages

## Troubleshooting

### Problem: No sprite capture messages at all
**Cause:** Sprites not loading or timing is wrong
**Solution:** Check `LoadSpriteTilesSms()` cycle timing

### Problem: Sprite captured messages but no sprite pixels stored
**Cause:** X position check failing in `DrawPixel()`
**Solution:** Check sprite X coordinate logic

### Problem: spritePixels=0 in frame processing
**Cause:** Frame buffer not populated with sprites
**Solution:** Check `DrawPixel()` sprite storage loop

### Problem: spriteTiles=0 in frame processing
**Cause:** Sprites not being extracted from frame buffer
**Solution:** Check `ProcessFrame()` sprite loop

### Problem: No SPRITES_000.png file
**Cause:** No sprite tiles in `_hdData.Tiles` marked as IsSprite=true
**Solution:** Check `ProcessTile()` isSprite parameter

## Current Status

Run the test and paste console output here to diagnose the issue.
