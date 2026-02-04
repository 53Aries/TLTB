import { useCallback, useEffect, useRef } from 'react';

import { createTltbBleSession } from '@/services/ble/tltbBleSession';
import { saveKnownDevice } from '@/storage/deviceCache';
import { KnownDevice, useDeviceStore } from '@/state/deviceStore';

interface UseRealBleOptions {
  enabled?: boolean;
}

export const useTltbBleSession = ({ enabled = true }: UseRealBleOptions = {}) => {
  const setConnectionState = useDeviceStore((state) => state.setConnectionState);
  const setHomeStatus = useDeviceStore((state) => state.setHomeStatus);
  const setRelayState = useDeviceStore((state) => state.setRelayState);
  const registerTransport = useDeviceStore((state) => state.registerTransport);
  const setKnownDevice = useDeviceStore((state) => state.setKnownDevice);
  const updateKnownDeviceSignal = useDeviceStore((state) => state.updateKnownDeviceSignal);
  const knownDevice = useDeviceStore((state) => state.knownDevice);

  const initialDeviceRef = useRef<KnownDevice | null>(knownDevice);

  useEffect(() => {
    if (!initialDeviceRef.current && knownDevice) {
      initialDeviceRef.current = knownDevice;
    }
  }, [knownDevice]);

  const handleKnownDevice = useCallback(
    (device: KnownDevice) => {
      setKnownDevice(device);
      saveKnownDevice(device);
    },
    [setKnownDevice],
  );

  const handleSignalUpdate = useCallback(
    (rssi: number | null) => {
      updateKnownDeviceSignal(rssi);
      const latest = useDeviceStore.getState().knownDevice;
      if (latest) {
        saveKnownDevice(latest);
      }
    },
    [updateKnownDeviceSignal],
  );

  useEffect(() => {
    if (!enabled) {
      return;
    }

    const session = createTltbBleSession(
      {
        onConnectionChange: setConnectionState,
        onStatus: setHomeStatus,
        onRelayState: setRelayState,
      },
      {
        initialKnownDevice: initialDeviceRef.current ?? undefined,
        onKnownDevice: handleKnownDevice,
        onSignalUpdate: handleSignalUpdate,
      },
    );

    registerTransport({
      setRelayState: session.setRelayState,
      refresh: session.refresh,
    });

    return () => {
      registerTransport(null);
      session.stop();
    };
  }, [
    enabled,
    handleKnownDevice,
    handleSignalUpdate,
    registerTransport,
    setConnectionState,
    setHomeStatus,
    setRelayState,
  ]);
};
