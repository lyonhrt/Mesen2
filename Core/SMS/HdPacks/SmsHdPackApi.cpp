#include "pch.h"
#include "SMS/HdPacks/SmsHdPackApi.h"
#include "SMS/HdPacks/HdPackBuilderSms.h"
#include "SMS/HdPacks/SmsHdTileDumper.h"
#include "SMS/SmsConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include <memory>
#include "SMS/HdPacks/HdPackDebug.h"
// Loader dependencies
#include <array>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Utilities/PNGHelper.h"

namespace SmsHdPackApi {
    // Global state for HD pack dumping
    static std::unique_ptr<HdPackBuilderSms> g_hdPackBuilder;
    static std::unique_ptr<SmsHdTileDumper> g_hdTileDumper;
    static bool g_isDumping = false;
    static uint64_t g_bgSeen = 0;
    static uint64_t g_sprSeen = 0;

    // ---------------- HD Replacement Loader (scaffolding) ----------------
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

    static inline uint8_t NormalizePaletteGroup(uint32_t paletteColors) {
        // Mirror HdTileKeySms::NormalizePaletteIndex logic: BG low=0 / high!=0 => 1; Sprites always 1
        return ((paletteColors >> 16) & 0xFF) ? 1 : 0;
    }

    static bool ParseManifest(const std::string& manifestPath, std::vector<std::string>& imgs, ParsedTiles& tiles, uint32_t& scale)
    {
        imgs.clear(); tiles.rows.clear(); tiles.isSprite.clear(); scale = 1;
        std::ifstream in(manifestPath);
        if(!in.is_open()) return false;
        std::string line; bool currentIsSprite = false; while(std::getline(in, line)) {
            std::string l = line; // trim
            auto trim=[&](std::string& s){ size_t a=s.find_first_not_of(" \t"); if(a==std::string::npos){s.clear();return;} size_t b=s.find_last_not_of(" \t\r\n"); s=s.substr(a,b-a+1);};
            trim(l); if(l.empty()) continue;
            if(l.rfind("<scale>",0)==0) {
                std::string rest = l.substr(7); size_t lt=rest.find('<'); if(lt!=std::string::npos) rest=rest.substr(0,lt); trim(rest);
                try{ int v=std::stoi(rest); if(v>=1&&v<=10) scale=(uint32_t)v; }catch(...){ }
            } else if(l.rfind("<img>",0)==0) {
                std::string fn = l.substr(5); trim(fn); imgs.push_back(fn);
            } else if(l.rfind("#",0)==0) {
                // Section header: use filename heuristic to set sprite/background flag
                std::string fname = l.substr(1); trim(fname);
                currentIsSprite = (fname.find("SPRITES_") == 0);
            } else if(l.rfind("<tile>",0)==0) {
                std::string rest = l.substr(6); std::array<std::string,7> parts{}; int idx=0; std::stringstream ss(rest); std::string item;
                while(std::getline(ss,item,',') && idx<7){ trim(item); parts[idx++]=item; }
                if(idx==7){ tiles.rows.push_back(parts); tiles.isSprite.push_back(currentIsSprite);}            
            }
        }
        return true;
    }

