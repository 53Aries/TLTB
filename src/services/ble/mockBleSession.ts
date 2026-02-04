import { RELAY_CHANNELS } from '@/constants/relays';
import { ControllerMode, HomeStatusSnapshot, RelayId } from '@/types/device';

import { BleSession, BleSessionHandlers } from './types';
import { buildFaultMessages, faultCatalog } from './faults';

const randomInRange = (min: number, max: number) => Math.random() * (max - min) + min;

const delay = (ms: number) => new Promise<void>((resolve) => {
  const timeout = setTimeout(() => {
    clearTimeout(timeout);
    resolve();
  }, ms);
});

const mapRelayIdToActiveLabel = (id: RelayId, mode: ControllerMode): string => {
  switch (id) {
    case 'relay-left':
      return 'LEFT';
    case 'relay-right':
      return 'RIGHT';
    case 'relay-brake':
      return 'BRAKE';
    case 'relay-tail':
      return 'TAIL';
    case 'relay-marker':
      return mode === 'RV' ? 'REV' : 'MARK';
    case 'relay-aux':
      return mode === 'RV' ? 'Ele Brakes' : 'AUX';
    default:
      return 'OFF';
  }
};

const buildHomeSnapshot = (
  state: Record<RelayId, boolean>,
  mode: ControllerMode,
  overrides: Partial<HomeStatusSnapshot> = {},
): HomeStatusSnapshot => {
  const activeRelay = Object.entries(state).find(([, isOn]) => isOn)?.[0] as RelayId | undefined;
  const activeLabel = activeRelay ? mapRelayIdToActiveLabel(activeRelay, mode) : 'OFF';
  const relayCount = Object.values(state).filter(Boolean).length;
  const baseLoad = relayCount === 0 ? 2.4 : 6.5 + relayCount * 2.1;
  const loadAmps = Number((baseLoad + randomInRange(-0.4, 0.6)).toFixed(1));
  const srcVoltage = Number(randomInRange(11.8, 13.2).toFixed(1));
  const outVoltage = Number(randomInRange(11.5, 12.6).toFixed(1));

  const snapshot: HomeStatusSnapshot = {
    mode,
    loadAmps,
    activeLabel,
    twelveVoltEnabled: true,
    srcVoltage,
    outVoltage,
    lvpLatched: srcVoltage < 11.4,
    lvpBypass: false,
    outvLatched: outVoltage < 11.0,
    outvBypass: false,
    cooldownActive: loadAmps > 22,
    cooldownSecsRemaining: loadAmps > 22 ? 45 : Math.max(0, 20 - relayCount * 2),
    startupGuard: false,
    faultMask: 0,
    faultMessages: [],
    timestamp: Date.now(),
  };

  return { ...snapshot, ...overrides };
};

export const createMockBleSession = (handlers: BleSessionHandlers): BleSession => {
  let telemetryTimer: ReturnType<typeof setInterval> | null = null;
  let connectTimer: ReturnType<typeof setTimeout> | null = null;
  let mode: ControllerMode = 'HD';
  let lvpBypass = false;
  let outvBypass = false;
  let faultMask = 0;
  let startupGuard = false;
  let cooldownSeconds = 0;
  let tickCounter = 0;
  let twelveVoltEnabled = true;

  const relayMap = RELAY_CHANNELS.reduce<Record<RelayId, boolean>>((acc, relay) => {
    acc[relay.id] = false;
    return acc;
  }, {} as Record<RelayId, boolean>);

  const emitRelaySnapshot = (relayId?: RelayId) => {
    if (relayId) {
      handlers.onRelayState({ id: relayId, isOn: relayMap[relayId] });
      return;
    }

    RELAY_CHANNELS.forEach((relay) => {
      handlers.onRelayState({ id: relay.id, isOn: relayMap[relay.id] });
    });
  };

  const emitStatus = () => {
    tickCounter += 1;

    if (tickCounter % 20 === 0) {
      mode = mode === 'HD' ? 'RV' : 'HD';
    }

    if (tickCounter % 18 === 0) {
      lvpBypass = !lvpBypass && Math.random() > 0.6;
      outvBypass = !outvBypass && Math.random() > 0.7;
    }

    if (tickCounter % 12 === 0 && Math.random() > 0.7) {
      twelveVoltEnabled = !twelveVoltEnabled;
    }

    if (tickCounter % 24 === 0) {
      startupGuard = Math.random() > 0.8;
      if (startupGuard) {
        faultMask = 0;
        twelveVoltEnabled = false;
      }
    } else if (!startupGuard && tickCounter % 15 === 0) {
      faultMask = faultMask === 0 ? faultCatalog[Math.floor(Math.random() * faultCatalog.length)].bit : 0;
    }

    const relayCount = Object.values(relayMap).filter(Boolean).length;
    cooldownSeconds = Math.max(0, cooldownSeconds - 2);
    if (relayCount > 3) {
      cooldownSeconds = Math.min(60, cooldownSeconds + 6);
    }

    const snapshot = buildHomeSnapshot(relayMap, mode, {
      lvpBypass,
      outvBypass,
      faultMask,
      faultMessages: buildFaultMessages(faultMask),
      startupGuard,
      cooldownSecsRemaining: cooldownSeconds,
      twelveVoltEnabled,
    });

    handlers.onStatus(snapshot);
  };

  const startStreaming = () => {
    emitRelaySnapshot();
    emitStatus();
    telemetryTimer = setInterval(emitStatus, 2500);
  };

  const cleanupTimers = () => {
    if (telemetryTimer) {
      clearInterval(telemetryTimer);
      telemetryTimer = null;
    }

    if (connectTimer) {
      clearTimeout(connectTimer);
      connectTimer = null;
    }
  };

  const beginConnection = () => {
    handlers.onConnectionChange('connecting');
    connectTimer = setTimeout(() => {
      handlers.onConnectionChange('connected');
      startStreaming();
    }, 850);
  };

  beginConnection();

  return {
    setRelayState: async (id, desiredState) => {
      relayMap[id] = desiredState;
      emitRelaySnapshot(id);
      emitStatus();
      await delay(140);
    },
    refresh: () => {
      cleanupTimers();
      beginConnection();
    },
    stop: () => {
      cleanupTimers();
      handlers.onConnectionChange('disconnected');
    },
  };
};
