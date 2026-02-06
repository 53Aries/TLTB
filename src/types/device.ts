export type RelayId =
  | 'relay-left'
  | 'relay-right'
  | 'relay-brake'
  | 'relay-tail'
  | 'relay-marker'
  | 'relay-aux';

export interface RelayChannel {
  id: RelayId;
  label: string;
  description: string;
}

export interface RelayStatus extends RelayChannel {
  isOn: boolean;
}

export type RelayStateMap = Partial<Record<RelayId, boolean>>;

export type ConnectionState = 'disconnected' | 'connecting' | 'connected';

export type ControllerMode = 'HD' | 'RV';

export interface HomeStatusSnapshot {
  mode: ControllerMode;
  loadAmps: number | null;
  activeLabel: string;
  twelveVoltEnabled: boolean;
  srcVoltage: number | null;
  outVoltage: number | null;
  lvpLatched: boolean;
  lvpBypass: boolean;
  outvLatched: boolean;
  outvBypass: boolean;
  cooldownActive: boolean;
  cooldownSecsRemaining: number;
  startupGuard: boolean;
  faultMask: number;
  faultMessages: string[];
  timestamp: number;
}

export interface BleStatusPayload extends Partial<HomeStatusSnapshot> {
  relayMask?: number;
  statusFlags?: number;
  relays?: RelayStateMap;
}
