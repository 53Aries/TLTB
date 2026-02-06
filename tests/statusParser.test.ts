import { describe, it, expect } from 'vitest';
import { encode as base64Encode } from 'base-64';

import { parseStatusNotification } from '@/services/ble/statusParser';

const encodePayload = (payload: Record<string, unknown>) => base64Encode(JSON.stringify(payload));

describe('parseStatusNotification', () => {
  it('parses valid payloads and relay states', () => {
    const statusFlags = (1 << 0) | (1 << 2) | (1 << 6);
    const payload = {
      mode: 'RV',
      loadAmps: 14.2,
      activeLabel: 'LEFT',
      srcVoltage: 12.6,
      outVoltage: 12.3,
      cooldownSecsRemaining: 0,
      faultMask: 0,
      faultMessages: [],
      timestamp: 1700000000000,
      statusFlags,
      relayMask: 0b010101,
    };

    const result = parseStatusNotification(encodePayload(payload));
    expect(result).not.toBeNull();
    expect(result?.snapshot.mode).toBe('RV');
    expect(result?.snapshot.loadAmps).toBe(14.2);
    expect(result?.snapshot.timestamp).toBe(1700000000000);
    expect(result?.snapshot.twelveVoltEnabled).toBe(true);
    expect(result?.snapshot.lvpBypass).toBe(true);
    expect(result?.snapshot.startupGuard).toBe(true);
    expect(result?.relayStates).toEqual({
      'relay-left': true,
      'relay-right': false,
      'relay-brake': true,
      'relay-tail': false,
      'relay-marker': true,
      'relay-aux': false,
    });
  });

  it('coerces primitive types and ignores unknown relays', () => {
    const payload = {
      mode: 'HD',
      loadAmps: '10.4',
      twelveVoltEnabled: 'true',
      srcVoltage: '12.1',
      outVoltage: '11.9',
      cooldownSecsRemaining: '9',
      faultMask: '2',
      relays: {
        'relay-left': '1',
        'relay-right': '0',
        'relay-marker': 'true',
        'relay-custom': true,
      },
      timestamp: undefined,
    } as unknown as Record<string, unknown>;

    const result = parseStatusNotification(encodePayload(payload));
    expect(result).not.toBeNull();
    expect(result?.snapshot.loadAmps).toBe(10.4);
    expect(result?.snapshot.twelveVoltEnabled).toBe(true);
    expect(result?.snapshot.cooldownSecsRemaining).toBe(9);
    expect(result?.snapshot.faultMask).toBe(2);
    expect(result?.relayStates).toEqual({
      'relay-left': true,
      'relay-right': false,
      'relay-marker': true,
    });
  });

  it('returns null for invalid payloads', () => {
    expect(parseStatusNotification('')).toBeNull();
    expect(parseStatusNotification('not-base64')).toBeNull();
  });
});
