# Recovery Partition

The project now reserves a dedicated `factory` slot that always hosts a minimal recovery firmware. This image only handles Wi-Fi setup and OTA flashing so that you can recover from a broken main build without opening the enclosure.

## Building & Flashing

1. **Build the recovery image once:**
   ```sh
   pio run -e esp32s3-devkitc1-recovery
   ```
2. **Flash it into the factory partition (0x10000) one time:**
   ```sh
   esptool.py --before default_reset --after hard_reset \
     write_flash 0x10000 .pio/build/esp32s3-devkitc1-recovery/firmware.bin
   ```
   After this initial install you normally update only the main OTA slots.
3. **Build/upload the main firmware as usual:**
   ```sh
   pio run -t upload -e esp32s3-devkitc1
   ```

OTA artifacts built from either environment already target the new `partitions_recovery.csv`, so release binaries remain compatible.

## Entering Recovery

- **Hardware shortcut:** hold the encoder **BACK + OK** buttons during power-up (for ~2 seconds). The device switches the boot slot to the `factory` partition, displays a notice, and reboots into the recovery firmware.
- **From the UI:** use `Menu → Recovery Mode` to trigger the same behavior without power cycling.
- **From recovery back to main:** hold the encoder OK button for ~1.5 seconds or use the "Boot Main Firmware" form on the web console.

## Recovery Features

- Starts its own AP named `TLTB-Recovery-XXXX` (no password) and keeps STA mode active; if stored Wi-Fi credentials are valid it also joins that network.
- Simple web console (http://AP_IP/) lets you:
  - Save / forget Wi-Fi credentials.
  - Upload a `.bin` file or provide a direct download URL for OTA flashing.
  - Tell the device which OTA slot to boot next (or default to the last known good slot stored in NVS).
- TFT shows the current AP/STA status, IP address, last action, and reminds you that holding OK will boot the main firmware.

Because the recovery image never touches the OTA slots, OTA updates continue to work exactly like before—only the active `ota_0`/`ota_1` partition gets rewritten when you push a new release.
