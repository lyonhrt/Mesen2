#include "pch.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include "SMS/HdPacks/SmsHdPackLoader.h"
#include "SMS/HdPacks/HdDataSms.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/VirtualFile.h"
#include "Shared/MessageManager.h"

using namespace std;

#define logError(y) MessageManager::Log(string("[SMS HDPack - Line ") + std::to_string(_currentLine) + "] " + (y)); _errorCount++

SmsHdPackLoader::SmsHdPackLoader()
{
}

bool SmsHdPackLoader::LoadHdSmsPack(std::string definitionFile, HdPackDataSms& outData)
{
    SmsHdPackLoader loader;
    std::ifstream defStream(definitionFile, std::ios::in | std::ios::binary);
    if(defStream.good()) {
        loader._data = &outData;
        loader._hdPackFolder = FolderUtilities::GetFolderName(definitionFile);
        return loader.LoadPack();
    }
    return false;
}

bool SmsHdPackLoader::LoadHdSmsPack(VirtualFile& romFile, HdPackDataSms& outData)
{
    SmsHdPackLoader loader;
    if(loader.InitializeLoader(romFile, &outData)) {
        return loader.LoadPack();
    }
    return false;
}

bool SmsHdPackLoader::InitializeLoader(VirtualFile& romFile, HdPackDataSms* data)
{
    _data = data;

    string romName = FolderUtilities::GetFilename(romFile.GetFileName(), false);
    string hdPackFolder = FolderUtilities::GetHdPackFolder();
    string definitionPath = FolderUtilities::CombinePath(romName, "hires.txt");

    string hdPackPath = FolderUtilities::CombinePath(hdPackFolder, definitionPath);
    std::ifstream defStream(hdPackPath, std::ios::in | std::ios::binary);
    if(defStream.good()) {
        _hdPackFolder = FolderUtilities::GetFolderName(hdPackPath);
        return true;
    }

    return false;
}

bool SmsHdPackLoader::CheckFile(std::string filename)
{
    std::ifstream file(FolderUtilities::CombinePath(_hdPackFolder, filename), std::ios::in | std::ios::binary);
    if(file.good()) { return true; }
    return false;
}

bool SmsHdPackLoader::LoadFile(std::string filename, std::vector<uint8_t>& fileData)
{
    fileData.clear();
    std::ifstream file(FolderUtilities::CombinePath(_hdPackFolder, filename), std::ios::in | std::ios::binary);
    if(file.good()) {
        file.seekg(0, std::ios::end);
        uint32_t fileSize = (uint32_t)file.tellg();
        file.seekg(0, std::ios::beg);

        fileData.resize(fileSize);
        file.read((char*)fileData.data(), fileSize);
        return true;
    }
    return false;
}

static inline string trim_copy(const string& s)
{
    string r = s;
    StringUtilities::Trim(r);
    return r;
}

