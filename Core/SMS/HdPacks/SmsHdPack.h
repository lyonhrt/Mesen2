#pragma once
#include "pch.h"
#include "SMS/HdPacks/HdDataSms.h"
#include "SMS/HdPacks/SmsHdPackApi.h"
#include "SMS/SmsConsole.h"
#include "SMS/SmsVdp.h"
#include "Shared/EmuSettings.h"
#include "Shared/MessageManager.h"
#include <algorithm>

// SMS screen constants
static constexpr uint32_t SmsScreenWidth = 256;
static constexpr uint32_t SmsScreenHeight = 240;

// Base class for SMS HD pack rendering (non-templated interface)
class BaseSmsHdPack
{
protected:
	HdScreenInfoSms* _hdScreenInfo = nullptr;

public:
	virtual uint32_t GetScale() = 0;
	HdScreenInfoSms* GetScreenInfo() { return _hdScreenInfo; }
	virtual void Process(HdScreenInfoSms* hdScreenInfo, uint32_t* outputBuffer, OverscanDimensions& overscan) = 0;
	virtual ~BaseSmsHdPack() {}
};

// Templated SMS HD pack renderer - scale is compile-time constant for performance
// Follows NES HdNesPack pattern for consistency
template<uint32_t scale>
class SmsHdPack final : public BaseSmsHdPack
{
private:
	SmsConsole* _console = nullptr;
	EmuSettings* _settings = nullptr;
	HdPackDataSms* _hdData = nullptr;

	// SMS palette converted to ARGB (32 colors: 16 BG + 16 sprite)
	uint32_t _palette[32] = {};

	// Tile lookup cache
	HdPackTileInfoSms* _cachedTile = nullptr;
	bool _cacheEnabled = true;
	bool _useCachedTile = false;
	
	// API-based tile lookup cache (for VDP pixel info path)
	// Cache the last tile lookup result to avoid repeated hash lookups
	// Cache is invalidated at tile boundaries (TileX == 0) for smoother scrolling
	struct CachedApiTile {
		int ImgIndex = -1;
		uint16_t SrcX = 0;
		uint16_t SrcY = 0;
		uint32_t HdScale = 1;
		bool Valid = false;
	};
	CachedApiTile _cachedBgTile;
	CachedApiTile _cachedSpriteTile;
	uint8_t _prevBgTileX = 0xFF;
	int16_t _prevBgTileStart = std::numeric_limits<int16_t>::min();
	uint8_t _cachedBgTileData[32] = {};
	uint8_t _cachedBgPaletteIndex = 0;
	uint32_t _cachedBgPaletteColors = 0;
	bool _cachedBgHMirror = false;
	bool _cachedBgVMirror = false;

	__forceinline void InvalidateBgCache()
	{
		_useCachedTile = false;
		_cachedBgTile.Valid = false;
		_prevBgTileX = 0xFF;
		_prevBgTileStart = std::numeric_limits<int16_t>::min();
	}

	// Brightness adjustment
	__forceinline uint32_t AdjustBrightness(uint8_t input[4], int brightness)
	{
		return (
			std::min(255, (brightness * ((int)input[0] + 1)) >> 8) |
			(std::min(255, (brightness * ((int)input[1] + 1)) >> 8) << 8) |
			(std::min(255, (brightness * ((int)input[2] + 1)) >> 8) << 16) |
			(input[3] << 24)
		);
	}

	// Draw a solid color at scale
	__forceinline void DrawColor(uint32_t color, uint32_t* outputBuffer, uint32_t screenWidth)
	{
		if constexpr(scale == 1) {
			*outputBuffer = color;
		} else if constexpr(scale == 2) {
			outputBuffer[0] = color;
			outputBuffer[1] = color;
			outputBuffer[screenWidth] = color;
			outputBuffer[screenWidth + 1] = color;
		} else {
			for(uint32_t i = 0; i < scale; i++) {
				std::fill(outputBuffer, outputBuffer + scale, color);
				outputBuffer += screenWidth;
			}
		}
	}

