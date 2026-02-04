import { create } from 'zustand';

import { RELAY_CHANNELS } from '@/constants/relays';
import { TransportControls } from '@/services/ble/types';
import { ConnectionState, HomeStatusSnapshot, RelayId, RelayStatus } from '@/types/device';

type RelayMap = Record<RelayId, RelayStatus>;

export interface KnownDevice {
  id: string;
  name?: string | null;
  lastRssi?: number | null;
  lastSeenTs: number;
}

interface DeviceStore {
  connectionState: ConnectionState;
  relays: RelayMap;
  homeStatus: HomeStatusSnapshot | null;
  lastUpdated: number | null;
  transport: TransportControls | null;
  knownDevice: KnownDevice | null;
  setConnectionState: (state: ConnectionState) => void;
  setHomeStatus: (snapshot: HomeStatusSnapshot) => void;
  setRelayState: (payload: { id: RelayId; isOn: boolean }) => void;
  setKnownDevice: (device: KnownDevice | null) => void;
  updateKnownDeviceSignal: (rssi: number | null) => void;
  registerTransport: (controls: TransportControls | null) => void;
  requestRelayToggle: (id: RelayId) => Promise<void>;
}

const buildRelayMap = (): RelayMap =>
  RELAY_CHANNELS.reduce<RelayMap>((acc, channel) => {
    acc[channel.id] = { ...channel, isOn: false };
    return acc;
  }, {} as RelayMap);

export const useDeviceStore = create<DeviceStore>((set, get) => ({
  connectionState: 'disconnected',
  relays: buildRelayMap(),
  homeStatus: null,
  lastUpdated: null,
  transport: null,
  knownDevice: null,
  setConnectionState: (connectionState) => set({ connectionState }),
  setHomeStatus: (homeStatus) => set({ homeStatus, lastUpdated: homeStatus.timestamp }),
  setRelayState: ({ id, isOn }) =>
    set((state) => ({
      relays: {
        ...state.relays,
        [id]: state.relays[id] ? { ...state.relays[id], isOn } : { id, label: id, description: '', isOn },
      },
    })),
  setKnownDevice: (device) =>
    set({
      knownDevice: device
        ? {
            ...device,
            lastSeenTs: device.lastSeenTs ?? Date.now(),
          }
        : null,
    }),
  updateKnownDeviceSignal: (rssi) =>
    set((state) => {
      if (!state.knownDevice) {
        return {} as Partial<DeviceStore>;
      }

      return {
        knownDevice: {
          ...state.knownDevice,
          lastRssi: typeof rssi === 'number' ? rssi : null,
          lastSeenTs: Date.now(),
        },
      };
    }),
  registerTransport: (controls) => set({ transport: controls }),
  requestRelayToggle: async (id) => {
    const state = get();
    const current = state.relays[id]?.isOn ?? false;
    const next = !current;

    state.setRelayState({ id, isOn: next });

    if (!state.transport) {
      return;
    }

    try {
      await state.transport.setRelayState(id, next);
    } catch (error) {
      state.setRelayState({ id, isOn: current });
      throw error;
    }
  },
}));