bool SmsHdPackLoader::LoadPack()
{
    string lineContent;
    _currentLine = 0;

    try {
        vector<uint8_t> hdDefinition;
        if(!LoadFile("hires.txt", hdDefinition)) {
            return false;
        }

        size_t len = hdDefinition.size();
        size_t pos = 0;
        while(pos < len) {
            lineContent.clear();

            size_t start = pos;
            for(; pos < len; pos++) {
                if(hdDefinition[pos] == '\n') {
                    pos++;
                    break;
                }
            }

            lineContent.insert(0, (char*)hdDefinition.data() + start, pos < len ? (pos - start - 1) : (len - start));
            _currentLine++;

            if(lineContent.empty()) { continue; }

            while(lineContent.size() > 0 && (lineContent.back() == '\r' || lineContent.back() == '\n')) {
                lineContent.pop_back();
            }

            // Skip comments
            string trimmed = lineContent;
            StringUtilities::Trim(trimmed);
            if(trimmed.empty() || trimmed[0] == '#') { continue; }

            if(trimmed.rfind("<scale>", 0) == 0) {
                // e.g. <scale>2</scale>
                string value = trimmed.substr(7); // stoi stops at non-digit
                _data->Scale = std::max(1, std::min(10, stoi(value)));
                continue;
            }

            // Parse <tile>tileIndex,paletteIndex,x,y,filename</tile>
            // Be robust to closing tag
            if(trimmed.find("<tile>") == 0) {
                string content = trimmed.substr(6);
                size_t closePos = content.find("</tile>");
                if(closePos != string::npos) {
                    content = content.substr(0, closePos);
                }

                vector<string> tokens = StringUtilities::Split(content, ',');
                ProcessTileTag(tokens);
                continue;
            }
        }

        // Build TileByKey lookup like NES InitializeHdPack()
        MessageManager::Log("[SMS HDPack] Building TileByKey from " + std::to_string(_data->Tiles.size()) + " tiles");
        
        for(unique_ptr<HdPackTileInfoSms>& tileInfo : _data->Tiles) {
            HdTileKeySms keyFalse = tileInfo->GetKey(false);
            auto it = _data->TileByKey.find(keyFalse);
            if(it == _data->TileByKey.end()) {
                _data->TileByKey[keyFalse] = vector<HdPackTileInfoSms*>();
            }
            _data->TileByKey[keyFalse].push_back(tileInfo.get());

            if(tileInfo->DefaultTile) {
                HdTileKeySms keyTrue = tileInfo->GetKey(true);
                auto it2 = _data->TileByKey.find(keyTrue);
                if(it2 == _data->TileByKey.end()) {
                    _data->TileByKey[keyTrue] = vector<HdPackTileInfoSms*>();
                }
                _data->TileByKey[keyTrue].push_back(tileInfo.get());
            }
        }
        
        MessageManager::Log("[SMS HDPack] TileByKey has " + std::to_string(_data->TileByKey.size()) + " entries, scale=" + std::to_string(_data->Scale));

        if(_errorCount > 0) {
            MessageManager::Log(string("[SMS HDPack] Loaded with ") + std::to_string(_errorCount) + " errors");
        }

        return true;
    } catch(std::exception& ex) {
        logError(string("Error: ") + ex.what() + " on line: " + lineContent);
        return false;
    }
}

// Helper to parse hex string to bytes
static bool HexToBytes(const std::string& hex, uint8_t* out, size_t outLen) {
    if(hex.length() != outLen * 2) {
        return false;
    }
    for(size_t i = 0; i < outLen; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        int hiVal = (hi >= '0' && hi <= '9') ? (hi - '0') : ((hi >= 'a' && hi <= 'f') ? (hi - 'a' + 10) : ((hi >= 'A' && hi <= 'F') ? (hi - 'A' + 10) : -1));
        int loVal = (lo >= '0' && lo <= '9') ? (lo - '0') : ((lo >= 'a' && lo <= 'f') ? (lo - 'a' + 10) : ((lo >= 'A' && lo <= 'F') ? (lo - 'A' + 10) : -1));
        if(hiVal < 0 || loVal < 0) {
            return false;
        }
        out[i] = (uint8_t)((hiVal << 4) | loVal);
    }
    return true;
}

