#pragma once
#include "pch.h"
#include <string>

// Minimal debug dump manager for SMS HD pack debug outputs
// Responsible for writing structured debug information and related assets.
class SmsHdDebugDump {
public:
    SmsHdDebugDump() = default;

    // Writes a human-readable debug info text file summarizing the dump session.
    // Palettes are expected as 16-entry ARGB arrays.
    void WriteDebugInfo(
        const std::string& saveFolder,
        const std::string& romName,
        uint32_t scale,
        uint32_t totalSpriteCount,
        uint32_t totalBgCount,
        uint32_t uniqueSpriteCount,
        uint32_t uniqueBgCount,
        size_t totalUniqueTiles,
        const uint32_t spritePalette[16],
        const uint32_t bgPalette[16]
    );
};
