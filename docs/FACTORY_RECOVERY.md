# Factory Recovery System

## Overview

This system provides a bulletproof OTA recovery mechanism for ESP32-S3 devices that have been powered down for extended periods and may have corrupted flash partitions.

## How It Works

The ESP32 flash is divided into multiple partitions:

```
┌──────────────────────────────────────┐
│ NVS (Settings storage)               │ 0x9000
├──────────────────────────────────────┤
│ OTA Data (Boot partition selector)   │ 0xE000
├──────────────────────────────────────┤
│ Coredump (Crash dumps)               │ 0x10000 (64KB)
├──────────────────────────────────────┤
│ OTA_0 (Main firmware - Slot A)       │ 0x20000  (1.81MB)
├──────────────────────────────────────┤
│ OTA_1 (Backup firmware - Slot B)     │ 0x1F0000 (1.81MB)
├──────────────────────────────────────┤
│ FACTORY (Recovery firmware)          │ 0x3C0000 (2.0MB)
├──────────────────────────────────────┤
│ SPIFFS (File storage)                │ 0x5C0000 (256KB)
└──────────────────────────────────────┘
```

**Normal Operation:**
- Device boots from OTA_0 or OTA_1
- OTA updates alternate between slots for rollback safety
- After long power-down, these partitions may become corrupted

**Recovery Mode:**
- Factory partition contains minimal recovery firmware
- Recovery firmware ONLY does WiFi + OTA
- Can completely erase and reflash OTA partitions
- Factory partition is write-protected and rarely accessed

## Usage

### Entering Recovery Mode

**At device power-on:**
- **No button**: Normal boot - starts main application firmware
- **Hold BACK button**: Display shows countdown (5 seconds), then boots to factory recovery mode
  - Visual feedback: "RECOVERY MODE..." with countdown timer
  - Release button early to cancel and continue normal boot
  - Keep holding through countdown to enter recovery mode

When in factory recovery mode:
- Display shows "RECOVERY MODE" 
- Press **OK** to start OTA update (or wait 10 seconds for auto-start)
- Press **BACK** to cancel and return to normal firmware

### First-Time Setup

You need to flash both the main firmware AND the factory recovery firmware:

```bash
# 1. Build and upload main firmware to OTA_0
pio run -e esp32s3-devkitc1 -t upload

# 2. Build and upload factory recovery firmware
pio run -e factory -t upload
```

⚠️ **Important:** The factory firmware must be flashed separately at least once. After that, you only need to update the main firmware via OTA.

### Normal Development Workflow

For day-to-day development, only build/upload the main firmware:

```bash
pio run -e esp32s3-devkitc1 -t upload
```

The factory partition remains unchanged unless you specifically update it.

### When to Use Recovery Mode

Use factory recovery mode when:
- Device has been powered off for days/weeks and won't accept OTA updates
- Both OTA partitions appear corrupted
- Normal OTA updates fail with timeout or flash errors
- Device is stuck in a boot loop

### Troubleshooting

**"Factory partition not found":**
- You need to flash the factory firmware first
- Run: `pio run -e factory -t upload`

**Recovery OTA fails:**
- Ensure WiFi credentials are saved in NVS
- Check that the device can reach GitHub (internet access)
- Verify your GitHub repo has a release with a .bin file

**Can't enter recovery mode:**
- Make sure you're holding BACK button DURING power-on
- Hold for full 5 seconds until device restarts
- Check button connections

## Technical Details

### Partition Sizes

- **OTA_0 / OTA_1:** 1.81MB each (sufficient for full featured firmware)
- **Factory:** 2.0MB (minimal firmware, plenty of space)
- **Coredump:** 64KB (crash dump storage)
- **NVS:** 20KB (settings storage)
- **SPIFFS:** 256KB (file storage)

### Build Configurations

**Main Environment (`esp32s3-devkitc1`):**
- Source: `src/`
- Full featured firmware with BLE, RF, sensors, etc.
- Uploads to OTA_0 (first flash) or alternates (via OTA)

**Factory Environment (`factory`):**
- Source: `src_factory/`
- Minimal: WiFi + OTA + Display only
- Uploads directly to factory partition at 0x3C0000
- No BLE, no RF, no sensors - just recovery capability

### Memory Safety

The factory partition is:
- Only accessed when explicitly requested (5-second button hold)
- Never modified during normal OTA updates
- Protected from accidental corruption
- Small attack surface (minimal code = fewer bugs)

## Developer Notes

### Modifying Factory Firmware

The factory firmware is intentionally minimal. If you need to modify it:

1. Edit `src_factory/main.cpp`
2. Keep it minimal - only WiFi, OTA, and basic display
3. Test thoroughly before flashing
4. Flash with: `pio run -e factory -t upload`

### Modifying Partition Layout

If you need to change partition sizes, edit `partitions_factory.csv`:
- Ensure factory partition is large enough for minimal firmware (~1.5MB is safe)
- OTA partitions should be identical size
- All offsets must be aligned to 0x1000 (4KB)
- Total must not exceed flash size (8MB for ESP32-S3-DevKitC-1)

### Testing Recovery Mode

To test without waiting for long power-down:

1. Flash main firmware
2. Flash factory firmware  
3. Power on with BACK held for 5+ seconds
4. Verify display shows "RECOVERY MODE"
5. Press OK to trigger OTA
6. Should download and flash latest GitHub release

## Future Enhancements

Potential improvements:
- [ ] Add USB serial OTA option (no WiFi needed)
- [ ] Support SD card firmware loading
- [ ] Add partition verification/repair tools
- [ ] Implement factory reset feature
- [ ] Add partition backup/restore

## Related Files

- [partitions_factory.csv](../partitions_factory.csv) - Partition table definition
- [src_factory/main.cpp](../src_factory/main.cpp) - Factory recovery firmware
- [src/main.cpp](../src/main.cpp) - Main firmware (see factory boot detection)
- [platformio.ini](../platformio.ini) - Build configuration
