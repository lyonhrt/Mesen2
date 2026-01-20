#pragma once
#define SMS_HD_DEBUG 1
#include "pch.h"
#include "SMS/HdPacks/SmsHdPackApi.h"
#include "SMS/HdPacks/HdPackBuilderSms.h"
#include "SMS/HdPacks/SmsHdTileDumper.h"
#include "SMS/HdPacks/HdPackDebug.h"
#include "SMS/SmsConsole.h"
#include "SMS/HdPacks/HdBuilderSmsVdp.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include <memory>
// Loader dependencies
#include <array>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Utilities/PNGHelper.h"
#include <iomanip>
#include <algorithm>
#include <map>
#include "Utilities/HexUtilities.h"
#include "SMS/HdPacks/HdDataSms.h"

namespace SmsHdPackApi {
    // Global state for HD pack dumping
    static std::unique_ptr<HdPackBuilderSms> g_hdPackBuilder;
    static std::unique_ptr<SmsHdTileDumper> g_hdTileDumper;
    static HdScreenInfoSms* g_lastProcessedFrame = nullptr;
    
    // ---------------- HD Replacement Loader (types) ----------------
    struct HdImage { std::string Filename; std::vector<uint32_t> Pixels; uint32_t W=0, H=0; };
    struct HdKey {
        uint32_t TileIndex;
        uint8_t PaletteGroup;
        uint8_t IsSprite;
        bool operator==(const HdKey& o) const {
            return TileIndex == o.TileIndex && PaletteGroup == o.PaletteGroup && IsSprite == o.IsSprite;
        }
    };
    struct HdKeyHash {
        size_t operator()(const HdKey& k) const {
            size_t h = 1469598103934665603ull;
            auto mix=[&](size_t v){ h ^= v; h *= 1099511628211ull; };
            mix((size_t)k.TileIndex); mix(k.PaletteGroup); mix(k.IsSprite); return h;
        }
    };

    struct HdEntry { int ImgIndex=-1; uint16_t X=0; uint16_t Y=0; bool IsSprite=false; uint8_t PaletteGroup=0; };
    struct ParsedTiles { std::vector<std::array<std::string,7>> rows; std::vector<bool> isSprite; };
    static bool g_hdReplacementEnabled = true;
    static bool g_hdPackLoaded = false;
    static uint32_t g_hdPackScale = 1;
    static std::vector<HdImage> g_hdImages;
    static std::unordered_map<HdKey,HdEntry,HdKeyHash> g_hdMap;
    static std::unordered_map<uint64_t, HdEntry> g_hdHashMap; // pattern+pal+isSprite hash -> entry

    // Dumping state (shared with UI + VDP hooks)
    static bool g_isDumping = false;

    static inline uint64_t ComputeCanonicalHash(const uint8_t* tileData32, bool isSprite, uint8_t palGroup)
    {
        // Matches HdPackBuilderSms::GetCanonicalHash: FNV-1a over 32 bytes, then palGroup, then sprite flag
        const uint64_t FNV_OFFSET = 1469598103934665603ULL;
        const uint64_t FNV_PRIME  = 1099511628211ULL;
        uint64_t h = FNV_OFFSET;
        for(int i = 0; i < 32; i++) {
            h ^= (uint64_t)tileData32[i];
            h *= FNV_PRIME;
        }
        h ^= (uint64_t)palGroup; h *= FNV_PRIME;
        h ^= (uint64_t)(isSprite ? 1 : 0); h *= FNV_PRIME;
        return h;
    }

    static std::string BytesToHex(const uint8_t* data, size_t len, size_t maxOut = 32)
    {
        if(!data) return std::string();
        size_t n = std::min(len, maxOut);
        std::ostringstream ss; ss << std::uppercase << std::hex << std::setfill('0');
        for(size_t i = 0; i < n; i++) {
            ss << std::setw(2) << (int)data[i];
        }
        return ss.str();
    }

    static std::string Hex64_16(uint64_t v)
    {
        std::ostringstream ss; ss << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << v; return ss.str();
    }

