import { encode as base64Encode } from 'base-64';
import { BleManager, Device, Subscription } from 'react-native-ble-plx';

import { appConfig } from '@/config/appConfig';
import { bleProfile } from '@/config/bleProfile';
import { RelayId } from '@/types/device';
import { KnownDevice } from '@/state/deviceStore';

import { parseStatusNotification } from './statusParser';
import { BleSession, BleSessionHandlers } from './types';

const matchesPreferredName = (device?: Device | null) => {
  if (!device?.name) {
    return false;
  }
  return device.name.startsWith(bleProfile.preferredDeviceNamePrefix ?? appConfig.deviceNamePrefix);
};

const encodeJson = (payload: unknown) => base64Encode(JSON.stringify(payload));

interface SessionOptions {
  initialKnownDevice?: KnownDevice | null;
  onKnownDevice?: (device: KnownDevice) => void;
  onSignalUpdate?: (rssi: number | null) => void;
}

const RSSI_INTERVAL_MS = 2000;
const RELAY_ACK_TIMEOUT_MS = 3000;
const STATUS_HEARTBEAT_TIMEOUT_MS = 5000; // Detect stale connection if no status for 5 seconds

interface PendingRelayAck {
  desiredState: boolean;
  resolve: () => void;
  reject: (error: Error) => void;
  timeoutId: ReturnType<typeof setTimeout>;
}

