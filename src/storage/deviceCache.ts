import AsyncStorage from '@react-native-async-storage/async-storage';

import { KnownDevice } from '@/state/deviceStore';

const STORAGE_KEY = 'tltb:lastKnownDevice';

const sanitizeDevice = (raw: Partial<KnownDevice>): KnownDevice | null => {
  if (!raw || typeof raw.id !== 'string') {
    return null;
  }

  return {
    id: raw.id,
    name: raw.name ?? null,
    lastRssi: typeof raw.lastRssi === 'number' ? raw.lastRssi : null,
    lastSeenTs: typeof raw.lastSeenTs === 'number' ? raw.lastSeenTs : Date.now(),
  };
};

export const loadKnownDevice = async (): Promise<KnownDevice | null> => {
  try {
    const raw = await AsyncStorage.getItem(STORAGE_KEY);
    if (!raw) {
      return null;
    }

    const parsed = JSON.parse(raw) as Partial<KnownDevice>;
    return sanitizeDevice(parsed);
  } catch (error) {
    console.warn('[BLE] Failed to load cached device', error);
    return null;
  }
};

export const saveKnownDevice = async (device: KnownDevice | null): Promise<void> => {
  try {
    if (!device) {
      await AsyncStorage.removeItem(STORAGE_KEY);
      return;
    }

    await AsyncStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({
        ...device,
        lastSeenTs: device.lastSeenTs ?? Date.now(),
      }),
    );
  } catch (error) {
    console.warn('[BLE] Failed to persist device', error);
  }
};