void SmsHdPackLoader::ProcessTileTag(const std::vector<std::string>& inTokens)
{
    vector<string> tokens = inTokens;
    for(string& t : tokens) { StringUtilities::Trim(t); }

    // New format: tileData(64 hex chars),paletteIndex,isSprite,x,y,filename
    // Old format: tileIndex,paletteIndex,x,y,filename (5 tokens)
    
    bool isNewFormat = (tokens.size() >= 6 && tokens[0].length() == 64);
    
    if(isNewFormat) {
        // New format with tile data hex
        if(tokens.size() < 6) {
            logError("Invalid <tile> tag - expected 6 parameters for new format");
            return;
        }
        
        string tileDataHex = tokens[0];
        int paletteIndex = 0;
        int isSprite = 0;
        int x = 0;
        int y = 0;
        
        try {
            paletteIndex = stoi(tokens[1]);
            isSprite = stoi(tokens[2]);
            x = stoi(tokens[3]);
            y = stoi(tokens[4]);
        } catch(...) {
            logError("Invalid numeric parameter in <tile> tag");
            return;
        }
        
        string filename = tokens[5];
        // Remove potential trailing tag remnants
        size_t tagPos = filename.find("</tile>");
        if(tagPos != string::npos) {
            filename = filename.substr(0, tagPos);
            StringUtilities::Trim(filename);
        }
        
        // Parse tile data from hex
        uint8_t tileData[32] = {};
        if(!HexToBytes(tileDataHex, tileData, 32)) {
            logError("Invalid tile data hex string");
            return;
        }
        
        // Load or reuse bitmap entry
        uint32_t bmpIndex = 0;
        auto it = _imageIndexByName.find(filename);
        if(it == _imageIndexByName.end()) {
            vector<uint8_t> fileData;
            if(!LoadFile(filename, fileData)) {
                logError("Missing image file: " + filename);
                return;
            }
            auto bmp = std::make_unique<HdPackBitmapInfoSms>();
            bmp->PngName = filename;
            bmp->FileData = std::move(fileData);
            bmpIndex = (uint32_t)_data->ImageFileData.size();
            _data->ImageFileData.push_back(std::move(bmp));
            _imageIndexByName[filename] = bmpIndex;
        } else {
            bmpIndex = it->second;
        }
        
        auto tile = std::make_unique<HdPackTileInfoSms>();
        
        // Key fields - use tile data for VRAM tile matching
        tile->TileIndex = -1; // Not used for VRAM tiles
        tile->IsVramTile = true;
        memcpy(tile->TileData, tileData, 32);
        
        // Palette handling
        tile->PaletteIndex = (uint8_t)std::clamp(paletteIndex, 0, 1);
        tile->PaletteColors = tile->PaletteIndex ? 0x00FF0000 : 0x00000000;
        
        // Sprite/background flag
        tile->IsSprite = (isSprite != 0);
        
        // Bitmap reference and coordinates
        tile->BitmapIndex = bmpIndex;
        tile->Bitmap = _data->ImageFileData[bmpIndex].get();
        tile->X = (uint32_t)std::max(0, x);
        tile->Y = (uint32_t)std::max(0, y);
        tile->Width = 8u * std::max(1u, _data->Scale);
        tile->Height = 8u * std::max(1u, _data->Scale);
        
        _data->Tiles.push_back(std::move(tile));
    } else {
        // Old format (backward compatibility)
        if(tokens.size() < 5) {
            logError("Invalid <tile> tag - expected 5 parameters");
            return;
        }

        int tileIndex = 0;
        int paletteIndex = 0;
        int x = 0;
        int y = 0;

        try {
            tileIndex = stoi(tokens[0]);
            paletteIndex = stoi(tokens[1]);
            x = stoi(tokens[2]);
            y = stoi(tokens[3]);
        } catch(...) {
            logError("Invalid numeric parameter in <tile> tag");
            return;
        }

        string filename = tokens[4];
        // Remove potential trailing tag remnants
        size_t tagPos = filename.find("</tile>");
        if(tagPos != string::npos) {
            filename = filename.substr(0, tagPos);
            StringUtilities::Trim(filename);
        }

        // Load or reuse bitmap entry
        uint32_t bmpIndex = 0;
        auto it = _imageIndexByName.find(filename);
        if(it == _imageIndexByName.end()) {
            vector<uint8_t> fileData;
            if(!LoadFile(filename, fileData)) {
                logError("Missing image file: " + filename);
                return;
            }
            auto bmp = std::make_unique<HdPackBitmapInfoSms>();
            bmp->PngName = filename;
            bmp->FileData = std::move(fileData);
            bmpIndex = (uint32_t)_data->ImageFileData.size();
            _data->ImageFileData.push_back(std::move(bmp));
            _imageIndexByName[filename] = bmpIndex;
        } else {
            bmpIndex = it->second;
        }

        auto tile = std::make_unique<HdPackTileInfoSms>();

        // Key fields
        tile->TileIndex = tileIndex;
        tile->IsVramTile = false;

        // Palette handling for key normalization: 0 -> 0, 1 -> set a non-zero bit in high byte
        tile->PaletteIndex = (uint8_t)std::clamp(paletteIndex, 0, 1);
        tile->PaletteColors = tile->PaletteIndex ? 0x00FF0000 : 0x00000000;

        // Sprite/background hint
        tile->IsSprite = (tile->PaletteIndex != 0);

        // Bitmap reference and coordinates
        tile->BitmapIndex = bmpIndex;
        tile->Bitmap = _data->ImageFileData[bmpIndex].get();
        tile->X = (uint32_t)std::max(0, x);
        tile->Y = (uint32_t)std::max(0, y);
        tile->Width = 8u * std::max(1u, _data->Scale);
        tile->Height = 8u * std::max(1u, _data->Scale);

        _data->Tiles.push_back(std::move(tile));
    }
}