    void LoadHdPackIfAvailable(Emulator* emu)
    {
        g_hdPackLoaded = false; g_hdImages.clear(); g_hdMap.clear(); g_hdPackScale = 1;
        if(!emu || emu->GetConsoleType()!=ConsoleType::Sms) return;
        std::string packFolder = FolderUtilities::CombinePath(FolderUtilities::GetHdPackFolder(), FolderUtilities::GetFilename(emu->GetRomInfo().RomFile.GetFileName(), false));
        std::string manifestPath = FolderUtilities::CombinePath(packFolder, "hires.txt");
        std::error_code ec; if(!std::filesystem::exists(std::filesystem::u8path(manifestPath), ec)) return;

        std::vector<std::string> imgs; ParsedTiles tiles; uint32_t scale=1;
        if(!ParseManifest(manifestPath, imgs, tiles, scale)) return;
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
        for(size_t i=0;i<tiles.rows.size();i++){
            const auto& p = tiles.rows[i]; bool isSpr = tiles.isSprite[i];
            int imgIndex = -1; try { imgIndex = std::stoi(p[0]); } catch(...) { continue; }
            if(imgIndex < 0 || imgIndex >= (int)g_hdImages.size()) continue;
            // tileDataHex is SMS VRAM tile index (we wrote this in the generator)
            uint32_t tileIndex = 0; try { tileIndex = (uint32_t)std::stoul(p[1], nullptr, 16); } catch(...) { continue; }
            uint32_t paletteHex = 0; try { paletteHex = (uint32_t)std::stoul(p[2], nullptr, 16); } catch(...) { paletteHex = 0; }
            int x=0,y=0; try { x = std::stoi(p[3]); y = std::stoi(p[4]); } catch(...) { continue; }
            uint8_t palGroup = isSpr ? 1 : NormalizePaletteGroup(paletteHex);

            HdKey key{ tileIndex, palGroup, (uint8_t)(isSpr?1:0) };
            g_hdMap[key] = HdEntry{ imgIndex, (uint16_t)x, (uint16_t)y, isSpr, palGroup };
        }

        g_hdPackLoaded = true;
        MessageManager::Log("[SMS HD Pack] Loaded HD pack: " + packFolder + ", imgs=" + std::to_string(g_hdImages.size()) + ", entries=" + std::to_string(g_hdMap.size()));
    }

    void SetHdReplacementEnabled(bool enabled) { g_hdReplacementEnabled = enabled; }
    bool IsHdReplacementEnabled() { return g_hdReplacementEnabled && g_hdPackLoaded; }

    bool TryGetReplacementByIndex(uint32_t tileIndex, bool isSprite, uint8_t palGroup,
                                  const uint8_t* /*tileData32*/,
                                  int& imgIndex, uint16_t& srcX, uint16_t& srcY, uint32_t& scale)
    {
        if(!IsHdReplacementEnabled()) return false;
        HdKey key{ tileIndex, palGroup, (uint8_t)(isSprite?1:0) };
        auto it = g_hdMap.find(key);
        if(it == g_hdMap.end()) return false;
        const HdEntry& e = it->second;
        imgIndex = e.ImgIndex; srcX = e.X; srcY = e.Y; scale = g_hdPackScale;
        return true;
    }

    bool TryGetReplacementByHash(const uint8_t* /*tileData32*/, bool /*isSprite*/, uint8_t /*palGroup*/,
                                 int& /*imgIndex*/, uint16_t& /*srcX*/, uint16_t& /*srcY*/, uint32_t& /*scale*/)
    {
        // Placeholder: hash-based lookup requires manifest entries keyed by pattern data.
        // For now, not implemented since current SMS manifest uses tile index.
        return false;
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
        uint32_t yPix = (uint32_t)srcY + (uint32_t)rowWithinTile * scale + (scale ? (scale / 2) : 0);
        if(yPix >= img.H) yPix = img.H - 1;

        for(int i = 0; i < 8; i++) {
            int col = hMirror ? (7 - i) : i;
            uint32_t xPix = (uint32_t)srcX + (uint32_t)col * scale + (scale ? (scale / 2) : 0);
            if(xPix >= img.W) xPix = img.W - 1;
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
        }
        return true;
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
        
        HDLOG_TAG("Session", "Power off: saving. BG seen=" + std::to_string(g_bgSeen) + ", SPR seen=" + std::to_string(g_sprSeen));

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
        g_bgSeen = g_sprSeen = 0;
    }

    // Explicit start recording entry point (called from UI shortcut)
    void StartRecording(Emulator* emu, HdPackBuilderOptions options) {
        if(emu->GetConsoleType() != ConsoleType::Sms) {
            return;
        }
        
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
        
        g_isDumping = true;
        g_bgSeen = g_sprSeen = 0;
        
        MessageManager::Log("[SMS HD Pack] Recording started. Saving to: " + options.SaveFolder);
        HDLOG_TAG("Session", "Recording started: folder=" + options.SaveFolder);
    }

