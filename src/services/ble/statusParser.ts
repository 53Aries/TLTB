import { decode as base64Decode } from 'base-64';

import { RELAY_CHANNELS } from '@/constants/relays';
import { buildFaultMessages } from '@/services/ble/faults';
import { BleStatusPayload, ControllerMode, HomeStatusSnapshot, RelayId, RelayStateMap } from '@/types/device';

const relayIds = RELAY_CHANNELS.map((relay) => relay.id);

const STATUS_FLAGS = {
  twelveVoltEnabled: 1 << 0,
  lvpLatched: 1 << 1,
  lvpBypass: 1 << 2,
  outvLatched: 1 << 3,
  outvBypass: 1 << 4,
  cooldownActive: 1 << 5,
  startupGuard: 1 << 6,
} as const;

type RawRelayMap = RelayStateMap | Record<string, boolean>;

type RawStatusPayload = Partial<BleStatusPayload> & { [key: string]: unknown };

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

const coerceRelayStates = (value: unknown): RelayStateMap => {
  if (typeof value !== 'object' || value === null) {
    return {};
  }

  const record: RelayStateMap = {};
  for (const relayId of relayIds) {
    if (Object.prototype.hasOwnProperty.call(value, relayId)) {
      record[relayId] = coerceBoolean((value as RawRelayMap)[relayId]);
    }
  }

  return record;
};

const coerceStatusFlags = (value: unknown): number | null => {
  const num = coerceNumber(value);
  if (num === null) {
    return null;
  }
  return Math.max(0, Math.floor(num));
};

const deriveFlaggedBoolean = (flags: number | null, bit: number, fallback: boolean): boolean =>
  flags !== null ? (flags & bit) !== 0 : fallback;

const decodeRelayMask = (value: unknown): RelayStateMap | null => {
  const mask = coerceNumber(value);
  if (mask === null) {
    return null;
  }

  const normalized = mask & ((1 << relayIds.length) - 1);
  const record: RelayStateMap = {};
  relayIds.forEach((relayId, index) => {
    record[relayId] = (normalized & (1 << index)) !== 0;
  });

  return record;
};

export interface ParsedStatusNotification {
  snapshot: HomeStatusSnapshot;
  relayStates: RelayStateMap;
}

export const parseStatusNotification = (payload: string): ParsedStatusNotification | null => {
  if (!payload) {
    return null;
  }

  try {
    const decoded = base64Decode(payload);
    const parsed = JSON.parse(decoded) as RawStatusPayload;

    const faultMask = coerceNumber(parsed.faultMask) ?? 0;
    const statusFlags = coerceStatusFlags(parsed.statusFlags);
    const fallbackTwelveVoltEnabled = coerceBoolean(parsed.twelveVoltEnabled, true);
    const fallbackLvpLatched = coerceBoolean(parsed.lvpLatched, false);
    const fallbackLvpBypass = coerceBoolean(parsed.lvpBypass, false);
    const fallbackOutvLatched = coerceBoolean(parsed.outvLatched, false);
    const fallbackOutvBypass = coerceBoolean(parsed.outvBypass, false);
    const fallbackCooldownActive = coerceBoolean(parsed.cooldownActive, false);
    const fallbackStartupGuard = coerceBoolean(parsed.startupGuard, false);
    const snapshot: HomeStatusSnapshot = {
      mode: coerceMode(parsed.mode),
      loadAmps: coerceNumber(parsed.loadAmps),
      activeLabel: typeof parsed.activeLabel === 'string' ? parsed.activeLabel : 'OFF',
      twelveVoltEnabled: deriveFlaggedBoolean(statusFlags, STATUS_FLAGS.twelveVoltEnabled, fallbackTwelveVoltEnabled),
      srcVoltage: coerceNumber(parsed.srcVoltage),
      outVoltage: coerceNumber(parsed.outVoltage),
      lvpLatched: deriveFlaggedBoolean(statusFlags, STATUS_FLAGS.lvpLatched, fallbackLvpLatched),
      lvpBypass: deriveFlaggedBoolean(statusFlags, STATUS_FLAGS.lvpBypass, fallbackLvpBypass),
      outvLatched: deriveFlaggedBoolean(statusFlags, STATUS_FLAGS.outvLatched, fallbackOutvLatched),
      outvBypass: deriveFlaggedBoolean(statusFlags, STATUS_FLAGS.outvBypass, fallbackOutvBypass),
      cooldownActive: deriveFlaggedBoolean(statusFlags, STATUS_FLAGS.cooldownActive, fallbackCooldownActive),
      cooldownSecsRemaining: coerceNumber(parsed.cooldownSecsRemaining) ?? 0,
      startupGuard: deriveFlaggedBoolean(statusFlags, STATUS_FLAGS.startupGuard, fallbackStartupGuard),
      faultMask,
      faultMessages: buildFaultMessages(faultMask, Array.isArray(parsed.faultMessages) ? (parsed.faultMessages as string[]) : undefined),
      timestamp: coerceNumber(parsed.timestamp) ?? Date.now(),
    };

    const relayStates = decodeRelayMask(parsed.relayMask) ?? coerceRelayStates(parsed.relays);

    return {
      snapshot,
      relayStates,
    };
  } catch (error) {
    console.warn('[BLE] Failed to parse status payload', error);
    return null;
  }
};
