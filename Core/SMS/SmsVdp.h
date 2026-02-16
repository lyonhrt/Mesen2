#pragma once
#include "pch.h"
#include "SMS/SmsTypes.h"
#include "Shared/SettingTypes.h"
#include "Shared/ColorUtilities.h"
#include "SMS/HdPacks/HdPackSharedConstantsSms.h"
#include "Utilities/ISerializable.h"

class Emulator;
class SmsConsole;
class SmsCpu;
class SmsControlManager;
class SmsMemoryManager;

enum class SmsVdpMemAccess : uint8_t
{
	None = 0,
	BgLoadTable = 1,
	BgLoadTile = 2,
	SpriteEval = 3,
	SpriteLoadTable = 4,
	SpriteLoadTile = 5,
	CpuSlot = 6
};

class SmsVdp : public ISerializable
{
public:
	static constexpr int SmsVdpLeftBorder = 8;

	// HD tile info per screen tile (32x24 tiles for 256x192, or 32x30 for 256x240)
	// Stores lookup result so video filter can render at full scale
	struct HdTileResult {
		int ImgIndex = -1;
		uint16_t SrcX = 0;
		uint16_t SrcY = 0;
		bool HMirror = false;
		bool VMirror = false;
		bool IsSprite = false;
		bool Valid = false;
	};
	static constexpr int MaxHdTilesPerFrame = 32 * 30; // 32 columns x 30 rows max

	// Current BG tile info for per-pixel storage (NES parity: stores tile key data)
	struct HdBgTileInfo {
		uint8_t TileData[32] = {};   // 32-byte tile pattern
		SmsHdPackSharedConstants::CapturedPalette CapturedPalette;
		uint32_t PaletteColors = 0;  // Packed palette colors (legacy)
		uint8_t PaletteIndex = 0;    // 0 = low palette, 1 = high palette
		bool HMirror = false;
		bool VMirror = false;
		uint8_t RowInTile = 0;
		bool Valid = false;
		bool IsSg1000Mode = false;   // True if SG-1000/TMS9918 mode (not SMS Mode 4)
		uint8_t PixelsRemaining = 0;
		uint8_t StartColumn = 0;     // Starting column within tile (for fine scroll)
	};

	// Per-pixel tile info for video filter (NES parity)
	// Stores tile key data so video filter can do the HD lookup
	struct HdTilePixelInfo {
		uint8_t TileData[32] = {};  // 32-byte tile pattern
		SmsHdPackSharedConstants::CapturedPalette CapturedPalette;
		uint32_t PaletteColors = 0; // Packed palette colors (legacy)
		uint8_t TileX = 0;          // X position within tile (0-7)
		uint8_t TileY = 0;          // Y position within tile (0-7)
		uint8_t ColorIndex = 0;     // Palette color index (0-31) for fallback rendering
		uint8_t PaletteIndex = 0;   // 0 = low palette, 1 = high palette (for HD pack lookup)
		bool HMirror = false;
		bool VMirror = false;
		bool HasTileData = false;   // True if tile data is valid
		bool IsSg1000Mode = false;  // True if SG-1000/TMS9918 mode (not SMS Mode 4)
		bool Priority = false;      // True if BG tile has priority over sprites
	};
	
	struct HdPixelInfo {
		HdTilePixelInfo Bg;         // Background tile info
		HdTilePixelInfo Sprite;     // Sprite tile info (if sprite is visible at this pixel)
		bool HasSprite = false;     // True if a sprite is visible at this pixel
	};
	static constexpr int MaxPixelsPerFrame = 256 * 240;

	// Debug accessor for HD pack development
	uint8_t DebugReadVram(uint16_t addr) const {
		return _videoRam[addr & 0x3FFF];
	}

protected:
	Emulator* _emu = nullptr;
	SmsConsole* _console = nullptr;
	SmsCpu* _cpu = nullptr;
	SmsControlManager* _controlManager = nullptr;
	SmsMemoryManager* _memoryManager = nullptr;

	uint8_t* _videoRam = nullptr;
	uint16_t _internalPaletteRam[0x20] = {};

	uint16_t _smsSgPalette[0x10] = {
		ColorUtilities::Rgb222To555(0x00), ColorUtilities::Rgb222To555(0x00), ColorUtilities::Rgb222To555(0x08), ColorUtilities::Rgb222To555(0x0C),
		ColorUtilities::Rgb222To555(0x10), ColorUtilities::Rgb222To555(0x30), ColorUtilities::Rgb222To555(0x01), ColorUtilities::Rgb222To555(0x3C),
		ColorUtilities::Rgb222To555(0x02), ColorUtilities::Rgb222To555(0x03), ColorUtilities::Rgb222To555(0x05), ColorUtilities::Rgb222To555(0x0F),
		ColorUtilities::Rgb222To555(0x04), ColorUtilities::Rgb222To555(0x33), ColorUtilities::Rgb222To555(0x15), ColorUtilities::Rgb222To555(0x3F),
	};

