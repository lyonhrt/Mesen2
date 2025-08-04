#pragma once
#include "pch.h"
#include "Utilities/VirtualFile.h"
#include <unordered_map>
#include <string>
#include <vector>

class Emulator;
class SmsConsole;

// SMS HD Tile Dumper - Captures tiles as they are rendered and saves them as PNGs
class SmsHdTileDumper
{
public:
    SmsHdTileDumper(Emulator* emu, const string& romHash);
    ~SmsHdTileDumper();

    // Initialize the dumper with the output folder
    void Initialize(const string& outputFolder);

    // Process a tile (background or sprite) and dump it if needed
    void DumpTile(uint8_t* patternData, int paletteIndex, bool hFlip, bool vFlip, int x, int y, bool isSprite);

    // Save the manifest file with all tile references
    void SaveManifest();

    // Check if dumping is enabled
    static bool IsDumpingEnabled();

    // Toggle dumping on/off
    static void SetDumpingEnabled(bool enabled);

private:
    // Generate a hash for a tile + palette + flip combination
    string GenerateTileHash(uint8_t* patternData, int paletteIndex, bool hFlip, bool vFlip);

    // Convert SMS tile data to RGBA using the current palette
    void ConvertTileToRgba(uint8_t* patternData, int paletteIndex, bool hFlip, bool vFlip, uint32_t* outputBuffer);

    // Save a tile as a PNG file
    string SaveTilePng(uint32_t* rgbaData, const string& hash);

    // Add an entry to the manifest
    void AddManifestEntry(int x, int y, int paletteIndex, bool hFlip, bool vFlip, bool isSprite, const string& pngFile);

    // Get the current SMS palette
    const uint16_t* GetSmsPalette();

private:
    Emulator* _emu = nullptr;
    SmsConsole* _console = nullptr;
    string _romHash;
    string _outputFolder;
    string _graphicsFolder;
    string _manifestPath;

    // Track tiles we've already dumped to avoid duplicates
    std::unordered_map<string, string> _tileHashToPngMap;

    // Manifest entries
    std::vector<string> _manifestEntries;

    // Static flag for enabling/disabling dumping
    static bool _dumpingEnabled;
};