	// Draw HD tile pixels for a single source pixel position (pixelX, pixelY within 0-7)
	// This draws the scale x scale block of HD pixels corresponding to one original pixel
	__forceinline void DrawTilePixel(HdSmsTileInfo& tileInfo, HdPackTileInfoSms& hdPackTileInfo, 
	                                  uint32_t pixelX, uint32_t pixelY,
	                                  uint32_t* outputBuffer, uint32_t screenWidth)
	{
		if(hdPackTileInfo.Blank && hdPackTileInfo.TransparencyRequired) {
			return;
		}

		uint32_t* bitmapData = hdPackTileInfo.HdTileData.data();
		if(!bitmapData || hdPackTileInfo.HdTileData.empty()) {
			return;
		}

		uint32_t tileWidth = 8 * scale;
		
		// Apply mirroring to get the actual pixel position in the HD tile
		uint32_t srcX = pixelX;
		uint32_t srcY = pixelY;
		if(tileInfo.HorizontalMirroring) {
			srcX = 7 - pixelX;
		}
		if(tileInfo.VerticalMirroring) {
			srcY = 7 - pixelY;
		}

		bool hasTransparency = hdPackTileInfo.TransparencyRequired;
		int brightness = hdPackTileInfo.Brightness;

		// Draw the scale x scale block for this pixel
		for(uint32_t dy = 0; dy < scale; dy++) {
			for(uint32_t dx = 0; dx < scale; dx++) {
				uint32_t hdX = srcX * scale + dx;
				uint32_t hdY = srcY * scale + dy;
				uint32_t bitmapOffset = hdY * tileWidth + hdX;
				
				if(bitmapOffset >= hdPackTileInfo.HdTileData.size()) {
					continue;
				}

				uint32_t rgbValue;
				if(brightness == 255) {
					rgbValue = bitmapData[bitmapOffset];
				} else {
					rgbValue = AdjustBrightness((uint8_t*)(bitmapData + bitmapOffset), brightness);
				}

				uint32_t* outPtr = outputBuffer + dy * screenWidth + dx;
				
				if(!hasTransparency || (bitmapData[bitmapOffset] & 0xFF000000) == 0xFF000000) {
					*outPtr = rgbValue;
				} else {
					if(bitmapData[bitmapOffset] & 0xFF000000) {
						uint8_t* out = (uint8_t*)outPtr;
						uint8_t* in = (uint8_t*)&rgbValue;
						uint8_t alpha = in[3] + 1;
						uint8_t invAlpha = 256 - in[3];
						out[0] = (uint8_t)((alpha * in[0] + invAlpha * out[0]) >> 8);
						out[1] = (uint8_t)((alpha * in[1] + invAlpha * out[1]) >> 8);
						out[2] = (uint8_t)((alpha * in[2] + invAlpha * out[2]) >> 8);
						out[3] = 0xFF;
					}
				}
			}
		}
	}

	// Find matching HD tile for replacement using VDP's HdPixelInfo
	__forceinline HdPackTileInfoSms* GetMatchingTileFromVdpPixel(uint32_t x, uint32_t y, const SmsVdp::HdPixelInfo* pixel)
	{
		if(!pixel || !pixel->HasTileData || !_hdData) {
			return nullptr;
		}

		HdTileKeySms key;
		key.TileIndex = -1; // Use tile data for matching, not index
		key.PaletteColors = pixel->PaletteColors;
		key.PaletteIndex = pixel->PaletteIndex;  // Must match hash function
		key.IsVramTile = true;
		key.IsSprite = pixel->IsSprite;
		key.IsSg1000Mode = pixel->IsSg1000Mode;  // Must match hash function
		memcpy(key.TileData, pixel->TileData, sizeof(key.TileData));

		auto hdTile = _hdData->TileByKey.find(key);
		if(hdTile == _hdData->TileByKey.end()) {
			key.PaletteColors = 0xFFFFFFFF;
			hdTile = _hdData->TileByKey.find(key);
		}

		if(hdTile != _hdData->TileByKey.end()) {
			for(HdPackTileInfoSms* hdPackTile : hdTile->second) {
				bool conditionsMet = true;
				for(HdPackConditionSms* condition : hdPackTile->Conditions) {
					if(condition && !condition->CheckCondition(x, y, nullptr)) {
						conditionsMet = false;
						break;
					}
				}

				if(conditionsMet) {
					if(hdPackTile->HdTileData.empty() && hdPackTile->Bitmap) {
						hdPackTile->Bitmap->Init();
						uint32_t bitmapOffset = hdPackTile->Y * hdPackTile->Bitmap->Width + hdPackTile->X;
						uint32_t* pngData = hdPackTile->Bitmap->RgbData;
						if(pngData) {
							hdPackTile->HdTileData.resize(hdPackTile->Width * hdPackTile->Height);
							for(uint32_t row = 0; row < hdPackTile->Height; row++) {
								memcpy(hdPackTile->HdTileData.data() + (row * hdPackTile->Width),
									   pngData + bitmapOffset,
									   hdPackTile->Width * sizeof(uint32_t));
								bitmapOffset += hdPackTile->Bitmap->Width;
							}
							hdPackTile->UpdateFlags();
						}
					}
					return hdPackTile;
				}
			}
		}
		return nullptr;
	}

