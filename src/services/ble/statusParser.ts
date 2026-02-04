import { decode as base64Decode } from 'base-64';

import { RELAY_CHANNELS } from '@/constants/relays';
import { buildFaultMessages } from '@/services/ble/faults';
import { ControllerMode, HomeStatusSnapshot, RelayId } from '@/types/device';

const relayIds = RELAY_CHANNELS.map((relay) => relay.id);

type RawRelayMap = Partial<Record<RelayId, boolean>> | Record<string, boolean>;

type RawStatusPayload = Partial<
  HomeStatusSnapshot & {
    relays?: RawRelayMap;
    timestamp?: number;
  }
> & { [key: string]: unknown };

const coerceBoolean = (value: unknown, fallback = false): boolean => {
  if (typeof value === 'boolean') {
    return value;
  }

  if (typeof value === 'string') {
    const normalized = value.trim().toLowerCase();
    if (['1', 'true', 'yes', 'on'].includes(normalized)) {
      return true;
    }
    if (['0', 'false', 'no', 'off'].includes(normalized)) {
      return false;
    }
  }

  if (typeof value === 'number') {
    return value !== 0;
  }

  return fallback;
};

const coerceNumber = (value: unknown): number | null => {
  if (value === null || value === undefined) {
    return null;
  }
  const num = Number(value);
  return Number.isFinite(num) ? num : null;
};

const coerceMode = (value: unknown): ControllerMode => (value === 'RV' ? 'RV' : 'HD');

const coerceRelayStates = (value: unknown): Partial<Record<RelayId, boolean>> => {
  if (typeof value !== 'object' || value === null) {
    return {};
  }

  const record: Partial<Record<RelayId, boolean>> = {};
  for (const relayId of relayIds) {
    if (Object.prototype.hasOwnProperty.call(value, relayId)) {
      record[relayId] = coerceBoolean((value as RawRelayMap)[relayId]);
    }
  }

  return record;
};

export interface ParsedStatusNotification {
  snapshot: HomeStatusSnapshot;
  relayStates: Partial<Record<RelayId, boolean>>;
}

export const parseStatusNotification = (payload: string): ParsedStatusNotification | null => {
  if (!payload) {
    return null;
  }

  try {
    const decoded = base64Decode(payload);
    const parsed = JSON.parse(decoded) as RawStatusPayload;

    const faultMask = parsed.faultMask ?? 0;
    const snapshot: HomeStatusSnapshot = {
      mode: coerceMode(parsed.mode),
      loadAmps: coerceNumber(parsed.loadAmps),
      activeLabel: typeof parsed.activeLabel === 'string' ? parsed.activeLabel : 'OFF',
      twelveVoltEnabled: coerceBoolean(parsed.twelveVoltEnabled, true),
      srcVoltage: coerceNumber(parsed.srcVoltage),
      outVoltage: coerceNumber(parsed.outVoltage),
      lvpLatched: coerceBoolean(parsed.lvpLatched, false),
      lvpBypass: coerceBoolean(parsed.lvpBypass, false),
      outvLatched: coerceBoolean(parsed.outvLatched, false),
      outvBypass: coerceBoolean(parsed.outvBypass, false),
      cooldownActive: coerceBoolean(parsed.cooldownActive, false),
      cooldownSecsRemaining: coerceNumber(parsed.cooldownSecsRemaining) ?? 0,
      startupGuard: coerceBoolean(parsed.startupGuard, false),
      faultMask,
      faultMessages: buildFaultMessages(faultMask, Array.isArray(parsed.faultMessages) ? (parsed.faultMessages as string[]) : undefined),
      timestamp: coerceNumber(parsed.timestamp) ?? Date.now(),
    };

    return {
      snapshot,
      relayStates: coerceRelayStates(parsed.relays),
    };
  } catch (error) {
    console.warn('[BLE] Failed to parse status payload', error);
    return null;
  }
};
