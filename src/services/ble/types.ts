import { ConnectionState, HomeStatusSnapshot, RelayId } from '@/types/device';

export interface BleSessionHandlers {
  onConnectionChange: (state: ConnectionState) => void;
  onStatus: (snapshot: HomeStatusSnapshot) => void;
  onRelayState: (payload: { id: RelayId; isOn: boolean }) => void;
}

export interface BleSession {
  setRelayState: (id: RelayId, desiredState: boolean) => Promise<void>;
  refresh: () => void;
  stop: () => void;
}

export interface TransportControls {
  setRelayState: (id: RelayId, desiredState: boolean) => Promise<void>;
  refresh: () => void;
}