	// Find matching HD tile for replacement
	__forceinline HdPackTileInfoSms* GetMatchingTile(uint32_t x, uint32_t y, HdSmsTileInfo* tile)
	{
		if(!tile || tile->TileIndex < 0 || !_hdData) {
			return nullptr;
		}

		HdTileKeySms key;
		key.TileIndex = tile->TileIndex;
		key.PaletteColors = tile->PaletteColors;
		key.PaletteIndex = tile->PaletteIndex;  // Must match hash function
		key.IsVramTile = tile->IsVramTile;
		key.IsSprite = tile->IsSprite;
		key.IsSg1000Mode = tile->IsSg1000Mode;  // Must match hash function
		memcpy(key.TileData, tile->TileData, sizeof(key.TileData));

		auto hdTile = _hdData->TileByKey.find(key);
		if(hdTile == _hdData->TileByKey.end()) {
			key.PaletteColors = 0xFFFFFFFF;
			hdTile = _hdData->TileByKey.find(key);
		}

		if(hdTile != _hdData->TileByKey.end()) {
			for(HdPackTileInfoSms* hdPackTile : hdTile->second) {
				bool conditionsMet = true;
				for(HdPackConditionSms* condition : hdPackTile->Conditions) {
					if(condition && !condition->CheckCondition(x, y, nullptr)) {
						conditionsMet = false;
						break;
					}
				}

				if(conditionsMet) {
					if(hdPackTile->HdTileData.empty() && hdPackTile->Bitmap) {
						hdPackTile->Bitmap->Init();
						uint32_t bitmapOffset = hdPackTile->Y * hdPackTile->Bitmap->Width + hdPackTile->X;
						uint32_t* pngData = hdPackTile->Bitmap->RgbData;
						if(pngData) {
							hdPackTile->HdTileData.resize(hdPackTile->Width * hdPackTile->Height);
							for(uint32_t row = 0; row < hdPackTile->Height; row++) {
								memcpy(hdPackTile->HdTileData.data() + (row * hdPackTile->Width),
									   pngData + bitmapOffset,
									   hdPackTile->Width * sizeof(uint32_t));
								bitmapOffset += hdPackTile->Bitmap->Width;
							}
							hdPackTile->UpdateFlags();
						}
					}
					return hdPackTile;
				}
			}
		}
		return nullptr;
	}

	__forceinline HdPackTileInfoSms* GetCachedMatchingTile(uint32_t x, uint32_t y, HdSmsTileInfo* tile)
	{
		if((x & 0x07) == 0) {
			_useCachedTile = false;
		}

		if(_useCachedTile && _cacheEnabled) {
			return _cachedTile;
		}

		HdPackTileInfoSms* hdPackTileInfo = GetMatchingTile(x, y, tile);

		if(_cacheEnabled) {
			_cachedTile = hdPackTileInfo;
			_useCachedTile = true;
		}

		return hdPackTileInfo;
	}