    // Explicit stop recording entry point (called from UI shortcut)
    void StopRecording(Emulator* emu) {
        if(!g_isDumping) {
            return;
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
        HDLOG_TAG("Counters", "BG tiles seen=" + std::to_string(g_bgSeen) + ", SPR tiles seen=" + std::to_string(g_sprSeen));
        g_bgSeen = g_sprSeen = 0;
        MessageManager::Log("[SMS HD Pack] Recording stopped.");
    }

    // Function to be called from SMS VDP during tile rendering
    void ProcessSmsBackgroundTile(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr, 
                                 uint8_t* tileData, uint32_t paletteColors, bool hMirror, bool vMirror, bool priority) {
        
        if(!g_isDumping) {
            return;
        }
        
        // Process with HD pack builder if available
        if(g_hdPackBuilder) {
            // Create tile key from the SMS tile data
            HdTileKeySms tileKey = {};
            tileKey.TileIndex = tileAddr / 32;
            tileKey.IsVramTile = true;
            tileKey.IsSprite = false;
            tileKey.PaletteColors = paletteColors;
            
            // Copy the 32-byte SMS tile data
            memcpy(tileKey.TileData, tileData, 32);
            
            // Process the tile
            uint32_t bankHash = tileAddr / 0x1000;
            g_bgSeen++;
            g_hdPackBuilder->ProcessTile(x, y, tileAddr, tileKey, false, bankHash, false);
        }
        
        // HD replacement lookup (no visual replacement yet - diagnostic only)
        if(IsHdReplacementEnabled()) {
            int imgIdx; uint16_t srcX, srcY; uint32_t sc;
            uint8_t palGroup = NormalizePaletteGroup(paletteColors);
            if(TryGetReplacementByIndex(tileAddr/32, false, palGroup, tileData, imgIdx, srcX, srcY, sc)) {
                HDLOG_TAG("Replace", "BG replacement found: img=" + std::to_string(imgIdx) + 
                    " x=" + std::to_string(srcX) + " y=" + std::to_string(srcY) + " scale=" + std::to_string(sc));
            }
        }

        // Process with HD tile dumper if available
        if(g_hdTileDumper && SmsHdTileDumper::IsDumpingEnabled()) {
            // Extract palette index from paletteColors
            int paletteIndex = (paletteColors >> 16) & 0xFF;
            
            // Dump the tile
            g_hdTileDumper->DumpTile(tileData, paletteIndex, hMirror, vMirror, x, y, false);
        }
    }

    // Function to be called from SMS VDP during sprite rendering  
    void ProcessSmsSprite(Emulator* emu, uint32_t x, uint32_t y, uint32_t tileAddr,
                         uint8_t* tileData, uint32_t paletteColors) {
        if(!g_isDumping) {
            return;
        }
        
        // Process with HD pack builder if available
        if(g_hdPackBuilder) {
            // Create tile key from the SMS sprite data
            HdTileKeySms tileKey = {};
            tileKey.TileIndex = tileAddr / 32;
            tileKey.IsVramTile = true;
            tileKey.IsSprite = true;
            tileKey.PaletteColors = paletteColors;
            
            // Copy the 32-byte SMS tile data
            memcpy(tileKey.TileData, tileData, 32);
            
            // Process as sprite tile
            uint32_t bankHash = tileAddr / 0x1000;
            g_sprSeen++;
            g_hdPackBuilder->ProcessTile(x, y, tileAddr, tileKey, true, bankHash, false);
        }
        
        // Process with HD tile dumper if available
        if(g_hdTileDumper && SmsHdTileDumper::IsDumpingEnabled()) {
            // SMS sprites always use high palette (0x10-0x1F)
            int paletteIndex = 1; // Sprite palette index (1 = high palette)
            
            // Dump the tile
            g_hdTileDumper->DumpTile(tileData, paletteIndex, false, false, x, y, true);
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