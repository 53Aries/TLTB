import { beforeEach, describe, expect, it, vi } from 'vitest';

import { RELAY_CHANNELS } from '@/constants/relays';
import { useDeviceStore } from '@/state/deviceStore';
import { RelayId, RelayStatus } from '@/types/device';
import { TransportControls } from '@/services/ble/types';

const buildRelayMap = () =>
  RELAY_CHANNELS.reduce<Record<RelayId, RelayStatus>>((acc, relay) => {
    acc[relay.id] = { ...relay, isOn: false };
    return acc;
  }, {} as Record<RelayId, RelayStatus>);

const resetStore = () => {
  useDeviceStore.setState({
    connectionState: 'disconnected',
    relays: buildRelayMap(),
    homeStatus: null,
    lastUpdated: null,
    transport: null,
    knownDevice: null,
    commandError: null,
  });
};

describe('useDeviceStore.requestRelayToggle', () => {
  beforeEach(() => {
    resetStore();
  });

  it('invokes the registered transport and keeps optimistic state on success', async () => {
    const transport: TransportControls = {
      setRelayState: vi.fn().mockResolvedValue(undefined),
      refresh: vi.fn(),
    };

    useDeviceStore.getState().registerTransport(transport);

    await useDeviceStore.getState().requestRelayToggle('relay-left');

    expect(transport.setRelayState).toHaveBeenCalledWith('relay-left', true);
    expect(useDeviceStore.getState().relays['relay-left'].isOn).toBe(true);
  });

  it('rolls back the optimistic toggle when the transport rejects', async () => {
    const transport: TransportControls = {
      setRelayState: vi.fn().mockRejectedValue(new Error('relay failed')),
      refresh: vi.fn(),
    };

    useDeviceStore.getState().registerTransport(transport);

    await expect(useDeviceStore.getState().requestRelayToggle('relay-left')).rejects.toThrow(
      'relay failed',
    );

    expect(transport.setRelayState).toHaveBeenCalledWith('relay-left', true);
    expect(useDeviceStore.getState().relays['relay-left'].isOn).toBe(false);
    expect(useDeviceStore.getState().commandError).toMatchObject({
      relayId: 'relay-left',
      relayLabel: 'Left Turn',
    });
  });

  it('skips transport calls when none is registered', async () => {
    await useDeviceStore.getState().requestRelayToggle('relay-left');
    expect(useDeviceStore.getState().relays['relay-left'].isOn).toBe(true);
    expect(useDeviceStore.getState().commandError).toBeNull();
  });
});
