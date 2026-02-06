import { useEffect } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { useDeviceStore } from '@/state/deviceStore';
import { palette } from '@/theme/colors';
import { radius, spacing } from '@/theme/layout';

const stateCopy: Record<string, { label: string; color: string; body: string }> = {
  connected: {
    label: 'Connected',
    color: palette.success,
    body: 'Bluetooth link established. Relays are ready.',
  },
  connecting: {
    label: 'Connecting',
    color: palette.warning,
    body: 'Scanning for the ESP32. Keep the app close to the vehicle.',
  },
  disconnected: {
    label: 'Disconnected',
    color: palette.danger,
    body: 'Tap reconnect once the controller is powered on.',
  },
};

const StatusBanner = () => {
  const connectionState = useDeviceStore((state) => state.connectionState);
  const knownDevice = useDeviceStore((state) => state.knownDevice);
  const commandError = useDeviceStore((state) => state.commandError);
  const dismissCommandError = useDeviceStore((state) => state.setCommandError);
  const copy = stateCopy[connectionState];

  const deviceName = knownDevice?.name ?? 'Unknown device';
  const rssiText =
    typeof knownDevice?.lastRssi === 'number' ? `${knownDevice.lastRssi} dBm` : 'signal n/a';

  useEffect(() => {
    if (!commandError) {
      return undefined;
    }
    const timer = setTimeout(() => dismissCommandError(null), 6000);
    return () => clearTimeout(timer);
  }, [commandError?.timestamp, dismissCommandError]);

  return (
    <View style={[styles.container, { borderColor: copy.color }]}>
      <View style={styles.badge}> 
        <Text style={[styles.badgeText, { color: copy.color }]}>{copy.label}</Text>
      </View>
      <Text style={styles.body}>{copy.body}</Text>
      {knownDevice ? (
        <Text style={styles.meta}>Device: {deviceName} · {rssiText}</Text>
      ) : (
        <Text style={styles.meta}>No cached controller yet</Text>
      )}
      {commandError && (
        <Pressable style={styles.errorBox} onPress={() => dismissCommandError(null)}>
          <Text style={styles.errorTitle}>Command failed</Text>
          <Text style={styles.errorBody}>
            Could not toggle {commandError.relayLabel}. {commandError.message}
          </Text>
          <Text style={styles.errorHint}>Tap to dismiss</Text>
        </Pressable>
      )}
    </View>
  );
};

const styles = StyleSheet.create({
  container: {
    borderWidth: 1,
    borderRadius: radius.lg,
    padding: spacing.md,
    backgroundColor: palette.surface,
    gap: spacing.xs,
  },
  badge: {
    borderRadius: radius.sm,
    paddingHorizontal: spacing.sm,
    paddingVertical: spacing.xs,
    backgroundColor: palette.background,
    alignSelf: 'flex-start',
  },
  badgeText: {
    fontWeight: '700',
    textTransform: 'uppercase',
    fontSize: 12,
  },
  body: {
    color: palette.textPrimary,
    fontSize: 14,
  },
  meta: {
    color: palette.textSecondary,
    fontSize: 12,
  },
  errorBox: {
    marginTop: spacing.sm,
    borderRadius: radius.md,
    backgroundColor: palette.danger,
    padding: spacing.sm,
    gap: spacing.xs,
  },
  errorTitle: {
    color: palette.background,
    fontWeight: '700',
    fontSize: 14,
    textTransform: 'uppercase',
  },
  errorBody: {
    color: palette.background,
    fontSize: 13,
  },
  errorHint: {
    color: palette.background,
    fontSize: 11,
    opacity: 0.8,
  },
});

export default StatusBanner;