    static bool ParseHexToBytes(const std::string& hex, std::array<uint8_t,32>& out)
    {
        if(hex.size() < 64) return false; // need 32 bytes
        auto hexVal = [](char c)->int{
            if(c >= '0' && c <= '9') return c - '0';
            if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        size_t count = 0;
        for(size_t i = 0; i + 1 < hex.size() && count < 32; i += 2) {
            int hi = hexVal(hex[i]);
            int lo = hexVal(hex[i+1]);
            if(hi < 0 || lo < 0) return false;
            out[count++] = (uint8_t)((hi << 4) | lo);
        }
        return count == 32;
    }

    static inline uint8_t NormalizePaletteGroup(uint32_t paletteColors) {
        // Derive palette group from palette colors: check if third byte is non-zero (high palette)
        // This is used when loading existing HD packs where we only have PaletteColors, not PaletteIndex
        return (((paletteColors >> 16) & 0xFF) ? 1 : 0);
    }

    // Parse HDNES-style hires.txt manifest used by SMS builder
    static bool ParseManifest(const std::string& manifestPath, std::vector<std::string>& outImgs, ParsedTiles& outTiles, uint32_t& outScale)
    {
        outImgs.clear(); outTiles.rows.clear(); outTiles.isSprite.clear(); outScale = 1;
        std::ifstream in(manifestPath);
        if(!in.is_open()) return false;
        std::string line; bool sectionIsSprite = false;
        while(std::getline(in, line)) {
            // trim leading/trailing whitespace
            auto ltrim=[&](std::string& s){ size_t i = s.find_first_not_of(" \t\r\n"); if(i==std::string::npos){ s.clear(); } else { s.erase(0,i);} };
            auto rtrim=[&](std::string& s){ size_t i = s.find_last_not_of(" \t\r\n"); if(i==std::string::npos){ s.clear(); } else { s.erase(i+1);} };
            rtrim(line); ltrim(line);
            if(line.empty()) continue;
            if(line[0] == '#') {
                // Section header: #filename.png
                std::string name = line.substr(1);
                ltrim(name); rtrim(name);
                // Consider sheets named SPRITES_###.png as sprite sheets
                sectionIsSprite = (name.find("SPRITES") != std::string::npos);
                continue;
            }
            if(line.rfind("<scale>", 0) == 0) {
                std::string rest = line.substr(7);
                // Stop at next tag if present
                size_t lt = rest.find('<'); if(lt != std::string::npos) rest = rest.substr(0, lt);
                ltrim(rest); rtrim(rest);
                try { int s = std::stoi(rest); if(s >= 1 && s <= 10) outScale = (uint32_t)s; } catch(...) {}
                continue;
            }
            if(line.rfind("<img>", 0) == 0) {
                std::string fn = line.substr(5); ltrim(fn); rtrim(fn);
                outImgs.push_back(fn);
                continue;
            }
            if(line.rfind("<tile>", 0) == 0) {
                std::string content = line.substr(6);
                // Remove possible trailing closing tag
                size_t close = content.find("</tile>"); if(close != std::string::npos) content = content.substr(0, close);
                // Split by comma
                std::vector<std::string> parts; parts.reserve(8);
                std::stringstream ss(content); std::string tok;
                while(std::getline(ss, tok, ',')) { ltrim(tok); rtrim(tok); parts.push_back(tok); }
                if(parts.size() < 5) continue; // require at least imgIndex, tileHex, paletteHex, x, y
                std::array<std::string,7> row{};
                for(size_t i=0;i<7;i++) {
                    row[i] = (i < parts.size()) ? parts[i] : std::string();
                }
                // Ensure brightness and default fields present (indices 5,6)
                if(row[5].empty()) row[5] = "1";
                if(row[6].empty()) row[6] = "N";
                outTiles.rows.push_back(row);
                outTiles.isSprite.push_back(sectionIsSprite);
                continue;
            }
        }
        return true;
    }

    void LoadHdPackIfAvailable(Emulator* emu)
    {
        g_hdPackLoaded = false; g_hdImages.clear(); g_hdMap.clear(); g_hdHashMap.clear(); g_hdPackScale = 1;

        if(!emu || emu->GetConsoleType()!=ConsoleType::Sms) return;
        std::string packFolder = FolderUtilities::CombinePath(FolderUtilities::GetHdPackFolder(), FolderUtilities::GetFilename(emu->GetRomInfo().RomFile.GetFileName(), false));
        std::string manifestPath = FolderUtilities::CombinePath(packFolder, "hires.txt");
        MessageManager::Log(std::string("[SMS HD Pack] Looking for hires.txt at: ") + manifestPath);
        std::error_code ec; 
        if(!std::filesystem::exists(std::filesystem::u8path(manifestPath), ec)) {
            MessageManager::Log(std::string("[SMS HD Pack] No HD pack found (missing hires.txt): ") + manifestPath);
            return;
        }

        std::vector<std::string> imgs; ParsedTiles tiles; uint32_t scale=1;
        if(!ParseManifest(manifestPath, imgs, tiles, scale)) {
            MessageManager::Log(std::string("[SMS HD Pack] Failed to parse hires.txt: ") + manifestPath);
            return;
        }
        MessageManager::Log("[SMS HD Pack] Parsed hires.txt: imgs=" + std::to_string(imgs.size()) + 
            ", tiles=" + std::to_string(tiles.rows.size()) + ", scale=" + std::to_string(scale));
        g_hdPackScale = scale;

        // Load images
        for(const std::string& rel : imgs) {
            std::string full = FolderUtilities::CombinePath(packFolder, rel);
            HdImage img; img.Filename = rel;
            std::vector<uint8_t> decodedBytes; uint32_t w=0,h=0;
            if(PNGHelper::ReadPNG(full, decodedBytes, w, h)) {
                // decodedBytes contains 32bpp ARGB bytes (PNGHelper converts ABGR->ARGB)
                if(decodedBytes.size() == (size_t)w * (size_t)h * 4) {
                    img.W = w; img.H = h;
                    img.Pixels.resize((size_t)w * (size_t)h);
                    memcpy(img.Pixels.data(), decodedBytes.data(), decodedBytes.size());
                    g_hdImages.push_back(std::move(img));
                } else {
                    g_hdImages.push_back(HdImage{rel,{/*empty*/},0,0});
                    MessageManager::Log("[SMS HD Pack] PNG size mismatch: " + full);
                }
            } else {
                // push empty to keep indices aligned
                g_hdImages.push_back(HdImage{rel,{/*empty*/},0,0});
                MessageManager::Log("[SMS HD Pack] Failed to load PNG: " + full);
            }
        }

        // Build mapping (tileIndex/paletteGroup/isSprite) -> imgIndex,x,y
        static int s_tileLogCount = 0;
        for(size_t i=0;i<tiles.rows.size();i++){
            const auto& p = tiles.rows[i]; bool isSpr = tiles.isSprite[i];
            int imgIndex = -1; try { imgIndex = std::stoi(p[0]); } catch(...) { continue; }
            if(imgIndex < 0 || imgIndex >= (int)g_hdImages.size()) continue;
            const std::string& tileField = p[1];
            // p[2] is paletteIndex (0 or 1 decimal)
            uint8_t palGroup = 0;
            try { palGroup = (uint8_t)std::stoi(p[2]); } catch(...) { palGroup = 0; }
            int x=0,y=0; try { x = std::stoi(p[3]); y = std::stoi(p[4]); } catch(...) { continue; }
            // Sprites always use high palette (1)
            if(isSpr) palGroup = 1;

            HdEntry entry{ imgIndex, (uint16_t)x, (uint16_t)y, isSpr, palGroup };

            // New format: 32-byte pattern hex; fallback: index hex
            if(tileField.size() >= 64) {
                std::array<uint8_t,32> pattern{};
                if(ParseHexToBytes(tileField, pattern)) {
                    uint64_t h = ComputeCanonicalHash(pattern.data(), isSpr, palGroup);
                    g_hdHashMap[h] = entry;
                    if(s_tileLogCount < 16) {
                        s_tileLogCount++;
                        HDLOG_TAG("MapTile", std::string("mode=hash img=") + std::to_string(imgIndex) +
                            " x=" + std::to_string(x) +
                            " y=" + std::to_string(y) +
                            " spr=" + (isSpr?"1":"0") +
                            " pal=" + std::to_string(palGroup) +
                            " key=0x" + Hex64_16(h)
                        );
                    }
                    continue;
                }
                // fallthrough to index if hex parse failed
            }

            uint32_t tileIndex = 0; 
            try { tileIndex = (uint32_t)std::stoul(tileField, nullptr, 16); } catch(...) { tileIndex = 0; }
            HdKey key{ tileIndex, palGroup, (uint8_t)(isSpr?1:0) };
            g_hdMap[key] = entry;
            if(s_tileLogCount < 16) {
                s_tileLogCount++;
                HDLOG_TAG("MapTile", std::string("mode=index img=") + std::to_string(imgIndex) +
                    " x=" + std::to_string(x) +
                    " y=" + std::to_string(y) +
                    " spr=" + (isSpr?"1":"0") +
                    " pal=" + std::to_string(palGroup) +
                    " idx=0x" + HexUtilities::ToHex((uint32_t)tileIndex, true)
                );
            }
        }

        g_hdPackLoaded = true;
        MessageManager::Log("[SMS HD Pack] Loaded HD pack: " + packFolder + 
            ", imgs=" + std::to_string(g_hdImages.size()) + 
            ", indexEntries=" + std::to_string(g_hdMap.size()) +
            ", hashEntries=" + std::to_string(g_hdHashMap.size()));

        // Debug: dump a small sample of hash entries
        int sample = 0;
        for(const auto& kv : g_hdHashMap) {
            if(sample++ >= 8) break;
            const HdEntry& e = kv.second;
            HDLOG_TAG("PackHash", std::string("key=0x") + Hex64_16(kv.first) +
                " img=" + std::to_string(e.ImgIndex) +
                " x=" + std::to_string(e.X) +
                " y=" + std::to_string(e.Y) +
                " spr=" + (e.IsSprite?"1":"0") +
                " pal=" + std::to_string(e.PaletteGroup)
            );
        }
    }

    void SetHdReplacementEnabled(bool enabled) { 
        g_hdReplacementEnabled = enabled; 
        MessageManager::Log(std::string("[SMS HD Pack] HD replacement ") + (enabled?"ENABLED":"DISABLED"));
    }
    bool IsHdReplacementEnabled() { return g_hdReplacementEnabled && g_hdPackLoaded; }
    bool IsHdPackLoaded() { return g_hdPackLoaded; }

    bool TryGetReplacementByIndex(uint32_t tileIndex, bool isSprite, uint8_t palGroup,
                                  const uint8_t* /*tileData32*/,
                                  int& imgIndex, uint16_t& srcX, uint16_t& srcY, uint32_t& scale)
    {
        if(!IsHdReplacementEnabled()) return false;
        HdKey key{ tileIndex, palGroup, (uint8_t)(isSprite?1:0) };
        auto it = g_hdMap.find(key);
        if(it == g_hdMap.end()) {
#ifdef SMS_HD_DEBUG
            static int s_indexMissLogs = 0;
            if(s_indexMissLogs < 40) {
                s_indexMissLogs++;
                HDLOG_TAG("IndexMiss", std::string("idx=0x") + HexUtilities::ToHex((uint32_t)tileIndex, true) +
                    " spr=" + (isSprite?"1":"0") +
                    " pal=" + std::to_string(palGroup)
                );
            }
#endif
            return false;
        }
        const HdEntry& e = it->second;
        imgIndex = e.ImgIndex; srcX = e.X; srcY = e.Y; scale = g_hdPackScale;
        return true;
    }

    bool TryGetReplacementByHash(const uint8_t* tileData32, bool isSprite, uint8_t palGroup,
                                 int& imgIndex, uint16_t& srcX, uint16_t& srcY, uint32_t& scale)
    {
        if(!IsHdReplacementEnabled()) return false;
        if(!tileData32) return false;
        uint64_t h = ComputeCanonicalHash(tileData32, isSprite, palGroup);
        
        // Debug: Log sprite hash lookups to see if they're all the same
        if(isSprite) {
            static std::map<uint64_t, int> spriteHashCounts;
            spriteHashCounts[h]++;
            if(spriteHashCounts[h] <= 3) { // Log first few occurrences
                std::stringstream ss;
                ss << "[SMS HD Pack] Sprite lookup hash: 0x" << std::hex << h 
                   << " count=" << std::dec << spriteHashCounts[h] << " pal=" << (int)palGroup;
                MessageManager::Log(ss.str());
            }
        }
        
        auto it = g_hdHashMap.find(h);
        if(it == g_hdHashMap.end()) {
#ifdef SMS_HD_DEBUG
            static int s_hashMissLogs = 0;
            if(s_hashMissLogs < 50) {
                s_hashMissLogs++;
                // Primary miss log
                HDLOG_TAG("HashMiss", std::string("key=0x") + Hex64_16(h) +
                    " spr=" + (isSprite?"1":"0") +
                    " pal=" + std::to_string(palGroup) +
                    " data=" + BytesToHex(tileData32, 32, 16)
                );
                // Diagnose likely mismatch causes by probing alt variants
                uint8_t altPal = palGroup ^ 1;
                uint64_t hPal = ComputeCanonicalHash(tileData32, isSprite, altPal);
                if(g_hdHashMap.find(hPal) != g_hdHashMap.end()) {
                    const HdEntry& eAlt = g_hdHashMap[hPal];
                    HDLOG_TAG("HintPal", std::string("altPal hit key=0x") + Hex64_16(hPal) +
                        " -> img=" + std::to_string(eAlt.ImgIndex) +
                        " x=" + std::to_string(eAlt.X) +
                        " y=" + std::to_string(eAlt.Y)
                    );
                }
                bool altSpr = !isSprite;
                uint64_t hSpr = ComputeCanonicalHash(tileData32, altSpr, palGroup);
                if(g_hdHashMap.find(hSpr) != g_hdHashMap.end()) {
                    const HdEntry& eAlt2 = g_hdHashMap[hSpr];
                    HDLOG_TAG("HintSpr", std::string("altSpr hit key=0x") + Hex64_16(hSpr) +
                        " -> img=" + std::to_string(eAlt2.ImgIndex) +
                        " x=" + std::to_string(eAlt2.X) +
                        " y=" + std::to_string(eAlt2.Y)
                    );
                }
            }
#endif
            return false;
        }
        const HdEntry& e = it->second;
        imgIndex = e.ImgIndex; srcX = e.X; srcY = e.Y; scale = g_hdPackScale;
        return true;
    }

    bool SampleReplacementRow8(int imgIndex, uint16_t srcX, uint16_t srcY, uint32_t scale,
                               uint8_t rowWithinTile, bool hMirror, uint16_t* outPixels8)
    {
        if(imgIndex < 0 || (size_t)imgIndex >= g_hdImages.size() || !outPixels8) {
            return false;
        }
        const HdImage& img = g_hdImages[(size_t)imgIndex];
        if(img.W == 0 || img.H == 0 || img.Pixels.empty()) {
            return false;
        }

        // Sample from the center of each scaled pixel block
        // Sanity: coordinates should align to tileSize (8*scale)
        uint32_t tileSize = 8u * (scale ? scale : 1u);
        static int s_alignLogs = 0;
        if(s_alignLogs < 20) {
            if(((uint32_t)srcX % tileSize) != 0 || ((uint32_t)srcY % tileSize) != 0) {
                s_alignLogs++;
                HDLOG_TAG("CoordMisalign", std::string("img=") + std::to_string(imgIndex) +
                    " srcX=" + std::to_string(srcX) +
                    " srcY=" + std::to_string(srcY) +
                    " scale=" + std::to_string(scale) +
                    " tileSize=" + std::to_string(tileSize)
                );
            }
        }

        uint32_t yPix = (uint32_t)srcY + (uint32_t)rowWithinTile * scale + (scale ? (scale / 2) : 0);
        bool yClamped = false;
        if(yPix >= img.H) { yPix = img.H - 1; yClamped = true; }

        for(int i = 0; i < 8; i++) {
            int col = hMirror ? (7 - i) : i;
            uint32_t xPix = (uint32_t)srcX + (uint32_t)col * scale + (scale ? (scale / 2) : 0);
            bool xClamped = false;
            if(xPix >= img.W) { xPix = img.W - 1; xClamped = true; }
            uint32_t pixel = img.Pixels[yPix * img.W + xPix]; // ARGB
            uint8_t a8 = (uint8_t)((pixel >> 24) & 0xFF);
            uint8_t r8 = (uint8_t)((pixel >> 16) & 0xFF);
            uint8_t g8 = (uint8_t)((pixel >> 8) & 0xFF);
            uint8_t b8 = (uint8_t)(pixel & 0xFF);
            uint16_t r5 = (uint16_t)(r8 >> 3);
            uint16_t g5 = (uint16_t)(g8 >> 3);
            uint16_t b5 = (uint16_t)(b8 >> 3);
            uint16_t rgb555 = (uint16_t)((r5 << 10) | (g5 << 5) | b5);
            // Use bit 15 as a transparency flag: 1 = transparent (alpha 0), 0 = opaque
            outPixels8[i] = (a8 < 128) ? (uint16_t)(0x8000 | rgb555) : rgb555;

            if((xClamped || yClamped) && s_alignLogs < 40) {
                s_alignLogs++;
                HDLOG_TAG("SampleClamp", std::string("img=") + std::to_string(imgIndex) +
                    " srcX=" + std::to_string(srcX) +
                    " srcY=" + std::to_string(srcY) +
                    " scale=" + std::to_string(scale) +
                    " w=" + std::to_string(img.W) +
                    " h=" + std::to_string(img.H) +
                    " yPix=" + std::to_string(yPix) +
                    " xPix=" + std::to_string(xPix)
                );
            }
        }
        return true;
    }

    bool GetImageData(int imgIndex, const uint32_t*& outPixels, uint32_t& outWidth, uint32_t& outHeight)
    {
        outPixels = nullptr;
        outWidth = 0;
        outHeight = 0;
        
        if(imgIndex < 0 || imgIndex >= (int)g_hdImages.size()) {
            return false;
        }
        
        const HdImage& img = g_hdImages[imgIndex];
        if(img.Pixels.empty() || img.W == 0 || img.H == 0) {
            return false;
        }
        
        outPixels = img.Pixels.data();
        outWidth = img.W;
        outHeight = img.H;
        return true;
    }

    uint32_t GetHdPackScale()
    {
        return g_hdPackScale;
    }

    HdPackDataSms* GetHdPackData()
    {
        // Currently not using HdPackDataSms - API uses static variables instead
        return nullptr;
    }

    // Helper function to sanitize ROM names for filesystem use
    string SanitizeRomName(const string& romName) {
        string clean = romName;
        
        // Replace problematic characters with safe alternatives
        for(char& c : clean) {
            if(c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
                c = '_';
            } else if(c == ',' || c == ':' || c == ';') {
                c = '_';
            } else if(c == '"' || c == '\'' || c == '`') {
                c = '_';
            } else if(c == '<' || c == '>' || c == '|' || c == '?' || c == '*') {
                c = '_';
            } else if(c == '/' || c == '\\') {
                c = '_';
            } else if(c == ' ') {
                c = '_';
            }
        }
        
        // Limit length to avoid path too long errors
        if(clean.length() > 80) {
            clean = clean.substr(0, 80);
        }
        
        // Remove trailing dots, spaces, and underscores
        while(!clean.empty() && (clean.back() == '.' || clean.back() == ' ' || clean.back() == '_')) {
            clean.pop_back();
        }
        
        // Ensure it's not empty
        if(clean.empty()) {
            clean = "Unknown_SMS_Game";
        }
        
        return clean;
    }

    // Auto-start hook when an SMS game is loaded (no longer auto-starts dumping; waits for explicit Start Recording)
    void AutoStartOnGameLoaded(Emulator* emu) {
        if(emu->GetConsoleType() != ConsoleType::Sms) {
            return;
        }
        
        // Build identifier and debug state
        MessageManager::Log(std::string("[SMS HD Pack] Build: ") + __DATE__ + " " + __TIME__);
#ifdef SMS_HD_DEBUG
        MessageManager::Log("[SMS HD Pack] Debug logging: ON");
#else
        MessageManager::Log("[SMS HD Pack] Debug logging: OFF");
#endif

        // Sync dumper enable flag from settings, but do not start recording automatically
        SmsConfig& smsConfig = emu->GetSettings()->GetSmsConfig();
        SmsHdTileDumper::SetDumpingEnabled(smsConfig.EnableHdTileDumping);
        
        // Ensure we're not considered dumping until the user explicitly starts it
        g_isDumping = false;
        
        MessageManager::Log("[SMS HD Pack] Ready. Use Start Recording to begin tile dumping.");
    }

    // Save and stop dumping when the game is powered off
    void SaveOnPowerOff(Emulator* emu) {
        if(!g_isDumping) {
            return;
        }
        
        if(g_hdPackBuilder) {
            MessageManager::Log("[SMS HD Pack] Saving tiles on power off...");
            g_hdPackBuilder->SaveHdPack();
            g_hdPackBuilder.reset();
            MessageManager::Log("[SMS HD Pack] Tiles saved successfully.");
        }
        
        if(g_hdTileDumper) {
            MessageManager::Log("[SMS HD Tile] Saving manifest on power off...");
            g_hdTileDumper->SaveManifest();
            g_hdTileDumper.reset();
            MessageManager::Log("[SMS HD Tile] Manifest saved successfully.");
        }
        
        g_isDumping = false;
    }

    // Explicit start recording entry point (called from UI shortcut)
    void StartRecording(Emulator* emu, HdPackBuilderOptions options) {
        if(emu->GetConsoleType() != ConsoleType::Sms) {
            return;
        }
        
        // Use NES-style processing pipeline for stable tile capture
        options.UseNesStylePipeline = true;

        // Create builder if needed
        if(!g_hdPackBuilder) {
            auto console = emu->GetConsole();
            SmsConsole* smsConsole = static_cast<SmsConsole*>(console.get());
            g_hdPackBuilder = std::make_unique<HdPackBuilderSms>(emu, smsConsole, options);
        }
        
        // Initialize tile dumper if enabled and not created yet
        if(SmsHdTileDumper::IsDumpingEnabled() && !g_hdTileDumper) {
            RomInfo romInfo = emu->GetRomInfo();
            g_hdTileDumper = std::make_unique<SmsHdTileDumper>(emu, romInfo.RomFile.GetSha1Hash());
            g_hdTileDumper->Initialize(options.SaveFolder);
        }
        
        // Ensure builder is actively recording
        if(!g_hdPackBuilder->IsRecording()) {
            g_hdPackBuilder->StartRecording();
        }
        
        // Enable VDP capture
        SmsConsole* smsConsole = static_cast<SmsConsole*>(emu->GetConsole().get());
        if(HdBuilderSmsVdp* hdVdp = smsConsole->GetHdBuilderSmsVdp()) {
            hdVdp->SetHdCaptureEnabled(true);
            MessageManager::Log("[SMS HD Pack] VDP capture enabled");
        }
        
        g_isDumping = true;
        g_lastProcessedFrame = nullptr;
        
        MessageManager::Log("[SMS HD Pack] Recording started. Saving to: " + options.SaveFolder);
        HDLOG_TAG("Session", "Recording started: folder=" + options.SaveFolder);
    }

    // Explicit stop recording entry point (called from UI shortcut)
    void StopRecording(Emulator* emu) {
        if(!g_isDumping) {
            return;
        }
        
        // Disable VDP capture
        if(emu && emu->GetConsoleType() == ConsoleType::Sms) {
            SmsConsole* smsConsole = static_cast<SmsConsole*>(emu->GetConsole().get());
            if(HdBuilderSmsVdp* hdVdp = smsConsole->GetHdBuilderSmsVdp()) {
                hdVdp->SetHdCaptureEnabled(false);
                MessageManager::Log("[SMS HD Pack] VDP capture disabled");
            }
        }
        
        if(g_hdPackBuilder) {
            g_hdPackBuilder->StopRecording();
            g_hdPackBuilder.reset();
        }
        if(g_hdTileDumper) {
            g_hdTileDumper->SaveManifest();
            g_hdTileDumper.reset();
        }
        
        g_isDumping = false;
        g_lastProcessedFrame = nullptr;
        MessageManager::Log("[SMS HD Pack] Recording stopped.");
    }

    // Function to be called from SMS VDP during tile rendering
    void ProcessFrameIfReady(Emulator* emu)
    {
        if(!g_isDumping || !emu) {
            return;
        }

        auto consolePtr = emu->GetConsole();
        if(!consolePtr) {
            return;
        }

        SmsConsole* smsConsole = static_cast<SmsConsole*>(consolePtr.get());
        if(!smsConsole) {
            return;
        }

        HdBuilderSmsVdp* hdVdp = smsConsole->GetHdBuilderSmsVdp();
        if(!hdVdp || !hdVdp->IsHdCaptureEnabled()) {
            return;
        }

        HdScreenInfoSms* readyFrame = hdVdp->SwapBuffersOnFrameEnd();
        if(!readyFrame || readyFrame == g_lastProcessedFrame) {
            return;
        }

        g_lastProcessedFrame = readyFrame;

        if(g_hdPackBuilder) {
            g_hdPackBuilder->ProcessFrame(readyFrame);
        }

    }

    // Check if currently dumping
    bool IsCurrentlyDumping() {
        return g_isDumping;
    }
    
    // Enable/disable HD tile dumping
    void SetHdTileDumpingEnabled(bool enabled) {
        SmsHdTileDumper::SetDumpingEnabled(enabled);
    }
    
    // Check if HD tile dumping is enabled
    bool IsHdTileDumpingEnabled() {
        return SmsHdTileDumper::IsDumpingEnabled();
    }
}