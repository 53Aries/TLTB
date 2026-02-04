export interface BleProfile {
  serviceUuid: string;
  statusCharacteristicUuid: string;
  controlCharacteristicUuid: string;
  preferredDeviceNamePrefix: string;
}

export const bleProfile: BleProfile = {
  // TODO: update these UUIDs once firmware publishes the definitive GATT profile.
  serviceUuid: '0000a11c-0000-1000-8000-00805f9b34fb',
  statusCharacteristicUuid: '0000a11d-0000-1000-8000-00805f9b34fb',
  controlCharacteristicUuid: '0000a11e-0000-1000-8000-00805f9b34fb',
  preferredDeviceNamePrefix: 'TLTB',
};
