#include "pch.h"
#include "SMS/HdPacks/HdPackConditionsSms.h"
#include "SMS/HdPacks/HdDataSms.h"

// Implementation of HdPackConditionSms methods
bool HdPackConditionSms::CheckCondition(int x, int y, HdPackTileInfoSms* tile)
{
    if(_useCache && _resultCache >= 0) {
        return _resultCache == 1;
    }

    bool result = InternalCheckCondition(x, y, tile);
    if(_useCache) {
        _resultCache = result ? 1 : 0;
    }
    return result;
}

void HdPackConditionSms::ClearCache()
{
    _resultCache = -1;
}

// Implementation of HdPackBaseTileConditionSms methods
void HdPackBaseTileConditionSms::Initialize(int32_t x, int32_t y, uint32_t palette, int32_t tileIndex, string tileData, bool ignorePalette)
{
    TileX = x;
    TileY = y;
    PixelOffset = (y * 256) + x;
    PaletteColors = palette;
    TileIndex = tileIndex;
    IgnorePalette = ignorePalette;
    if(tileData.size() == 64) { // SMS tiles have 64 hex chars (32 bytes)
        for(int i = 0; i < 32; i++) {
            TileData[i] = HexUtilities::FromHex(tileData.substr(i * 2, 2));
        }
        TileIndex = -1;
    }
}

string HdPackBaseTileConditionSms::ToString()
{
    stringstream out;
    out << "<condition>" << Name << "," << GetConditionName() << ",";
    out << TileX << ",";
    out << TileY << ",";
    if(TileIndex >= 0) {
        out << HexUtilities::ToHex(TileIndex) << ",";
    } else {
        for(int i = 0; i < 32; i++) {
            out << HexUtilities::ToHex(TileData[i]);
        }
    }
    out << HexUtilities::ToHex(PaletteColors, true);

    return out.str();
}

// Implementation of HdPackBaseMemoryConditionSms methods
void HdPackBaseMemoryConditionSms::Initialize(uint32_t operandA, HdPackConditionOperatorSms op, uint32_t operandB, uint8_t mask)
{
    OperandA = operandA;
    Operator = op;
    OperandB = operandB;
    Mask = mask;
}

bool HdPackBaseMemoryConditionSms::IsVdpCondition()
{
    return (OperandA & HdPackBaseMemoryConditionSms::VdpMemoryMarker) != 0;
}

string HdPackBaseMemoryConditionSms::ToString()
{
    stringstream out;
    out << "<condition>" << Name << "," << GetConditionName() << ",";
    out << HexUtilities::ToHex(OperandA & 0xFFFF) << ",";
    switch(Operator) {
        case HdPackConditionOperatorSms::Equal: out << "=="; break;
        case HdPackConditionOperatorSms::NotEqual: out << "!="; break;
        case HdPackConditionOperatorSms::GreaterThan: out << ">"; break;
        case HdPackConditionOperatorSms::LowerThan: out << "<"; break;
        case HdPackConditionOperatorSms::LowerThanOrEqual: out << "<="; break;
        case HdPackConditionOperatorSms::GreaterThanOrEqual: out << ">="; break;
    }
    out << ",";
    out << HexUtilities::ToHex(OperandB);
    out << ",";
    out << HexUtilities::ToHex(Mask);

    return out.str();
}

// Implementation of HdPackHorizontalMirroringConditionSms methods
bool HdPackHorizontalMirroringConditionSms::InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile)
{
    return tile && tile->HorizontalMirroring;
}

// Implementation of HdPackVerticalMirroringConditionSms methods
bool HdPackVerticalMirroringConditionSms::InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile)
{
    return tile && tile->VerticalMirroring;
}

// Implementation of HdPackBgPriorityConditionSms methods
bool HdPackBgPriorityConditionSms::InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile)
{
    return tile && tile->BackgroundPriority;
}

// Implementation of HdPackMemoryCheckConditionSms methods
bool HdPackMemoryCheckConditionSms::InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile)
{
    uint8_t a = (uint8_t)(_screenInfo->WatchedAddressValues[OperandA] & Mask);
    uint8_t b = (uint8_t)(_screenInfo->WatchedAddressValues[OperandB] & Mask);

    switch(Operator) {
        case HdPackConditionOperatorSms::Equal: return a == b;
        case HdPackConditionOperatorSms::NotEqual: return a != b;
        case HdPackConditionOperatorSms::GreaterThan: return a > b;
        case HdPackConditionOperatorSms::LowerThan: return a < b;
        case HdPackConditionOperatorSms::LowerThanOrEqual: return a <= b;
        case HdPackConditionOperatorSms::GreaterThanOrEqual: return a >= b;
    }
    return false;
}

// Implementation of HdPackMemoryCheckConstantConditionSms methods
bool HdPackMemoryCheckConstantConditionSms::InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile)
{
    uint8_t a = (uint8_t)(_screenInfo->WatchedAddressValues[OperandA] & Mask);
    uint8_t b = OperandB;

    switch(Operator) {
        case HdPackConditionOperatorSms::Equal: return a == b;
        case HdPackConditionOperatorSms::NotEqual: return a != b;
        case HdPackConditionOperatorSms::GreaterThan: return a > b;
        case HdPackConditionOperatorSms::LowerThan: return a < b;
        case HdPackConditionOperatorSms::LowerThanOrEqual: return a <= b;
        case HdPackConditionOperatorSms::GreaterThanOrEqual: return a >= b;
    }
    return false;
}

// Implementation of HdPackFrameRangeConditionSms methods
void HdPackFrameRangeConditionSms::Initialize(uint32_t operandA, uint32_t operandB)
{
    OperandA = operandA;
    OperandB = operandB;
}

string HdPackFrameRangeConditionSms::ToString()
{
    stringstream out;
    out << "<condition>" << Name << "," << GetConditionName() << ",";
    out << OperandA << ",";
    out << OperandB;

    return out.str();
}

bool HdPackFrameRangeConditionSms::InternalCheckCondition(int x, int y, HdPackTileInfoSms* tile)
{
    return _screenInfo->FrameNumber % OperandA >= OperandB;
}
