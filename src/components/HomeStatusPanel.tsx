import { memo, useMemo } from 'react';
import { StyleSheet, Text, View } from 'react-native';

import { useDeviceStore } from '@/state/deviceStore';
import { palette } from '@/theme/colors';
import { radius, spacing } from '@/theme/layout';
import { formatCurrent, formatSeconds, formatVoltage } from '@/utils/formatters';

const toneColors = {
  success: palette.success,
  warning: palette.warning,
  danger: palette.danger,
};

type Tone = keyof typeof toneColors;

interface StatusLine {
  label: string;
  value: string;
  extra?: string;
  tone: Tone;
}

const PendingCard = () => (
  <View style={styles.pendingCard}>
    <Text style={styles.pendingTitle}>Waiting for controller</Text>
    <Text style={styles.pendingSubtitle}>Power the ESP32 or stay within BLE range.</Text>
  </View>
);

const StartupGuardCard = () => (
  <View style={[styles.pendingCard, styles.guardCard]}>
    <Text style={styles.guardTitle}>Startup Guard Active</Text>
    <Text style={styles.guardBody}>Cycle OUTPUT to OFF before operating the system.</Text>
  </View>
);

const HomeStatusPanel = () => {
  const snapshot = useDeviceStore((state) => state.homeStatus);
  const lastUpdated = useDeviceStore((state) => state.lastUpdated);

  const statusLines = useMemo<StatusLine[]>(() => {
    if (!snapshot) {
      return [];
    }

    const battTone: Tone = snapshot.lvpBypass ? 'warning' : snapshot.lvpLatched ? 'danger' : 'success';
    const systemTone: Tone = snapshot.outvBypass ? 'warning' : snapshot.outvLatched ? 'danger' : 'success';
    const twelveVTone: Tone = snapshot.twelveVoltEnabled ? 'success' : 'danger';

    return [
      {
        label: '12V sys',
        value: snapshot.twelveVoltEnabled ? 'ENABLED' : 'DISABLED',
        tone: twelveVTone,
      },
      {
        label: 'Batt Volt',
        value: snapshot.lvpBypass ? 'BYPASS' : snapshot.lvpLatched ? 'ACTIVE' : 'OK',
        extra: snapshot.srcVoltage !== null ? formatVoltage(snapshot.srcVoltage) : 'N/A',
        tone: battTone,
      },
      {
        label: 'System Volt',
        value: snapshot.outvBypass ? 'BYPASS' : snapshot.outvLatched ? 'ACTIVE' : 'OK',
        extra: snapshot.outVoltage !== null ? formatVoltage(snapshot.outVoltage) : 'N/A',
        tone: systemTone,
      },
    ];
  }, [snapshot]);

  if (!snapshot) {
    return <PendingCard />;
  }

  if (snapshot.startupGuard) {
    return <StartupGuardCard />;
  }

  const loadTone: Tone = snapshot.loadAmps !== null && snapshot.loadAmps >= 20
    ? 'danger'
    : snapshot.loadAmps !== null && snapshot.loadAmps >= 15
      ? 'warning'
      : 'success';

  const cooldownCopy = snapshot.cooldownActive
    ? { label: 'Cooldown', tone: 'danger' as Tone, value: `Wait ${formatSeconds(snapshot.cooldownSecsRemaining)}` }
    : snapshot.cooldownSecsRemaining > 0
      ? { label: 'Hi-Amps Time', tone: 'warning' as Tone, value: formatSeconds(snapshot.cooldownSecsRemaining) }
      : { label: 'Cooldown', tone: 'success' as Tone, value: 'OK' };

  return (
    <View style={styles.wrapper}>
      <View style={styles.modeCard}>
        <Text style={styles.cardLabel}>Mode</Text>
        <Text style={styles.modeValue}>{snapshot.mode}</Text>
        <Text style={styles.activeLabel}>Active: {snapshot.activeLabel}</Text>
        {lastUpdated && (
          <Text style={styles.timestamp}>Updated {new Date(lastUpdated).toLocaleTimeString()}</Text>
        )}
      </View>

      <View style={styles.loadCard}>
        <Text style={styles.cardLabel}>Load</Text>
        <Text style={[styles.loadValue, { color: toneColors[loadTone] }]}>
          {snapshot.loadAmps !== null ? formatCurrent(snapshot.loadAmps) : 'N/A'}
        </Text>
        <Text style={styles.loadCaption}>
          &lt;15A safe · 15-20A caution · &gt;=20A high draw
        </Text>
      </View>

      <View style={styles.statusList}>
        {statusLines.map((line) => (
          <View key={line.label} style={styles.statusRow}>
            <Text style={styles.statusLabel}>{line.label}</Text>
            <Text style={[styles.statusValue, { color: toneColors[line.tone] }]}>{line.value}</Text>
            {line.extra && <Text style={styles.statusExtra}>{line.extra}</Text>}
          </View>
        ))}
      </View>

      <View style={styles.statusRow}>
        <Text style={styles.statusLabel}>{cooldownCopy.label}</Text>
        <Text style={[styles.statusValue, { color: toneColors[cooldownCopy.tone] }]}>
          {cooldownCopy.value}
        </Text>
      </View>
    </View>
  );
};

const styles = StyleSheet.create({
  wrapper: {
    backgroundColor: palette.card,
    borderRadius: radius.lg,
    padding: spacing.lg,
    gap: spacing.md,
  },
  cardLabel: {
    color: palette.textSecondary,
    textTransform: 'uppercase',
    letterSpacing: 1,
    fontSize: 12,
  },
  modeCard: {
    gap: spacing.xs,
  },
  modeValue: {
    color: palette.textPrimary,
    fontSize: 28,
    fontWeight: '700',
  },
  activeLabel: {
    color: palette.textPrimary,
    fontSize: 16,
  },
  timestamp: {
    color: palette.textSecondary,
    fontSize: 12,
  },
  loadCard: {
    padding: spacing.md,
    borderRadius: radius.md,
    backgroundColor: palette.cardMuted,
    gap: spacing.xs,
  },
  loadValue: {
    fontSize: 32,
    fontWeight: '700',
  },
  loadCaption: {
    color: palette.textSecondary,
    fontSize: 12,
  },
  statusList: {
    borderRadius: radius.md,
    backgroundColor: palette.surface,
  },
  statusRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'flex-start',
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.sm,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: palette.outline,
    gap: spacing.sm,
  },
  statusLabel: {
    color: palette.textSecondary,
    fontSize: 14,
    flex: 1,
  },
  statusValue: {
    fontSize: 16,
    fontWeight: '600',
  },
  statusExtra: {
    color: palette.textSecondary,
    fontSize: 14,
  },
  pendingCard: {
    backgroundColor: palette.card,
    padding: spacing.lg,
    borderRadius: radius.lg,
    gap: spacing.sm,
  },
  pendingTitle: {
    color: palette.textPrimary,
    fontSize: 18,
    fontWeight: '600',
  },
  pendingSubtitle: {
    color: palette.textSecondary,
  },
  guardCard: {
    backgroundColor: palette.danger,
  },
  guardTitle: {
    color: palette.background,
    fontSize: 20,
    fontWeight: '700',
  },
  guardBody: {
    color: palette.background,
    fontSize: 14,
  },
});

export default memo(HomeStatusPanel);