	static constexpr uint16_t _originalSgPalette[0x10] = { 
		0x0000, 0x0000, 0x2324, 0x3f6b, 0x754a, 0x7dcf, 0x255a, 0x7ba8,
		0x295f, 0x3dff, 0x2b1a, 0x433c, 0x1ec4, 0x5d79, 0x6739, 0x7fff
	};

	const uint16_t* _activeSgPalette = nullptr;
	bool _disableBackground = false;
	bool _disableSprites = false;
	bool _removeSpriteLimit = false;
	SmsModel _model = {};
	SmsRevision _revision = {};

	uint16_t* _outputBuffers[2] = {};
	uint16_t* _currentOutputBuffer = nullptr;

	SmsVdpState _state = {};
	uint64_t _lastMasterClock = 0;

	uint32_t _bgShifters[4] = {};
	uint32_t _bgPriority = 0;
	uint32_t _bgPalette = 0;
	uint16_t _bgTileAddr = 0;
	// Raw BG tile index from nametable (ntData & 0x1FF), used for HD replacement key
	uint16_t _bgTileIndexRaw = 0;
	uint16_t _bgOffsetY = 0;
	uint16_t _minDrawCycle = 0;
	uint8_t _pixelsAvailable = 0;
	bool _bgHorizontalMirror = false;
	bool _bgVerticalMirror = false;
	uint8_t _bgLogicalRow = 0;  // Logical row within tile (0-7, before mirroring) - scroll-adjusted
	uint8_t _bgScreenRow = 0;   // Screen-relative row (scanline & 0x07) - for HD pack rendering
	uint8_t _hdBorderPixelsRemaining = 0; // Border pixels remaining before first tile's HD data starts

	// HD tile 3-slot ring buffer: LoadBgTilesSms shifts cur→prev→prev2, writes new to cur.
	// DrawPixel uses _pixelsAvailable to pick the correct tile and column:
	//   _pixelsAvailable > 16 → prev2 tile (only with borderWidth=7, max pixAvail=17)
	//   _pixelsAvailable > 8  → prev tile
	//   _pixelsAvailable <= 8 → current tile
	// Column within tile: (8 - _pixelsAvailable) & 7
	HdBgTileInfo _hdBgTileCur = {};
	HdBgTileInfo _hdBgTilePrev = {};
	HdBgTileInfo _hdBgTilePrev2 = {};

	HdTileResult _hdBgTiles[MaxHdTilesPerFrame] = {};
	HdTileResult _hdSpriteTiles[64 * 2] = {}; // Up to 64 sprites, 2 tiles each (tall sprites)
	
	// NES parity: Double-buffered HD pixel info to prevent tearing
	// VDP writes to _hdPixelInfoWrite, video filter reads from _hdPixelInfoRead
	// Dynamically allocated to avoid compiler heap issues with large static arrays
	HdPixelInfo* _hdPixelInfoBuffer0 = nullptr;
	HdPixelInfo* _hdPixelInfoBuffer1 = nullptr;
	HdPixelInfo* _hdPixelInfoWrite = nullptr;  // VDP writes here
	HdPixelInfo* _hdPixelInfoRead = nullptr;   // Video filter reads here

	struct SpriteShifter
	{
		uint8_t TileData[4] = {};
		uint16_t TileAddr = 0;
		int16_t SpriteX = 0;
		uint8_t SpriteRow = 0;
		bool HardwareSprite = false;

		// Raw sprite tile index read from sprite table (without pattern base),
		// used for HD replacement key to match manifest indices
		uint16_t RawTileIndex = 0;

		// HD: Snapshot of SpriteRow taken during hblank sprite loading.
		// SpriteRow gets overwritten by sprite evaluation for the next scanline
		// while rendering is still using it, causing flickering lines.
		uint8_t HdSpriteRow = 0;
	};

	uint8_t _evalCounter = 0;
	uint8_t _inRangeSpriteCount = 0;
	bool _spriteOverflowPending = false;
	
	uint8_t _spriteIndex = 0;
	uint8_t _inRangeSpriteIndex = 0;
	uint8_t _spriteCount = 0;
	uint8_t _inRangeSprites[64] = {};
	SpriteShifter _spriteShifters[64];

	uint8_t _paletteRam[0x40] = {};
	uint16_t _scanlineCount = 262;
	ConsoleRegion _region = ConsoleRegion::Ntsc;

