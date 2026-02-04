import { useEffect, useState } from 'react';

import { loadKnownDevice } from '@/storage/deviceCache';
import { useDeviceStore } from '@/state/deviceStore';

export const useKnownDeviceBootstrap = () => {
  const setKnownDevice = useDeviceStore((state) => state.setKnownDevice);
  const [hydrated, setHydrated] = useState(false);

  useEffect(() => {
    let cancelled = false;

    const hydrate = async () => {
      const device = await loadKnownDevice();
      if (cancelled) {
        return;
      }

      if (device) {
        setKnownDevice(device);
      }
      setHydrated(true);
    };

    hydrate();

    return () => {
      cancelled = true;
    };
  }, [setKnownDevice]);

  return hydrated;
};