	// Process a single pixel using VDP's HdPixelInfo - x,y are screen coordinates
	// Uses SmsHdPackApi for tile lookup (hash-based matching)
	// NES parity: Renders BG first, then sprite on top (layered rendering)
	__forceinline void GetPixelsFromVdp(uint32_t x, uint32_t y, const SmsVdp::HdPixelInfo* pixelInfo, uint32_t* outputBuffer, uint32_t screenWidth)
	{
		// Draw background color first (palette index 0)
		uint32_t bgColor = _palette[0];
		DrawColor(bgColor, outputBuffer, screenWidth);

		if(!pixelInfo) {
			InvalidateBgCache();
			return;
		}

		// Step 1: Draw BG tile (if present)
		if(pixelInfo->Bg.HasTileData) {
			// SG-1000 tiles always use palGroup=0 (no palette banks in TMS9918).
			// PaletteColors for SG-1000 is a color hash, not a palette index.
			// SMS/GG tiles use PaletteIndex (0 = low/BG, 1 = high/sprite).
			uint8_t bgPalGroup = pixelInfo->Bg.IsSg1000Mode ? 0 : pixelInfo->Bg.PaletteIndex;
				
				int imgIndex = -1;
				uint16_t srcX = 0, srcY = 0;
				uint32_t hdScale = 1;
				
				// Detect tile boundary by monitoring TileX wrap (handles fine scroll changes)
				if(pixelInfo->Bg.TileX <= _prevBgTileX) {
					_useCachedTile = false;
				}
				_prevBgTileX = pixelInfo->Bg.TileX;

				int16_t tileStart = static_cast<int16_t>(x) - static_cast<int16_t>(pixelInfo->Bg.TileX);
				if(tileStart != _prevBgTileStart) {
					_useCachedTile = false;
					_prevBgTileStart = tileStart;
				}
				
				bool sameTile = _cachedBgTile.Valid &&
					memcmp(_cachedBgTileData, pixelInfo->Bg.TileData, sizeof(_cachedBgTileData)) == 0 &&
					_cachedBgPaletteIndex == bgPalGroup &&
					_cachedBgPaletteColors == pixelInfo->Bg.PaletteColors &&
					_cachedBgHMirror == pixelInfo->Bg.HMirror &&
					_cachedBgVMirror == pixelInfo->Bg.VMirror;
				if(!sameTile) {
					_useCachedTile = false;
				}
				
				// Use cached tile if valid (same tile, just different pixel within it)
				if(_useCachedTile && _cachedBgTile.Valid) {
					imgIndex = _cachedBgTile.ImgIndex;
					srcX = _cachedBgTile.SrcX;
					srcY = _cachedBgTile.SrcY;
					hdScale = _cachedBgTile.HdScale;
				} else {
					// Look up HD replacement for this tile
					if(SmsHdPackApi::TryGetReplacementByHash(pixelInfo->Bg.TileData, false, bgPalGroup,
					                                          imgIndex, srcX, srcY, hdScale,
					                                          pixelInfo->Bg.PaletteColors, pixelInfo->Bg.IsSg1000Mode)) {
						// Cache the result for subsequent pixels of the same tile
						_cachedBgTile.ImgIndex = imgIndex;
						_cachedBgTile.SrcX = srcX;
						_cachedBgTile.SrcY = srcY;
						_cachedBgTile.HdScale = hdScale;
						_cachedBgTile.Valid = true;
						_useCachedTile = true;
					} else {
						// No HD replacement - mark cache as valid but with no replacement
						_cachedBgTile.ImgIndex = -1;
						_cachedBgTile.Valid = true;
						_useCachedTile = true;
					}
					// Store tile signature for future comparisons
					memcpy(_cachedBgTileData, pixelInfo->Bg.TileData, sizeof(_cachedBgTileData));
					_cachedBgPaletteIndex = bgPalGroup;
					_cachedBgPaletteColors = pixelInfo->Bg.PaletteColors;
					_cachedBgHMirror = pixelInfo->Bg.HMirror;
					_cachedBgVMirror = pixelInfo->Bg.VMirror;
				}
				
				if(imgIndex >= 0) {
					// Found HD replacement for BG - compute fade brightness and draw
					uint8_t bgFade = SmsHdPackApi::GetFadeBrightness(
						pixelInfo->Bg.TileData, false, bgPalGroup, pixelInfo->Bg.PaletteColors, pixelInfo->Bg.IsSg1000Mode);
					DrawHdTilePixelFromApi(imgIndex, srcX, srcY, hdScale, 
					                       pixelInfo->Bg.TileX, pixelInfo->Bg.TileY,
					                       pixelInfo->Bg.HMirror, pixelInfo->Bg.VMirror,
					                       outputBuffer, screenWidth, bgFade);
					
					// DEBUG: At 1x scale, verify HD pixel matches VDP pixel
					if constexpr(scale == 1) {
						static int mismatchCount = 0;
						uint8_t ci = pixelInfo->Bg.ColorIndex;
						if(ci > 0 && ci < 32 && mismatchCount < 30) {
							uint32_t vdpColor = _palette[ci];
							uint32_t hdColor = *outputBuffer;
							uint32_t hdAlpha = (hdColor >> 24) & 0xFF;
							// Only compare if HD pixel is opaque (non-transparent)
							if(hdAlpha == 0xFF && (hdColor & 0x00FFFFFF) != (vdpColor & 0x00FFFFFF)) {
								mismatchCount++;
								// Extract what color index the PNG pixel corresponds to
								// by reverse-looking up the HD color in the palette
								int hdPalIdx = -1;
								for(int pi = 0; pi < 32; pi++) {
									if((_palette[pi] & 0x00FFFFFF) == (hdColor & 0x00FFFFFF)) { hdPalIdx = pi; break; }
								}
								// Show tile data row bytes for the mismatched row
								uint8_t ty = pixelInfo->Bg.TileY;
								uint8_t rowOff = ty * 4;
								MessageManager::Log("[HD Mismatch] x=" + std::to_string(x) + " y=" + std::to_string(y) +
									" tileX=" + std::to_string(pixelInfo->Bg.TileX) + " tileY=" + std::to_string(ty) +
									" hMir=" + std::to_string(pixelInfo->Bg.HMirror) + " vMir=" + std::to_string(pixelInfo->Bg.VMirror) +
									" palIdx=" + std::to_string(pixelInfo->Bg.PaletteIndex) +
									" colorIdx=" + std::to_string(ci) + " hdPalIdx=" + std::to_string(hdPalIdx) +
									" vdp=0x" + HexUtilities::ToHex32(vdpColor) + " hd=0x" + HexUtilities::ToHex32(hdColor) +
									" row[" + std::to_string(ty) + "]=" + 
									HexUtilities::ToHex(pixelInfo->Bg.TileData[rowOff]) + " " +
									HexUtilities::ToHex(pixelInfo->Bg.TileData[rowOff+1]) + " " +
									HexUtilities::ToHex(pixelInfo->Bg.TileData[rowOff+2]) + " " +
									HexUtilities::ToHex(pixelInfo->Bg.TileData[rowOff+3]) +
									" srcXY=" + std::to_string(srcX) + "," + std::to_string(srcY));
							}
						}
					}
				} else {
					// No HD replacement - draw original BG pixel color
					uint8_t colorIndex = pixelInfo->Bg.ColorIndex;
					if(colorIndex < 32) {
						uint32_t color = _palette[colorIndex];
						DrawColor(color, outputBuffer, screenWidth);
					}
				}
		}

		// Step 2: Draw sprite on top (if present)
		// NES parity: Always try HD replacement first - let HD tile alpha control transparency
		// This allows HD tiles to have content where original sprite was transparent
		// But skip sprites if BG has priority (and BG pixel is non-transparent)
		if(pixelInfo->HasSprite && pixelInfo->Sprite.HasTileData) {
			// Check BG priority: if BG tile has priority and has a non-transparent pixel,
			// sprites should be hidden behind it (matches VDP behavior)
			bool bgHasPriority = pixelInfo->Bg.HasTileData && pixelInfo->Bg.Priority;
			uint8_t bgColorIndex = pixelInfo->Bg.ColorIndex;
			bool bgNonTransparent = (bgColorIndex != 0);
			
			// Skip sprite rendering if BG has priority and is non-transparent
			if(bgHasPriority && bgNonTransparent) {
				return;  // Don't draw sprite - it's behind the BG
			}
			
			uint8_t colorIndex = pixelInfo->Sprite.ColorIndex;
			bool spriteTransparent = (colorIndex == 0 || colorIndex == 0x10);
			
			int imgIndex = -1;
			uint16_t srcX = 0, srcY = 0;
			uint32_t hdScale = 1;
			
			// SG-1000 sprites have no palette banks — always palGroup=0.
			// SMS/GG sprites always use high palette (group 1).
			uint8_t spritePalGroup = pixelInfo->Sprite.IsSg1000Mode ? 0 : 1;
			
			// Sprites: Don't cache - multiple different sprites can appear on the same scanline
			// and caching causes incorrect tiles to be drawn when sprites overlap or change
			SmsHdPackApi::TryGetReplacementByHash(pixelInfo->Sprite.TileData, true, spritePalGroup,
			                                      imgIndex, srcX, srcY, hdScale,
			                                      pixelInfo->Sprite.PaletteColors, pixelInfo->Sprite.IsSg1000Mode);
			
			if(imgIndex >= 0) {
				// Found HD replacement for sprite - draw it on top of BG
				static int _sprDbgCount = 0;
				if(_sprDbgCount < 5) {
					_sprDbgCount++;
					const uint32_t* _dbgPx = nullptr; uint32_t _dbgW = 0, _dbgH = 0;
					SmsHdPackApi::GetImageData(imgIndex, _dbgPx, _dbgW, _dbgH);
					uint32_t _dbgSample = 0;
					if(_dbgPx && _dbgW > 0) {
						uint32_t _sx = srcX + (uint32_t)pixelInfo->Sprite.TileX * scale;
						uint32_t _sy = srcY + (uint32_t)pixelInfo->Sprite.TileY * scale;
						if(_sx < _dbgW && _sy < _dbgH) _dbgSample = _dbgPx[_sy * _dbgW + _sx];
					}
					MessageManager::Log("[SprHit#" + std::to_string(_sprDbgCount) +
						"] img=" + std::to_string(imgIndex) +
						" src=" + std::to_string(srcX) + "," + std::to_string(srcY) +
						" tXY=" + std::to_string(pixelInfo->Sprite.TileX) + "," + std::to_string(pixelInfo->Sprite.TileY) +
						" imgWH=" + std::to_string(_dbgW) + "x" + std::to_string(_dbgH) +
						" px=0x" + HexUtilities::ToHex(_dbgSample) +
						" scale=" + std::to_string(scale));
				}
				uint8_t sprFade = SmsHdPackApi::GetFadeBrightness(
					pixelInfo->Sprite.TileData, true, spritePalGroup, pixelInfo->Sprite.PaletteColors, pixelInfo->Sprite.IsSg1000Mode);
				DrawHdTilePixelFromApi(imgIndex, srcX, srcY, hdScale, 
				                       pixelInfo->Sprite.TileX, pixelInfo->Sprite.TileY,
				                       pixelInfo->Sprite.HMirror, pixelInfo->Sprite.VMirror,
				                       outputBuffer, screenWidth, sprFade);
			} else if(!spriteTransparent) {
				// No HD replacement - draw original sprite pixel color (only if not transparent)
				static int _sprMissDbg = 0;
				if(_sprMissDbg < 5) {
					_sprMissDbg++;
					MessageManager::Log("[SprMiss#" + std::to_string(_sprMissDbg) +
						"] colorIdx=" + std::to_string(colorIndex) +
						" palColor=0x" + HexUtilities::ToHex(_palette[colorIndex]) +
						" palGroup=" + std::to_string(spritePalGroup));
				}
				if(colorIndex < 32) {
					uint32_t color = _palette[colorIndex];
					DrawColor(color, outputBuffer, screenWidth);
				}
			}
		}
	}
	