	SmsVdpWriteType _writePending = SmsVdpWriteType::None;
	bool _readPending = false;

	bool _latchRequest = false;
	uint8_t _latchPos = 0;

	bool _needCramDot = false;
	uint16_t _cramDotColor = 0;

	//Used by SG-1000 modes
	uint16_t _bgTileIndex = 0;
	uint8_t _bgPatternData = 0;
	uint8_t _textModeStep = 0;

	SmsVdpMemAccess _memAccess[342] = {};

	void UpdateIrqState();

	void UpdateDisplayMode();

	uint8_t ReadVerticalCounter();

	__forceinline uint8_t ReadVram(uint16_t addr, SmsVdpMemAccess type);
	__forceinline void WriteVram(uint16_t addr, uint8_t value, SmsVdpMemAccess type);

	void DebugProcessMemoryAccessView();
	__forceinline void ProcessVramAccess();
	void ProcessVramWrite();

	uint8_t ReverseBitOrder(uint8_t val);
	
	__forceinline void Exec();
	__forceinline void ExecForcedBlank();
	__forceinline void ProcessForcedBlankVblank();

	int GetVisiblePixelIndex();
	virtual void LoadBgTilesSms();
	virtual void LoadBgTilesSg();
	void LoadBgTilesSgTextMode();
	void PushBgPixel(uint8_t color, int index);
	
	virtual void DrawPixel();

	void ProcessScanlineEvents();
	virtual void ProcessEndOfScanline();

	__forceinline void ProcessSpriteEvaluation();

	uint16_t GetSmsSpriteTileAddr(uint8_t sprTileIndex, uint8_t spriteRow, uint8_t i);
	virtual void LoadSpriteTilesSms();
	void LoadExtraSpritesSms();
	__forceinline uint16_t GetPixelColor();

	virtual void LoadSpriteTilesSg();
	void LoadExtraSpritesSg();
	void ShiftSprite(uint8_t sprIndex);
	void ShiftSpriteSg(uint8_t sprIndex);

	// HD pack helper: lookup and cache sprite HD replacement
	void LookupSpriteHdReplacement(uint8_t spriteIndex);

	__forceinline bool IsZoomedSpriteAllowed(int spriteIndex);

	void WriteRegister(uint8_t reg, uint8_t value);
	void WriteSmsPalette(uint8_t addr, uint8_t value);
	void WriteGameGearPalette(uint8_t addr, uint16_t value);

	void InitSmsPostBiosState();
	void InitGgPowerOnState();
	
	void UpdateConfig();

public:
	void Init(Emulator* emu, SmsConsole* console, SmsCpu* cpu, SmsControlManager* controlManager, SmsMemoryManager* memoryManager);
	virtual ~SmsVdp();

	void Run(uint64_t runTo);

	void WritePort(uint8_t port, uint8_t value);
	uint8_t ReadPort(uint8_t port);
	uint8_t PeekPort(uint8_t port);

	void SetLocationLatchRequest(uint8_t x);
	void InternalLatchHorizontalCounter(uint16_t cycle);
	void LatchHorizontalCounter();
	void SetRegion(ConsoleRegion region);

	void DebugSendFrame();
	uint16_t GetScanline() { return _state.Scanline; }
	uint16_t GetScanlineCount() { return _scanlineCount; }
	uint16_t GetCycle() { return _state.Cycle; }
	uint16_t GetFrameCount() { return _state.FrameCount; }
	uint32_t GetPixelBrightness(uint8_t x, uint8_t y);
	int GetViewportYOffset();
	const uint16_t* GetSmsSgPalette() { return _activeSgPalette; }
	SmsVdpState& GetState() { return _state; }

	// HD Pack palette access methods
	uint8_t* GetPaletteRam() { return _paletteRam; }
	const uint16_t* GetInternalPaletteRam() const { return _internalPaletteRam; }

	void DebugWritePalette(uint8_t addr, uint8_t value);

	uint16_t* GetScreenBuffer(bool previousBuffer)
	{
		return previousBuffer ? ((_currentOutputBuffer == _outputBuffers[0]) ? _outputBuffers[1] : _outputBuffers[0]) : _currentOutputBuffer;
	}

	// HD tile result accessors for video filter
	const HdTileResult* GetHdBgTiles() const { return _hdBgTiles; }
	const HdTileResult* GetHdSpriteTiles() const { return _hdSpriteTiles; }
	const HdPixelInfo* GetHdPixelInfo() const { return _hdPixelInfoRead; }  // Video filter reads from read buffer
	void ClearHdTileResults();
	void SwapHdPixelBuffers();  // Swap read/write buffers at frame end

	void Serialize(Serializer& s) override;
};