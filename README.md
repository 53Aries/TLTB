# TLTB Mobile App

Cross-platform (Android + iOS) companion app for the TLTB platform. The app pairs to the ESP32 controller over Bluetooth LE, exposes six relay controls, and mirrors the home-screen telemetry (active function, current draw, etc.).

## Getting started

```bash
cd "TLTB App"
npm install
npm run start            # launches Expo Dev Tools
npm run android          # open Expo Go on Android
npm run ios              # requires macOS / iOS simulator
npm run web
npm run typecheck        # TypeScript only
```

## Project layout

- `App.tsx` – entry that mounts the provider tree.
- `src/AppProviders.tsx` – wraps navigation, safe areas, and BLE lifecycle hooks.
- `src/state/deviceStore.ts` – central Zustand store for connection, relays, and the home-status snapshot (mode, load, voltages, cooldown, faults).
- `src/services/ble/mockBleSession.ts` – mock transport that simulates the ESP32 until real firmware is ready.
- `src/services/ble/tltbBleSession.ts` – real BLE client built on `react-native-ble-plx` (scan, connect, subscribe, command write).
- `src/hooks/useBleTransport.ts` – routes between mock and real BLE based on environment flags.
- `src/config/appConfig.ts` & `src/config/bleProfile.ts` – runtime feature flags and the expected service/characteristic UUIDs.
- `src/storage/deviceCache.ts` & `src/hooks/useKnownDeviceBootstrap.ts` – caches the last connected controller (id/name/RSSI) and hydrates the store before BLE starts.
- `src/components/HomeStatusPanel.tsx` – mirrors the TFT home screen (mode, load, batt volt, system volt, cooldown).
- `src/components/FaultTicker.tsx` – reproduces the red scrolling banner when faults exist.
- `src/components/*` – relay grid, status banner, and supporting primitives.
- `src/screens/HomeScreen.tsx` – main dashboard surfaced today.

## BLE integration plan

1. **Document the controller profile** – service UUIDs, read/write characteristics, notification rate, and security mode. Update `src/config/bleProfile.ts` with the final IDs; the notification payload should include: mode, load amps, src/out volts, bypass flags, relay states, cooldown, startup guard, and fault bitmask/message strings.
2. **Firmware payload contract** – the app expects the status characteristic to emit base64-encoded JSON matching `HomeStatusSnapshot` plus an optional `relays` map. Keep property names stable to avoid parsing failures (see `src/services/ble/statusParser.ts`).
3. **Persist device metadata** – store the last connected device ID and auto-reconnect on launch; fall back to a guided scan if the device is unavailable.
4. **Command reliability** – keep optimistic UI but listen for firmware acknowledgments; rollback a relay tile if an ACK is missed or rejected.
5. **Security** – require numeric comparison or a pre-shared PIN during pairing, and encrypt command payloads if the firmware exposes a custom scheme.

## Maximizing BLE range

- Use BLE 5 coded PHY (S2/S8) on the ESP32 when available, otherwise 2M PHY with the highest legal TX power.
- Lengthen the advertising interval only after the link is stable; start aggressive (e.g., 100–200 ms) to make discovery snappy, then relax for battery savings.
- Expose an external antenna or tuned PCB trace on the hardware, and keep the mobile app polling at modest frequency (2–4 Hz) so airtime stays low.
- Allow the app to show RSSI and prompt the user to get closer when commands fail.

## Next steps

1. Fill in the final UUIDs + payload schema, then flip `EXPO_PUBLIC_USE_MOCK_BLE=false` to exercise the real transport.
2. Mirror the firmware schema (relay names, telemetry fields) via a shared JSON file or codegen step so breaking changes are obvious.
3. Add integration tests (Detox or Maestro) that tap each relay tile and verify the correct GATT writes are issued.
4. Prepare release builds via Expo EAS once BLE is stable.

## Environment flags

| Variable | Default | Description |
| --- | --- | --- |
| `EXPO_PUBLIC_USE_MOCK_BLE` | `true` | When `false`, the app skips the simulator and connects via `react-native-ble-plx`. |
| `EXPO_PUBLIC_BLE_DEVICE_PREFIX` | `TLTB` | Device name prefix to filter during scans. |
| `EXPO_PUBLIC_BLE_RECONNECT_MS` | `4000` | Delay before attempting to reconnect after a drop. |

Update `src/config/bleProfile.ts` with the production service + characteristic UUIDs to ensure the scan/connect logic discovers the right controller. Expo will inline these values at build time, so restarting Metro is required after edits.

## Reconnection UX

- The app stores the last connected device ID/name/RSSI via AsyncStorage so reconnection focuses on that controller before scanning every peripheral.
- `StatusBanner` now surfaces the cached device name and most recent RSSI to help with range diagnostics.
- RSSI samples are polled every 4 seconds while connected; the latest value is persisted so you can see the last known signal even if the link drops.
