# SMS HD Pack: Debugging and Improvement Plan

This plan follows the “fix first, then refactor” rule and focuses on minimal, low-risk steps. It is split into diagnostics, stability hardening, and cleanup, without changing existing behavior.

## Current Baseline
- Repo restored to `origin/hdsms` (clean).
- Build (Release, x64) succeeds.
- Mesen launches.

## Objectives
- Ensure folders and files are created deterministically.
- Confirm tiles are captured only when intended.
- Keep behavior identical while improving observability and readiness for refactor.

## Phase 1 – Diagnostics (No Behavior Change)
- Add a central logging helper for HD pack code paths: `Core/SMS/HdPacks/HdPackDebug.h`.
  - Enable via `#define SMS_HD_DEBUG 1` in a translation unit, or with a project define for Debug configs.
  - Use tags: `Folders`, `Capture`, `Sheets`, `Manifest`, `Counters`.
- Instrumentation targets (only log calls, no logic changes):
  - `HdPackBuilderSms::StartRecording()` and `StopRecording()`
    - Log save folder path and whether folder creation succeeds.
    - Log counts: tiles seen, unique BG/Sprite, saved sheets.
  - `HdPackBuilderSms::SaveHdPack()`
    - Log manifest path, number of entries written per section.
  - `SmsVdp` capture points
    - Add counters: tiles encountered per frame (BG/Sprite) vs. tiles accepted by builder.
  - `SmsHdPackApi` glue
    - Add session begin/end logs with ROM name and sanitized output path.

## Phase 2 – Stability Hardening (No Behavior Change)
- Folder creation guards:
  - Before any write: ensure `HdPacks/<RomName>/` exists.
  - Ensure subfolders like `Graphics/` (if used) exist.
  - On failure, log explicit errors with OS error message.
- File path safety:
  - Sanitize input strings (ROM names) and assert/guard against empty paths.
  - Prefer `FolderUtilities::CombinePath()` only with known-safe parts.

## Phase 3 – SaveTileSheet Refactor Blueprint (Form Only)
- Keep behavior intact; split responsibilities into helpers:
  - `CollectTilesForSheet(...)`
  - `CreateSheetBitmap(...)`
  - `WriteSheetPng(...)`
  - `AnnotateDebugOverlay(...)` (only when DebugMode)
  - `RecordManifestEntriesForSheet(...)`
- Add unit-testable helpers that do not rely on global state. Keep the old public API unchanged initially.

## Phase 4 – Validation Tools
- Add a light-weight manifest validator (offline tool or script) to check:
  - All referenced PNG files exist.
  - Coordinates are multiples of tile size.
  - Palette indices are valid for console/VDP mode.
  - Counts match sheet grids (e.g., 16x16 tiles => 256 entries per sheet).
- Create a developer checklist (see `docs/SMS_HdPack_Manifest_Validation.md`).

## Phase 5 – Warnings and Hygiene
- Triage harmless warnings in `HdPackBuilderSms.cpp` (unused locals, signed/unsigned mismatches).
  - Remove or wrap with `#if defined(SMS_HD_DEBUG)` where they’re debug-only.
  - Add comments capturing intent.

## Phase 6 – UI/Config Quality of Life (Optional)
- Add a `DebugMode` toggle to `SmsHdPackBuilderViewModel` to turn on HD logs without altering production behavior.
- Optional: Toasts when a recording starts/stops, with folder link.

## Rollout Strategy
- Apply Phase 1 logging and Phase 2 folder guards behind `SMS_HD_DEBUG` macros for Debug builds only.
- Verify with a few ROMs (SMS, GG, SG-1000) and observe counters/logs without behavior change.
- After validation, proceed with Phase 3 refactor in small PRs.

## Notes
- Keep manifest format identical.
- Avoid changes to NES paths.
- Do not change tile hashing or dedup behavior during Phase 1–2.