	// Draw HD tile pixel using SmsHdPackApi image data
	// NES parity: Use template scale consistently for both reading PNG and writing output
	// fadeBrightness: 255 = full brightness (no fade), <255 = apply proportional darkening
	__forceinline void DrawHdTilePixelFromApi(int imgIndex, uint16_t srcX, uint16_t srcY, uint32_t /*hdScale*/,
	                                           uint8_t tileX, uint8_t tileY, bool hMirror, bool vMirror,
	                                           uint32_t* outputBuffer, uint32_t screenWidth,
	                                           uint8_t fadeBrightness = 255)
	{
		// Get the image from the API
		const uint32_t* imgPixels = nullptr;
		uint32_t imgWidth = 0, imgHeight = 0;
		if(!SmsHdPackApi::GetImageData(imgIndex, imgPixels, imgWidth, imgHeight) || !imgPixels) {
			return;
		}
		
		// NES parity: TileX/TileY already represent the logical position within the tile
		// as it appears on screen. The HD tile PNG is stored in non-mirrored orientation,
		// so we need to apply mirroring to map screen position to PNG position.
		// When HMirror is set: screen column 0 shows tile column 7, so we mirror.
		// When VMirror is set: screen row 0 shows tile row 7, so we mirror.
		uint32_t pixelX = hMirror ? (7 - tileX) : tileX;
		uint32_t pixelY = vMirror ? (7 - tileY) : tileY;
		
		// NES parity: Use template scale for tile size (matches how PNG was saved)
		// srcX/srcY point to the top-left of the tile in the PNG
		// pixelX/pixelY are the 0-7 position within the 8x8 tile
		
		// Draw the scale x scale block for this pixel (matches DrawTilePixel)
		for(uint32_t dy = 0; dy < scale; dy++) {
			for(uint32_t dx = 0; dx < scale; dx++) {
				uint32_t hdX = srcX + pixelX * scale + dx;
				uint32_t hdY = srcY + pixelY * scale + dy;
				
				if(hdX >= imgWidth || hdY >= imgHeight) {
					continue;
				}
				
				uint32_t srcOffset = hdY * imgWidth + hdX;
				uint32_t rgbValue = imgPixels[srcOffset];
				uint32_t alpha = (rgbValue >> 24) & 0xFF;
				
				// Apply fade brightness if needed (darken RGB channels proportionally)
				if(fadeBrightness < 255 && alpha > 0) {
					rgbValue = AdjustBrightness((uint8_t*)&rgbValue, fadeBrightness);
				}
				
				uint32_t* outPtr = outputBuffer + dy * screenWidth + dx;
				
				if(alpha == 255) {
					*outPtr = rgbValue;
				} else if(alpha > 0) {
					// Alpha blend
					uint8_t* out = (uint8_t*)outPtr;
					uint8_t* in = (uint8_t*)&rgbValue;
					uint8_t a = alpha + 1;
					uint8_t invA = 256 - alpha;
					out[0] = (uint8_t)((a * in[0] + invA * out[0]) >> 8);
					out[1] = (uint8_t)((a * in[1] + invA * out[1]) >> 8);
					out[2] = (uint8_t)((a * in[2] + invA * out[2]) >> 8);
					out[3] = 0xFF;
				}
			}
		}
	}

