#pragma once
#include "pch.h"
#include "SMS/HdPacks/HdDataSms.h"
#include "Shared/SettingTypes.h"

class Emulator;
class SmsConsole;

struct HdPackBuilderOptions {
    string SaveFolder;
    uint32_t Scale = 1;
    uint32_t ChrRamBankSize = 0x1000;
    bool GroupBlankTiles = false;
    bool SortByUsageFrequency = false;
    bool IgnoreOverscan = false;
};

class HdPackBuilderSms {
private:
    Emulator* _emu = nullptr;
    SmsConsole* _console = nullptr;
    bool _isChrRam = true;
    string _saveFolder;
    string _romName;
    HdPackBuilderOptions _options = {};
    HdPackDataSms _hdData = {};
    
    // Track tile usage
    unordered_map<HdTileKeySms, uint32_t> _tileUsageCount;
    unordered_map<HdTileKeySms, HdPackTileInfoSms*> _tilesByKey;
    uint32_t _palette[32] = {};

public:
    HdPackBuilderSms(Emulator* emu, SmsConsole* console, HdPackBuilderOptions options);
    ~HdPackBuilderSms();

    void ProcessTile(uint32_t cycle, uint32_t scanline, uint32_t tileAddr, HdTileKeySms& tile, 
                    bool isSprite, uint32_t bankHash, bool hasBgSprite);
    
    void SaveHdPack();
    
private:
    void AddTile(HdPackTileInfoSms* tile, uint32_t usageCount);
    uint32_t GetChrBankId(uint32_t tileAddr);
    void GenerateHdTile(HdPackTileInfoSms* tile);
    void SaveTileSheet(const vector<HdPackTileInfoSms*>& tiles, const string& saveFolder, int sheetIndex);
    void DrawTile(HdPackTileInfoSms* tile, int tileNumber, uint32_t* pngBuffer, int pngWidth);
    string CleanFilename(const string& filename);  // Helper for sanitizing filenames
};
