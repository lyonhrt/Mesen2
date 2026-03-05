#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SmsHdPackSharedConstants {
	constexpr size_t TilePaletteEntryCount = 16;
	constexpr size_t MaxPaletteBytesPerEntry = 2; // SMS uses 1 byte, Game Gear uses 2 bytes per color
	constexpr size_t TilePaletteDataSize = TilePaletteEntryCount * MaxPaletteBytesPerEntry;

	enum class PaletteFormat : uint8_t {
		None = 0,
		Sms = 1,
		GameGear = 2,
		Sg1000 = 3
	};

	struct CapturedPalette {
		uint8_t Data[TilePaletteDataSize] = {};
		uint8_t EntryCount = 0;
		uint8_t BytesPerEntry = 0;
		PaletteFormat Format = PaletteFormat::None;

		void Reset() {
			std::memset(Data, 0, sizeof(Data));
			EntryCount = 0;
			BytesPerEntry = 0;
			Format = PaletteFormat::None;
		}

		bool IsValid() const {
			return Format != PaletteFormat::None && EntryCount > 0 && BytesPerEntry > 0;
		}

		bool IsAllZero() const {
			size_t bytesToCheck = static_cast<size_t>(EntryCount) * BytesPerEntry;
			if(bytesToCheck == 0 || bytesToCheck > sizeof(Data)) return true;
			for(size_t i = 0; i < bytesToCheck; i++) {
				if(Data[i] != 0) return false;
			}
			return true;
		}
	};
}
