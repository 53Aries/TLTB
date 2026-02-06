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
    <Text style={styles.label} numberOfLines={1}>{label}</Text>
    <View style={[styles.badge, isOn ? styles.badgeOn : styles.badgeOff]}>
      <Text style={styles.badgeText} numberOfLines={1}>{isOn ? 'ON' : 'OFF'}</Text>
    </View>
    <Text style={styles.description} numberOfLines={2}>{description}</Text>
  </Pressable>
);

const styles = StyleSheet.create({
  base: {
    width: '47%',
    minHeight: 120,
    padding: spacing.lg,
    borderRadius: radius.lg,
    borderWidth: 2,
    borderColor: palette.outline,
    backgroundColor: palette.card,
    justifyContent: 'center',
    alignItems: 'center',
  },
  active: {
    borderColor: palette.accent,
    backgroundColor: palette.accent + '20',
  },
  inactive: {
    borderColor: palette.outline,
  },
  disabled: {
    opacity: 0.5,
  },
  pressed: {
    opacity: 0.8,
  },
  label: {
    color: palette.textPrimary,
    fontSize: 16,
    fontWeight: '700',
    textAlign: 'center',
    marginBottom: spacing.xs,
  },
  description: {
    color: palette.textSecondary,
    fontSize: 12,
    textAlign: 'center',
    marginTop: spacing.xs,
  },
  badge: {
    paddingHorizontal: spacing.md,
    paddingVertical: spacing.xs,
    borderRadius: radius.full,
    marginVertical: spacing.sm,
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
    fontSize: 14,
  },
});

export default memo(RelayToggleButton);