	// Process a single pixel - x,y are screen coordinates (legacy, for HdScreenInfoSms)
	__forceinline void GetPixels(uint32_t x, uint32_t y, HdSmsPixelInfo& pixelInfo, uint32_t* outputBuffer, uint32_t screenWidth)
	{
		HdPackTileInfoSms* hdPackTileInfo = nullptr;
		HdPackTileInfoSms* hdPackSpriteInfo = nullptr;

		bool hasSprite = pixelInfo.SpriteCount > 0;

		// Calculate pixel position within the 8x8 tile
		uint32_t pixelX = x & 0x07;
		uint32_t pixelY = y & 0x07;

		if(pixelInfo.Background.TileIndex >= 0) {
			hdPackTileInfo = GetCachedMatchingTile(x, y, &pixelInfo.Background);
		}

		uint32_t bgColor = _palette[0];
		DrawColor(bgColor, outputBuffer, screenWidth);

		if(hdPackTileInfo) {
			DrawTilePixel(pixelInfo.Background, *hdPackTileInfo, pixelX, pixelY, outputBuffer, screenWidth);
		} else if(pixelInfo.Background.TileIndex >= 0) {
			uint8_t palIndex = pixelInfo.Background.PaletteIndex;
			uint32_t color = _palette[palIndex * 16];
			DrawColor(color, outputBuffer, screenWidth);
		}

		if(hasSprite) {
			// Check if BG tile has priority flag set - sprites should be hidden behind it
			// Only hide sprites if BG has priority AND the BG pixel is non-transparent
			bool bgHasPriority = pixelInfo.Background.BackgroundPriority && pixelInfo.Background.TileIndex >= 0;
			
			for(int k = pixelInfo.SpriteCount - 1; k >= 0; k--) {
				HdSmsTileInfo& sprite = pixelInfo.Sprites[k];
				if(sprite.TileIndex < 0) continue;
				
				// Skip sprite if BG has priority and we drew a non-transparent BG pixel
				// The hdPackTileInfo check ensures we only hide sprites when there's actual BG content
				if(bgHasPriority && hdPackTileInfo) continue;

				hdPackSpriteInfo = GetMatchingTile(x, y, &sprite);
				if(hdPackSpriteInfo) {
					DrawTilePixel(sprite, *hdPackSpriteInfo, pixelX, pixelY, outputBuffer, screenWidth);
				}
			}
		}
	}

