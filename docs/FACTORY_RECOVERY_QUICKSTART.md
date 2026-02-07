# Factory Recovery - Quick Start

## Problem Solved
ESP32 devices that have been powered down for days/weeks can develop flash corruption, causing OTA updates to fail. This factory recovery system provides a bulletproof way to recover these devices.

## Solution Overview
We've implemented a **factory partition** that contains minimal recovery firmware. When you need to recover a device:

1. Hold **BACK button** for **5+ seconds** during power-on
2. Device boots into factory recovery mode
3. Factory firmware performs OTA update to fresh partitions
4. Device reboots into updated main firmware

## Initial Setup (One Time)

Flash both firmwares to your device:

```powershell
# 1. Flash main firmware
pio run -e esp32s3-devkitc1 -t upload

# 2. Flash factory recovery firmware
pio run -e factory -t upload
```

✅ **Done!** Your device now has recovery capability.

## Daily Development

For normal development, only flash the main firmware:

```powershell
pio run -e esp32s3-devkitc1 -t upload
```

The factory partition stays unchanged.

## Using Recovery Mode

When your device won't accept OTA updates after being powered off:

1. **Power off** the device
2. **Hold BACK button**
3. **Power on** (keep holding BACK)
4. Display shows **"RECOVERY MODE..."** with countdown (5, 4, 3, 2, 1)
5. Keep holding until countdown reaches 0
6. Display shows **"ENTERING RECOVERY"** and device restarts
7. Press **OK** to start OTA (or wait 10 seconds)
8. Device downloads latest firmware from GitHub
9. Device automatically reboots to updated firmware

## What Each Button Does During Boot

- **No button**: Normal boot - full application starts
- **BACK held**: Factory recovery mode triggered after 5-second countdown (visible on screen)

## Files Created

| File | Purpose |
|------|---------|
| `partitions_factory.csv` | Partition layout with factory partition |
| `src_factory/main.cpp` | Minimal recovery firmware |
| `src_factory/ota/*` | OTA update code (shared) |
| `scripts/factory_build.py` | Build script for factory environment |
| `docs/FACTORY_RECOVERY.md` | Full documentation |

## Partition Layout

```
0x10000  - COREDUMP (64KB)   ← Crash dump storage
0x20000  - OTA_0 (1.81MB)    ← Normal boot partition A
0x1F0000 - OTA_1 (1.81MB)    ← Normal boot partition B  
0x3C0000 - FACTORY (2.0MB)   ← Recovery partition (rarely accessed)
0x5C0000 - SPIFFS (256KB)    ← File storage
```

## Technical Notes

- Factory firmware is **~880KB** (vs 1.1MB main firmware)
- Only includes: WiFi, OTA, Display
- Excludes: BLE, RF, sensors, relays
- Minimal code = fewer bugs, more reliable

## Why This Works

**Old approach (failed):**
- Boot to OTA partition A or B
- Both could be corrupted after long power-down
- No way to recover without physical access

**New approach (reliable):**
- Factory partition is write-protected
- Only accessed when explicitly requested
- Can completely erase and reflash both OTA partitions
- Always works, even if OTA partitions are corrupt

## Troubleshooting

**"Factory partition not found"**
→ Flash factory firmware: `pio run -e factory -t upload`

**Recovery mode doesn't start**
→ Hold BACK button DURING power-on for full 5 seconds

**OTA update fails in recovery mode**
→ Check WiFi credentials are saved in main firmware
→ Verify GitHub release has a .bin file

## Memory Usage Comparison

| Component | Main Firmware | Factory Firmware |
|-----------|--------------|------------------|
| Flash | 1.17 MB (59.6%) | 0.88 MB (44.9%) |
| RAM | 53.8 KB (16.4%) | 45.8 KB (14.0%) |

Factory firmware is leaner and more reliable!

## See Also

- [docs/FACTORY_RECOVERY.md](FACTORY_RECOVERY.md) - Full technical documentation
- [src_factory/main.cpp](../src_factory/main.cpp) - Recovery firmware source
- [partitions_factory.csv](../partitions_factory.csv) - Partition configuration

---

**🎉 Your device is now protected against flash corruption!**
