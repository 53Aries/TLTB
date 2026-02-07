# OTA Update & Validation System

## Overview
The TLTB firmware uses ESP32's built-in OTA rollback protection to ensure safe firmware updates. This document explains how the validation system works and prevents error 9 (activation failures).

## How It Works

### 1. OTA Update Process (Ota.cpp)

When an OTA update is triggered:

1. **Pre-flight Checks**
   - Verify Wi-Fi is connected
   - Check current partition state
   - **BLOCK OTA if firmware is still PENDING_VERIFY** (prevents race condition)
   - BLOCK OTA if firmware is marked INVALID
   - Warn if previous OTA was ABORTED

2. **Download & Flash**
   - Fetch latest release from GitHub
   - Stream firmware to inactive partition
   - Verify write operations
   - Persist version tag to NVS

3. **Reboot**
   - New partition boots in **ESP_OTA_IMG_PENDING_VERIFY** state
   - Firmware has 8 seconds to validate itself
   - If validation fails or device crashes, ESP32 rolls back on next boot

### 2. Validation on Boot (main.cpp)

When device boots after OTA:

1. **Early Boot Check** (setup())
   ```cpp
   - Check if running partition is in PENDING_VERIFY state
   - Set g_otaPendingVerify flag
   - Record boot timestamp
   - Display "New firmware booted... Validating..." on screen
   ```

2. **Continuous Validation** (loop())
   ```cpp
   - After 8 seconds of stable operation:
     - Verify critical subsystems (INA226 sensors present)
     - Call esp_ota_mark_app_valid_cancel_rollback()
     - Display green bar on success
   - If validation fails after 15 seconds:
     - Give up, allow rollback on next boot
     - Display red bar warning
   ```

### 3. Safety Features

#### Race Condition Prevention
- **Problem**: User triggers OTA before previous firmware validates (within 8s window)
- **Solution**: Block OTA attempts if current firmware is PENDING_VERIFY
- **Error Message**: "Wait 10 seconds after boot before updating"

#### System Health Checks
- Verify INA226 sensors are present before validation
- Prevent validating broken firmware
- Allow rollback if critical systems fail

#### Visual Feedback
- Startup message: "New firmware booted... Validating..."
- Success: Green bar at bottom of screen
- Failure: Red bar warning

## Partition Table

The custom partition table (partitions.csv) provides:
- **3MB per OTA slot** (app0 and app1)
- **otadata partition** for OTA state tracking
- **10MB SPIFFS** for future data storage

```
nvs:      20KB   @ 0x9000
otadata:  8KB    @ 0xE000
app0:     3MB    @ 0x10000
app1:     3MB    @ 0x310000
spiffs:   10MB   @ 0x610000
```

## Common Issues & Solutions

### Error 9: "Could not activate firmware"

**Cause**: Race condition - OTA attempted while firmware still pending verification

**Solution**: System now blocks OTA during 8-second validation window

### Updates Work, Then Stop Working

**Cause**: Missing partition table or partition alignment issues

**Solution**: Custom partitions.csv with proper 3MB slots and otadata partition

### Firmware Doesn't Validate

**Cause**: System instability during 8-second validation window

**Check**:
- Are INA226 sensors working?
- Is device crashing/resetting?
- Check serial output for validation errors

## Best Practices

### For Users
1. Wait at least 10 seconds after boot before triggering OTA
2. Keep device powered and stable during update
3. Don't power off during validation (first 10s after update)

### For Developers
1. Test new firmware for at least 10 seconds before OTA
2. Ensure critical subsystems initialize reliably
3. Monitor serial output during validation
4. If firmware becomes corrupted, use factory recovery mode (hold BACK ≥ 5 sec during power-on)

## Rollback Behavior

If validation fails or device crashes within 8 seconds:
- ESP32 automatically rolls back to previous firmware
- Device boots into old, working firmware
- No manual recovery needed

If device crashes after validation completes:
- Firmware is marked valid, no automatic rollback
- Manual recovery required (USB flash)

## Technical Details

### ESP32 OTA States
- `ESP_OTA_IMG_NEW` - Freshly flashed, not yet booted
- `ESP_OTA_IMG_PENDING_VERIFY` - Booted, awaiting validation
- `ESP_OTA_IMG_VALID` - Validated, safe to use
- `ESP_OTA_IMG_INVALID` - Failed validation, will rollback
- `ESP_OTA_IMG_ABORTED` - Update was aborted mid-flash

### Validation Timing
- **8 seconds**: Minimum stable runtime before validation
- **15 seconds**: Maximum time to attempt validation before giving up
- **500ms**: Delay after OTA complete to ensure NVS write finishes

### Error Codes
- **Error 9**: Partition activation failure (race condition)
- **HTTP errors**: Network/GitHub issues during download
- **Write errors**: Insufficient space or flash corruption
