#pragma once
#include "pch.h"
#include "Utilities/HexUtilities.h"

// Forward declarations
struct HdScreenInfoSms;
struct HdPackTileInfoSms;

// SMS-specific condition type enum
enum class HdPackConditionTypeSms
{
	None,
	Tile,
	TileAtPosition,
	SpriteAtPosition,
	BgPriority,
	HMirror,
	VMirror,
	MemoryCheck,
	MemoryCheckConstant,
	FrameRange
};

// SMS-specific condition operator enum
enum class HdPackConditionOperatorSms
{
	Equal,
	NotEqual,
	LowerThan,
	GreaterThan,
	LowerThanOrEqual,
	GreaterThanOrEqual
};

// SMS-specific base condition class
struct HdPackConditionSms
{
	string Name;
	HdScreenInfoSms* _screenInfo = nullptr;

	virtual ~HdPackConditionSms() = default;

	virtual HdPackConditionTypeSms GetConditionType() = 0;
	virtual string GetConditionName() = 0;
	virtual string ToString() = 0;
	virtual bool IsExcludedFromFile() { return false; }

	void Initialize(HdScreenInfoSms* screenInfo) {
		_screenInfo = screenInfo;
		_resultCache = -1;
	}

	bool CheckCondition(int x, int y, HdPackTileInfoSms* tile);
	void ClearCache();

protected:
	bool _useCache = true;
	int8_t _resultCache = -1;

	virtual bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) = 0;
};

// SMS-specific base tile condition
struct HdPackBaseTileConditionSms : public HdPackConditionSms
{
	int32_t TileX = 0;
	int32_t TileY = 0;
	uint32_t PaletteColors = 0;
	uint8_t TileData[32] = {}; // SMS tiles are 8x8 with 32 bytes (4 bitplanes * 8 rows)
	int32_t TileIndex = 0;
	int32_t PixelOffset = 0;
	bool IgnorePalette = false;

	void Initialize(int32_t x, int32_t y, uint32_t palette, int32_t tileIndex, string tileData, bool ignorePalette);
	string ToString() override;
};

// SMS-specific base memory condition
struct HdPackBaseMemoryConditionSms : public HdPackConditionSms
{
	static constexpr uint32_t VdpMemoryMarker = 0x80000000; // SMS uses VDP instead of PPU
	uint32_t OperandA = 0;
	HdPackConditionOperatorSms Operator = {};
	uint32_t OperandB = 0;
	uint8_t Mask = 0;

	void Initialize(uint32_t operandA, HdPackConditionOperatorSms op, uint32_t operandB, uint8_t mask);
	bool IsVdpCondition();
	string ToString() override;

protected:
	virtual bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) = 0;
};

// SMS-specific horizontal mirroring condition
struct HdPackHorizontalMirroringConditionSms : public HdPackConditionSms
{
	HdPackConditionTypeSms GetConditionType() override { return HdPackConditionTypeSms::HMirror; }
	string GetConditionName() override { return "hmirror"; }
	string ToString() override { return ""; }
	bool IsExcludedFromFile() override { return true; }

	bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) override;
};

// SMS-specific vertical mirroring condition
struct HdPackVerticalMirroringConditionSms : public HdPackConditionSms
{
	HdPackConditionTypeSms GetConditionType() override { return HdPackConditionTypeSms::VMirror; }
	string GetConditionName() override { return "vmirror"; }
	string ToString() override { return ""; }
	bool IsExcludedFromFile() override { return true; }

	bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) override;
};

// SMS-specific background priority condition
struct HdPackBgPriorityConditionSms : public HdPackConditionSms
{
	HdPackConditionTypeSms GetConditionType() override { return HdPackConditionTypeSms::BgPriority; }
	string GetConditionName() override { return "bgpriority"; }
	string ToString() override { return ""; }
	bool IsExcludedFromFile() override { return true; }

	bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) override;
};

// SMS-specific memory check condition
struct HdPackMemoryCheckConditionSms : public HdPackBaseMemoryConditionSms
{
	HdPackConditionTypeSms GetConditionType() override { return HdPackConditionTypeSms::MemoryCheck; }
	HdPackMemoryCheckConditionSms() { _useCache = true; }
	string GetConditionName() override { return IsVdpCondition() ? "vdpMemoryCheck" : "memoryCheck"; }

	bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) override;
};

// SMS-specific memory check constant condition
struct HdPackMemoryCheckConstantConditionSms : public HdPackBaseMemoryConditionSms
{
	HdPackConditionTypeSms GetConditionType() override { return HdPackConditionTypeSms::MemoryCheckConstant; }
	HdPackMemoryCheckConstantConditionSms() { _useCache = true; }
	string GetConditionName() override { return IsVdpCondition() ? "vdpMemoryCheckConstant" : "memoryCheckConstant"; }

	bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) override;
};

// SMS-specific frame range condition
struct HdPackFrameRangeConditionSms : public HdPackConditionSms
{
	uint32_t OperandA = 0;
	uint32_t OperandB = 0;

	HdPackFrameRangeConditionSms() { _useCache = true; }
	HdPackConditionTypeSms GetConditionType() override { return HdPackConditionTypeSms::FrameRange; }
	string GetConditionName() override { return "frameRange"; }

	void Initialize(uint32_t operandA, uint32_t operandB);
	string ToString() override;
	bool InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile) override;
};
