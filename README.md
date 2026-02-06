# TLTB Mobile App

Cross-platform (Android + iOS) companion app for the TLTB platform. The app pairs to the ESP32 controller over Bluetooth LE, exposes six relay controls, and mirrors the home-screen telemetry (active function, current draw, etc.). The firmware already exposes the matching BLE profile, so you can toggle a single env flag to talk to hardware.

## Getting started

```bash
cd "TLTB App"
npm install
npm run start            # launches Expo Dev Tools (respects env vars)
npm run android          # open Expo Go on Android
npm run ios              # requires macOS / iOS simulator
npm run web              # mock transport only
npm run typecheck        # TypeScript only
npm test                 # Vitest suite for BLE payload + state logic
npm run decode:ble-payload -- --value <base64>
```

## Project layout

- `App.tsx` – entry that mounts the provider tree.
- `src/AppProviders.tsx` – wraps navigation, safe areas, and BLE lifecycle hooks.
- `src/state/deviceStore.ts` – central Zustand store for connection, relays, and the home-status snapshot (mode, load, voltages, cooldown, faults).
- `src/services/ble/mockBleSession.ts` – mock transport that simulates the ESP32 (default when the BLE env flag is left on).
- `src/services/ble/tltbBleSession.ts` – real BLE client built on `react-native-ble-plx` (scan, connect, subscribe, command write).
- `src/hooks/useBleTransport.ts` – routes between mock and real BLE based on environment flags.
- `src/config/appConfig.ts` & `src/config/bleProfile.ts` – runtime feature flags and the expected service/characteristic UUIDs.
- `src/storage/deviceCache.ts` & `src/hooks/useKnownDeviceBootstrap.ts` – caches the last connected controller (id/name/RSSI) and hydrates the store before BLE starts.
- `src/components/HomeStatusPanel.tsx` – mirrors the TFT home screen (mode, load, batt volt, system volt, cooldown).
- `src/components/FaultTicker.tsx` – reproduces the red scrolling banner when faults exist.
- `src/components/*` – relay grid, status banner, and supporting primitives.
- `src/screens/HomeScreen.tsx` – main dashboard surfaced today.

## Current BLE integration

- **Profile** – `src/config/bleProfile.ts` contains the production UUIDs (`0000a11c-*` service plus status/control characteristics). Scans filter by the configured service UUID and the `TLTB` name prefix.
- **Payload contract** – firmware emits Base64 JSON matching `HomeStatusSnapshot` plus a `relays` map; parsing lives in `src/services/ble/statusParser.ts` and feeds the Zustand store.
- **Schema contract** – `docs/ble_payload_schema.json` mirrors `BleStatusPayload`; run `npm run export:ble-schema` after tweaking relay IDs or telemetry fields, `npm run validate:ble-payload -- docs/examples/status_sample.json` to lint fixtures, and `npm run decode:ble-payload -- --value <base64>` to inspect captured notifications.
- **Transport selection** – `useBleTransport` chooses the real client when `EXPO_PUBLIC_USE_MOCK_BLE=false`; restart Metro after toggling so Expo picks up the flag.
- **Device memory** – AsyncStorage + `useKnownDeviceBootstrap` persist the last controller (id/name/RSSI) and hydrate the store pre-scan for faster reconnects.
- **Command flow** – relay taps still optimistically flip UI state, but the app now waits for the next status frame to confirm each `{ type: 'relay', relayId, state }` write; missing acks within ~3 seconds revert the tile and surface a dismissal banner inside StatusBanner.
- **Security/TX power** – the firmware currently allows unauthenticated, no-IO pairing and runs at `ESP_PWR_LVL_P9`; when that policy evolves we’ll mirror requirements here.

## Maximizing BLE range

- The firmware now locks ESP32 TX power to `ESP_PWR_LVL_P9` for default/scan/adv roles and advertises every 20–40 ms; keep this aggressive profile unless a regulatory test requires lowering it.
- Connection parameter requests target 7.5–15 ms intervals so notifications and command retries stay extremely chatty—battery draw is not a constraint.
- Expose an external antenna or tuned PCB trace on the hardware to take advantage of the extra airtime.
- The app samples RSSI every 2 seconds and auto-reconnects after 1.5 seconds to keep the link sticky and give users immediate feedback when signal drops.

## Next steps

1. Hook the generated schema (`docs/ble_payload_schema.json`) into firmware CI/tests (you can reuse `npm run validate:ble-payload -- <file>` to lint fixtures) so payload drift is caught automatically.
2. Add integration tests (Detox or Maestro) that tap each relay tile and verify the correct GATT writes are issued.
3. Prepare release builds via Expo EAS once BLE is stable.

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
