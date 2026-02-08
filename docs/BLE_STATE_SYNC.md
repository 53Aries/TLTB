# BLE State Synchronization

## Overview

The BLE implementation ensures **100% synchronization** between the ESP32 device and mobile app at all times, including across disconnection/reconnection events.

## Synchronization Mechanisms

### 1. Connection Establishment Sync

**ESP Side:**
- When a client connects, `handleClientConnect()` immediately:
  - Sets `_forceNextStatus = true`
  - Resets `_lastNotifyMs = 0`
  - Queues immediate status notification

**App Side:**
- After successful connection, `connectToDevice()`:
  - Establishes status monitor
  - Sends explicit `refresh` command after 100ms delay
  - Starts heartbeat monitoring

**Result:** App receives current relay states within ~100-200ms of connection.

### 2. Periodic Status Updates

**ESP Side:**
- Sends status notifications every 1 second (`kStatusIntervalMs = 1000`)
- Status includes:
  - All 6 relay states as bitmask (`relayMask`)
  - Telemetry data (voltage, current)
  - Protection status flags
  - Active mode and label

**App Side:**
- Monitors status characteristic for notifications
- Updates relay state store on each notification
- Updates heartbeat timestamp

### 3. Command Acknowledgment

**App Side:**
- When user toggles relay, app:
  1. Optimistically updates UI
  2. Sends command to ESP
  3. Waits for acknowledgment via status update (3s timeout)
  4. Reverts UI if acknowledgment not received

**ESP Side:**
- Processes relay command
- Updates physical relay state
- Forces immediate status notification (`requestImmediateStatus()`)

**Result:** User gets immediate feedback, but UI reverts if command fails.

### 4. Heartbeat Monitoring

**App Side:**
- Tracks timestamp of last received status notification
- If no status received for 5 seconds (`STATUS_HEARTBEAT_TIMEOUT_MS`):
  - Assumes connection is stale
  - Triggers disconnect and reconnect

**ESP Side:**
- Continuously sends status every 1 second while connected
- Acts as implicit heartbeat

**Result:** Dead connections are detected and recovered automatically.

## Reconnection Behavior

### Scenario: BLE Disconnects and Reconnects

1. **Disconnect detected** (either side)
   - App clears all timers and subscriptions
   - App marks connection as disconnected
   - App schedules reconnect attempt

2. **Reconnect initiated**
   - App scans for device
   - App discovers and connects to device

3. **Sync on reconnect**
   - ESP detects new connection → forces immediate status
   - App completes connection setup → sends refresh command
   - App receives status notification with current relay states
   - UI updates to reflect current ESP state

4. **Operation resumes**
   - Periodic status updates continue every 1 second
   - Heartbeat monitoring active

### Edge Cases Handled

- **Relay changed while disconnected:** ESP state persists, app receives current state on reconnect
- **MTU negotiation delay:** Status sent after MTU ready, no blocking
- **App killed and restarted:** Known device cache allows quick reconnect, sync triggers immediately
- **Multiple rapid reconnects:** Each connection triggers fresh sync
- **Status notification loss:** Heartbeat timeout triggers reconnect if ESP alive

## Testing Recommendations

1. **Basic Sync Test:**
   - Connect app to device
   - Toggle relay from app → verify ESP matches
   - Toggle relay from device (if possible) → verify app matches

2. **Disconnect Test:**
   - Connect app
   - Toggle several relays
   - Disable Bluetooth on phone
   - Toggle relays via RF remote or local controls
   - Re-enable Bluetooth
   - Verify app shows correct current state

3. **Reconnection Stress Test:**
   - Connect/disconnect repeatedly
   - Verify no state drift over 100+ reconnections

4. **Timeout Test:**
   - Connect app
   - Put device in Faraday cage or similar RF-blocking environment
   - Verify app detects stale connection within ~5-6 seconds
   - Remove from cage
   - Verify reconnection and sync

## Configuration Constants

### ESP Side (`TltbBleService.cpp`)
```cpp
constexpr uint32_t kStatusIntervalMs = 1000;  // Status update rate
```

### App Side (`tltbBleSession.ts`)
```typescript
const RSSI_INTERVAL_MS = 2000;                    // RSSI poll rate
const RELAY_ACK_TIMEOUT_MS = 3000;                // Command acknowledgment timeout
const STATUS_HEARTBEAT_TIMEOUT_MS = 5000;         // Stale connection detection
```

## Synchronization Guarantees

✅ **Always synced on connection** - Immediate status sent
✅ **Always synced on reconnection** - Fresh sync triggered
✅ **Commands acknowledged** - App knows if command succeeded
✅ **Dead connections detected** - Heartbeat monitoring
✅ **State persistence** - ESP maintains physical relay state
✅ **No phantom toggles** - App reverts failed commands

## Logging

Enable debug logging to trace synchronization:

**ESP:**
```
[TLTB-BLE] Client connected, waiting for MTU negotiation
[TLTB-BLE] Immediate status sync queued for new connection
[TLTB-BLE] MTU negotiated: 512 bytes
```

**App:**
```
[BLE] Requesting state sync after connection...
[BLE] State sync requested successfully
[BLE] Received status notification, length: XXX
[BLE] Parsed status successfully: HD OFF
```
