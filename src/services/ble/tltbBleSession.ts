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

const RSSI_INTERVAL_MS = 4000;

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
  let stopped = false;
  let preferredDeviceId = options.initialKnownDevice?.id ?? null;
  let rememberedDevice = options.initialKnownDevice ?? null;

  const stopRssiMonitor = () => {
    if (rssiTimer) {
      clearInterval(rssiTimer);
      rssiTimer = null;
    }
  };

  const clearReconnectTimer = () => {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  };

  const cleanupDevice = async () => {
    stopRssiMonitor();
    statusSubscription?.remove();
    statusSubscription = null;
    disconnectSubscription?.remove();
    disconnectSubscription = null;

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
    statusSubscription = device.monitorCharacteristicForService(
      bleProfile.serviceUuid,
      bleProfile.statusCharacteristicUuid,
      (error, characteristic) => {
        if (error) {
          console.warn('[BLE] Status monitor error', error);
          handlers.onConnectionChange('disconnected');
          cleanupDevice().finally(scheduleReconnect);
          return;
        }

        if (!characteristic?.value) {
          return;
        }

        const parsed = parseStatusNotification(characteristic.value);
        if (!parsed) {
          return;
        }

        handlers.onStatus(parsed.snapshot);
        Object.entries(parsed.relayStates).forEach(([id, isOn]) => {
          if (typeof isOn === 'boolean') {
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
      activeDevice = connected;
      handlers.onConnectionChange('connected');

      emitKnownDevice(connected);
      startRssiMonitor(connected);

      disconnectSubscription = manager.onDeviceDisconnected(connected.id, () => {
        handlers.onConnectionChange('disconnected');
        cleanupDevice().finally(scheduleReconnect);
      });

      monitorStatus(connected);
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
      try {
        await sendCommand({ type: 'relay', relayId: id, state: desiredState });
      } catch (error) {
        console.warn('[BLE] Failed to send relay command', error);
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
      cleanupDevice().finally(() => manager.destroy());
    },
  };
};
