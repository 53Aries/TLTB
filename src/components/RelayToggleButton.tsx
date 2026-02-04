import { memo } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { palette } from '@/theme/colors';
import { radius, spacing } from '@/theme/layout';

interface RelayToggleButtonProps {
  label: string;
  description: string;
  isOn: boolean;
  disabled?: boolean;
  onPress: () => void;
}

const RelayToggleButton = ({ label, description, isOn, disabled, onPress }: RelayToggleButtonProps) => (
  <Pressable
    accessibilityRole="button"
    onPress={onPress}
    disabled={disabled}
    style={({ pressed }) => [
      styles.base,
      isOn ? styles.active : styles.inactive,
      disabled && styles.disabled,
      pressed && !disabled ? styles.pressed : null,
    ]}
  >
    <View style={styles.headerRow}>
      <Text style={styles.label}>{label}</Text>
      <View style={[styles.badge, isOn ? styles.badgeOn : styles.badgeOff]}>
        <Text style={styles.badgeText}>{isOn ? 'ON' : 'OFF'}</Text>
      </View>
    </View>
    <Text style={styles.description}>{description}</Text>
  </Pressable>
);

const styles = StyleSheet.create({
  base: {
    flex: 1,
    padding: spacing.lg,
    borderRadius: radius.lg,
    borderWidth: 1,
    borderColor: palette.outline,
    backgroundColor: palette.card,
    gap: spacing.sm,
  },
  active: {
    borderColor: palette.accent,
    shadowColor: palette.accent,
    shadowOpacity: 0.2,
    shadowRadius: 8,
  },
  inactive: {
    borderColor: palette.outline,
  },
  disabled: {
    opacity: 0.5,
  },
  pressed: {
    transform: [{ scale: 0.98 }],
  },
  headerRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  label: {
    color: palette.textPrimary,
    fontSize: 18,
    fontWeight: '600',
  },
  description: {
    color: palette.textSecondary,
    fontSize: 13,
  },
  badge: {
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.xs,
    borderRadius: radius.md,
  },
  badgeOn: {
    backgroundColor: palette.success,
  },
  badgeOff: {
    backgroundColor: palette.cardMuted,
  },
  badgeText: {
    color: palette.background,
    fontWeight: '700',
    fontSize: 12,
  },
});

export default memo(RelayToggleButton);
