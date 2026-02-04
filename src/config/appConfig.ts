const parseBoolean = (value: string | undefined, fallback: boolean) => {
  if (value === undefined) {
    return fallback;
  }

  const normalized = value.trim().toLowerCase();
  if (['1', 'true', 'yes', 'on'].includes(normalized)) {
    return true;
  }

  if (['0', 'false', 'no', 'off'].includes(normalized)) {
    return false;
  }

  return fallback;
};

const parseNumber = (value: string | undefined, fallback: number) => {
  if (value === undefined) {
    return fallback;
  }

  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
};

export const appConfig = {
  useMockBleTransport: parseBoolean(process.env.EXPO_PUBLIC_USE_MOCK_BLE, true),
  deviceNamePrefix: process.env.EXPO_PUBLIC_BLE_DEVICE_PREFIX ?? 'TLTB',
  autoReconnectMs: parseNumber(process.env.EXPO_PUBLIC_BLE_RECONNECT_MS, 4000),
};