export const createTltbBleSession = (
  handlers: BleSessionHandlers,
  options: SessionOptions = {},
): BleSession => {
  const manager = new BleManager();
  let activeDevice: Device | null = null;
  let statusSubscription: Subscription | null = null;
  let disconnectSubscription: Subscription | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  let rssiTimer: ReturnType<typeof setInterval> | null = null;
  let heartbeatTimer: ReturnType<typeof setTimeout> | null = null;
  let lastStatusTimestamp: number = 0;
  let stopped = false;
  let preferredDeviceId = options.initialKnownDevice?.id ?? null;
  let rememberedDevice = options.initialKnownDevice ?? null;
  const pendingRelayAcks = new Map<RelayId, PendingRelayAck>();

  const resolvePendingRelayAck = (relayId: RelayId) => {
    const pending = pendingRelayAcks.get(relayId);
    if (!pending) {
      return;
    }
    clearTimeout(pending.timeoutId);
    pendingRelayAcks.delete(relayId);
    pending.resolve();
  };

  const rejectPendingRelayAck = (relayId: RelayId, error: Error) => {
    const pending = pendingRelayAcks.get(relayId);
    if (!pending) {
      return;
    }
    clearTimeout(pending.timeoutId);
    pendingRelayAcks.delete(relayId);
    pending.reject(error);
  };

  const rejectAllPendingRelayAcks = (error: Error) => {
    pendingRelayAcks.forEach((_, relayId) => {
      rejectPendingRelayAck(relayId, error);
    });
  };

  const waitForRelayAck = (relayId: RelayId, desiredState: boolean) => {
    if (pendingRelayAcks.has(relayId)) {
      rejectPendingRelayAck(relayId, new Error('Relay command superseded'));
    }

    let resolveAck: () => void;
    let rejectAck: (error: Error) => void;
    const promise = new Promise<void>((resolve, reject) => {
      resolveAck = resolve;
      rejectAck = reject;
    });

    const timeoutId = setTimeout(() => {
      rejectPendingRelayAck(relayId, new Error('Relay command timed out'));
    }, RELAY_ACK_TIMEOUT_MS);

    pendingRelayAcks.set(relayId, {
      desiredState,
      resolve: resolveAck!,
      reject: rejectAck!,
      timeoutId,
    });

    return promise;
  };

  const stopRssiMonitor = () => {
    if (rssiTimer) {
      clearInterval(rssiTimer);
      rssiTimer = null;
    }
  };stopHeartbeatMonitor = () => {
    if (heartbeatTimer) {
      clearTimeout(heartbeatTimer);
      heartbeatTimer = null;
    }
  };

  const startHeartbeatMonitor = () => {
    stopHeartbeatMonitor();
    
    const checkHeartbeat = () => {
      const now = Date.now();
      if (lastStatusTimestamp > 0 && now - lastStatusTimestamp > STATUS_HEARTBEAT_TIMEOUT_MS) {
        console.warn('[BLE] Status heartbeat timeout - connection may be stale');
        // Connection is stale, trigger reconnect
        handlers.onConnectionChange('disconnected');
        cleanupDevice().finally(scheduleReconnect);
        return;
      }
      
      // Check again in STATUS_HEARTBEAT_TIMEOUT_MS
      heartbeatTimer = setTimeout(checkHeartbeat, STATUS_HEARTBEAT_TIMEOUT_MS);
    };
    
    // Start monitoring
    heartbeatTimer = setTimeout(checkHeartbeat, STATUS_HEARTBEAT_TIMEOUT_MS);
  };

  const clearReconnectTimer = () => {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  };

  const cleanupDevice = async () => {
    stopRssiMonitor();
    stopHeartbeatMonitor();
    lastStatusTimestamp = 0
  const cleanupDevice = async () => {
    stopRssiMonitor();
    statusSubscription?.remove();
    statusSubscription = null;
    disconnectSubscription?.remove();
    disconnectSubscription = null;
    rejectAllPendingRelayAcks(new Error('BLE device disconnected'));

    if (activeDevice) {
      try {
        await manager.cancelDeviceConnection(activeDevice.id);
      } catch (error) {
        console.warn('[BLE] cancelDeviceConnection failed', error);
      }
    }
    activeDevice = null;
  };

  const scheduleReconnect = () => {
    if (stopped) {
      return;
    }
    if (reconnectTimer) {
      return;
    }
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      startScan();
    }, appConfig.autoReconnectMs);
  };

  const emitKnownDevice = (device: Device) => {
    const info: KnownDevice = {
      id: device.id,
      name: device.name ?? rememberedDevice?.name ?? null,
      lastRssi: rememberedDevice?.lastRssi ?? null,
      lastSeenTs: Date.now(),
    };

    rememberedDevice = info;
    preferredDeviceId = info.id;
    options.onKnownDevice?.(info);
  };

  const startRssiMonitor = (device: Device) => {
    stopRssiMonitor();
    rssiTimer = setInterval(async () => {
      try {
        const updated = await device.readRSSI();
        const rssi = typeof updated.rssi === 'number' ? updated.rssi : null;
        if (rssi !== null) {
          rememberedDevice = {
            ...(rememberedDevice ?? {
              id: device.id,
              name: device.name ?? null,
        // Update heartbeat timestamp
        lastStatusTimestamp = Date.now();

              lastSeenTs: Date.now(),
            }),
            lastRssi: rssi,
            lastSeenTs: Date.now(),
          };
          options.onSignalUpdate?.(rssi);
        }
      } catch (error) {
        console.debug('[BLE] RSSI read failed', error);
      }
    }, RSSI_INTERVAL_MS);
  };

  const monitorStatus = (device: Device) => {
    console.log('[BLE] Setting up status monitor...');
    statusSubscription = device.monitorCharacteristicForService(
      bleProfile.serviceUuid,
      bleProfile.statusCharacteristicUuid,
      (error, characteristic) => {
        if (error) {
          console.error('[BLE] Status monitor error:', error);
          console.error('[BLE] Error details:', JSON.stringify(error, null, 2));
          handlers.onConnectionChange('disconnected');
          cleanupDevice().finally(scheduleReconnect);
          return;
        }

        if (!characteristic?.value) {
          console.log('[BLE] Received empty characteristic update');
          return;
        }

        console.log('[BLE] Received status notification, length:', characteristic.value.length);
        const parsed = parseStatusNotification(characteristic.value);
        if (!parsed) {
          console.warn('[BLE] Failed to parse status notification');
          return;
        }
        console.log('[BLE] Parsed status successfully:', parsed.snapshot.mode, parsed.snapshot.activeLabel);

        handlers.onStatus(parsed.snapshot);
      startHeartbeatMonitor(); // Monitor connection health
        Object.entries(parsed.relayStates).forEach(([id, isOn]) => {
          if (typeof isOn === 'boolean') {
            const relayId = id as RelayId;
            const pending = pendingRelayAcks.get(relayId);
            if (pending && pending.desiredState === isOn) {
              resolvePendingRelayAck(relayId);
            }
            handlers.onRelayState({ id: id as RelayId, isOn });
          }
        });
      },
    );
  };

  const connectToDevice = async (device: Device) => {
    try {
      const connected = await device.connect();
      await connected.discoverAllServicesAndCharacteristics();
      
      // Request larger MTU for sending larger command payloads and receiving status notifications
      // iOS ignores this (always uses 185), Android will negotiate up to 512
      try {
        const mtuResult = await connected.requestMTU(512);
        console.log('[BLE] MTU negotiated:', mtuResult);
      } catch (mtuError) {
        console.warn('[BLE] MTU negotiation failed (may not be supported on iOS):', mtuError);
      }
      
      activeDevice = connected;
      handlers.onConnectionChange('connected');

      emitKnownDevice(connected);
      startRssiMonitor(connected);

      disconnectSubscription = manager.onDeviceDisconnected(connected.id, () => {
        handlers.onConnectionChange('disconnected');
        cleanupDevice().finally(scheduleReconnect);
      });

      monitorStatus(connected);
      
      // CRITICAL: Request immediate state sync after connection established
      // This ensures relay states are always accurate, even after reconnect
      console.log('[BLE] Requesting state sync after connection...');
      setTimeout(() => {
        sendCommand({ type: 'refresh' })
          .then(() => console.log('[BLE] State sync requested successfully'))
          .catch((error) => console.warn('[BLE] State sync request failed:', error));
      }, 100); // Small delay to ensure monitor is ready
    } catch (error) {
      console.warn('[BLE] Failed to connect', error);
      handlers.onConnectionChange('disconnected');
      cleanupDevice().finally(scheduleReconnect);
    }
  };

  const startScan = () => {
    if (stopped) {
      return;
    }

    clearReconnectTimer();
    handlers.onConnectionChange('connecting');

    manager.startDeviceScan([bleProfile.serviceUuid], null, (error, device) => {
      if (error) {
        console.warn('[BLE] Scan error', error);
        manager.stopDeviceScan();
        scheduleReconnect();
        return;
      }

      if (!device) {
        return;
      }

      const matchById = preferredDeviceId ? device.id === preferredDeviceId : false;
      const matchByName = matchesPreferredName(device);
      const matchByService = device.serviceUUIDs?.includes(bleProfile.serviceUuid);
      if (!matchById && !matchByName && !matchByService) {
        return;
      }

      manager.stopDeviceScan();
      connectToDevice(device);
    });
  };

  const sendCommand = async (command: Record<string, unknown>) => {
    if (!activeDevice) {
      throw new Error('No active BLE device');
    }

    const payload = encodeJson(command);
    await activeDevice.writeCharacteristicWithoutResponseForService(
      bleProfile.serviceUuid,
      bleProfile.controlCharacteristicUuid,
      payload,
    );
  };

  startScan();

  return {
    setRelayState: async (id, desiredState) => {
      handlers.onRelayState({ id, isOn: desiredState });
      const ackPromise = waitForRelayAck(id, desiredState);
      try {
        await sendCommand({ type: 'relay', relayId: id, state: desiredState });
        await ackPromise;
      } catch (error) {
        console.warn('[BLE] Failed to send relay command', error);
        rejectPendingRelayAck(
          id,
          error instanceof Error ? error : new Error('Relay command failed'),
        );
        await ackPromise.catch(() => undefined);
        handlers.onRelayState({ id, isOn: !desiredState });
        throw error;
      }
    },
    refresh: () => {
      if (!activeDevice) {
        startScan();
        return;
      }

      sendCommand({ type: 'refresh' }).catch((error) => console.warn('[BLE] Refresh failed', error));
    },
    stop: () => {
      stopped = true;
      clearReconnectTimer();
      manager.stopDeviceScan();
      rejectAllPendingRelayAcks(new Error('BLE transport stopped'));
      cleanupDevice().finally(() => manager.destroy());
    },
  };
};