	void UpdatePalette()
	{
		if(!_console) {
			for(int i = 0; i < 32; i++) {
				uint8_t gray = (i * 8) & 0xFF;
				_palette[i] = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
			}
			return;
		}

		SmsVdp* vdp = _console->GetVdp();
		if(!vdp) return;

		SmsVdpState state = vdp->GetState();
		SmsModel model = _console->GetModel();

		// Check if we're in SG-1000/TMS9918 mode (not Mode 4)
		if(!state.UseMode4) {
			// SG-1000 mode uses fixed 16-color palette
			const uint16_t* sgPalette = vdp->GetSmsSgPalette();
			if(sgPalette) {
				for(int i = 0; i < 16; i++) {
					// Convert 15-bit RGB555 to 32-bit ARGB
					uint16_t rgb555 = sgPalette[i];
					uint8_t r = ((rgb555 >> 0) & 0x1F) << 3;
					uint8_t g = ((rgb555 >> 5) & 0x1F) << 3;
					uint8_t b = ((rgb555 >> 10) & 0x1F) << 3;
					_palette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
					_palette[i + 16] = _palette[i];  // Mirror to sprite palette
				}
			}
			return;
		}

		// SMS Mode 4 - use palette RAM
		uint8_t* paletteRam = vdp->GetPaletteRam();

		for(int i = 0; i < 32; i++) {
			if(model == SmsModel::GameGear) {
				uint16_t ggColor = paletteRam[i * 2] | (paletteRam[i * 2 + 1] << 8);
				uint8_t r = (ggColor & 0x0F) * 17;
				uint8_t g = ((ggColor >> 4) & 0x0F) * 17;
				uint8_t b = ((ggColor >> 8) & 0x0F) * 17;
				_palette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
			} else {
				uint8_t smsColor = paletteRam[i];
				uint8_t r = (smsColor & 0x03) * 85;
				uint8_t g = ((smsColor >> 2) & 0x03) * 85;
				uint8_t b = ((smsColor >> 4) & 0x03) * 85;
				_palette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
			}
		}
	}

