import { useEffect } from 'react';

import { createMockBleSession } from '@/services/ble/mockBleSession';
import { useDeviceStore } from '@/state/deviceStore';

interface UseMockOptions {
  enabled?: boolean;
}

export const useMockBleSession = ({ enabled = true }: UseMockOptions = {}) => {
  const setConnectionState = useDeviceStore((state) => state.setConnectionState);
  const setHomeStatus = useDeviceStore((state) => state.setHomeStatus);
  const setRelayState = useDeviceStore((state) => state.setRelayState);
  const registerTransport = useDeviceStore((state) => state.registerTransport);

  useEffect(() => {
    if (!enabled) {
      return;
    }

    const session = createMockBleSession({
      onConnectionChange: setConnectionState,
      onStatus: setHomeStatus,
      onRelayState: setRelayState,
    });

    registerTransport({
      setRelayState: session.setRelayState,
      refresh: session.refresh,
    });

    return () => {
      registerTransport(null);
      session.stop();
    };
  }, [enabled, registerTransport, setConnectionState, setHomeStatus, setRelayState]);
};