	void OnBeforeApplyFilter()
	{
		UpdatePalette();
		if(_hdData) {
			for(auto& condition : _hdData->Conditions) {
				condition->Initialize(_hdScreenInfo);
			}
		}
		_useCachedTile = false;
		_cachedTile = nullptr;
	}

public:
	SmsHdPack(SmsConsole* console, EmuSettings* settings, HdPackDataSms* hdData)
	{
		_console = console;
		_settings = settings;
		_hdData = hdData;
	}

	virtual ~SmsHdPack() {}

	uint32_t GetScale() override { return scale; }

	void Process(HdScreenInfoSms* hdScreenInfo, uint32_t* outputBuffer, OverscanDimensions& overscan) override
	{
		_hdScreenInfo = hdScreenInfo;
		uint32_t hdScale = GetScale();
		uint32_t screenWidth = (SmsScreenWidth - overscan.Left - overscan.Right) * hdScale;

		OnBeforeApplyFilter();

		// Get VDP's pixel info directly - this is populated during rendering
		SmsVdp* vdp = _console ? _console->GetVdp() : nullptr;
		const SmsVdp::HdPixelInfo* vdpPixelInfo = vdp ? vdp->GetHdPixelInfo() : nullptr;
		
		// Get viewport offset - VDP writes to _hdPixelInfo using scanline 0-191,
		// but screen coordinates include the viewport offset (24 for 192-line mode)
		uint32_t viewportYOffset = vdp ? vdp->GetViewportYOffset() : 24;
		uint32_t visibleScanlineCount = vdp ? vdp->GetState().VisibleScanlineCount : 192;

		for(uint32_t y = overscan.Top, yMax = SmsScreenHeight - overscan.Bottom; y < yMax; y++) {
			_useCachedTile = false;
			// Invalidate API tile caches at start of each scanline for proper scroll handling
			_cachedBgTile.Valid = false;
			_prevBgTileX = 0xFF;
			_prevBgTileStart = std::numeric_limits<int16_t>::min();
			_cachedSpriteTile.Valid = false;
			// screenWidth already includes hdScale, so multiply by hdScale for row stride (hdScale rows per source pixel)
			uint32_t bufferIndex = (y - overscan.Top) * hdScale * screenWidth;

			for(uint32_t x = overscan.Left, xMax = SmsScreenWidth - overscan.Right; x < xMax; x++) {
				// Convert screen Y to VDP scanline (subtract viewport offset)
				// VDP writes _hdPixelInfo using scanline * 256 + x
				int32_t vdpScanline = (int32_t)y - (int32_t)viewportYOffset;
				
				// Only process pixels within the visible scanline range
				if(vdpScanline >= 0 && vdpScanline < (int32_t)visibleScanlineCount) {
					uint32_t pixelIndex = vdpScanline * SmsScreenWidth + x;
					
					// Use VDP's pixel info if available (preferred path for HD pack loading)
					if(vdpPixelInfo && pixelIndex < SmsVdp::MaxPixelsPerFrame) {
						GetPixelsFromVdp(x, y, &vdpPixelInfo[pixelIndex], outputBuffer + bufferIndex, screenWidth);
					} else if(hdScreenInfo && pixelIndex < hdScreenInfo->ScreenTiles.size()) {
						// Fallback to HdScreenInfoSms (for recording/builder path)
						GetPixels(x, y, hdScreenInfo->ScreenTiles[pixelIndex], outputBuffer + bufferIndex, screenWidth);
					} else {
						// No tile info - just draw background
						DrawColor(_palette[0], outputBuffer + bufferIndex, screenWidth);
					}
				} else {
					// Outside visible area - draw background
					DrawColor(_palette[0], outputBuffer + bufferIndex, screenWidth);
				}
				bufferIndex += hdScale;
			}
		}
	}
};
