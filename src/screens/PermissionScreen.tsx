import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { palette } from '@/theme/colors';
import { radius, spacing } from '@/theme/layout';
import { PermissionStatus } from '@/hooks/useBlePermissions';

interface PermissionScreenProps {
  status: PermissionStatus;
  onRetry: () => void;
}

const PermissionScreen = ({ status, onRetry }: PermissionScreenProps) => {
  if (status === 'checking') {
    return (
      <SafeAreaView style={styles.container}>
        <ActivityIndicator size="large" color={palette.accent} />
        <Text style={styles.title}>Checking Permissions...</Text>
        <Text style={styles.subtitle}>Setting up TLTB Controller</Text>
      </SafeAreaView>
    );
  }

  if (status === 'denied' || status === 'blocked') {
    const isBlocked = status === 'blocked';
    
    return (
      <SafeAreaView style={styles.container}>
        <View style={styles.card}>
          <Text style={styles.icon}>⚠️</Text>
          <Text style={styles.title}>
            {isBlocked ? 'Permission Denied' : 'Bluetooth Permission Required'}
          </Text>
          <Text style={styles.message}>
            {isBlocked
              ? 'TLTB needs Bluetooth permissions to connect to your controller. Please enable them in your device Settings.'
              : 'TLTB needs Bluetooth permissions to connect to your controller.'}
          </Text>
          
          {!isBlocked && (
            <Pressable style={styles.button} onPress={onRetry}>
              <Text style={styles.buttonText}>Grant Permission</Text>
            </Pressable>
          )}
          
          {isBlocked && (
            <View style={styles.infoBox}>
              <Text style={styles.infoTitle}>How to enable:</Text>
              <Text style={styles.infoText}>
                1. Open Settings{'\n'}
                2. Go to Apps → TLTB Control{'\n'}
                3. Tap Permissions{'\n'}
                4. Enable Bluetooth and Location
              </Text>
            </View>
          )}
        </View>
      </SafeAreaView>
    );
  }

  return null;
};

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: palette.background,
    justifyContent: 'center',
    alignItems: 'center',
    padding: spacing.xl,
    gap: spacing.lg,
  },
  card: {
    backgroundColor: palette.surface,
    borderRadius: radius.xl,
    padding: spacing.xl,
    width: '100%',
    maxWidth: 400,
    alignItems: 'center',
    gap: spacing.md,
  },
  icon: {
    fontSize: 64,
  },
  title: {
    color: palette.textPrimary,
    fontSize: 20,
    fontWeight: '700',
    textAlign: 'center',
  },
  subtitle: {
    color: palette.textSecondary,
    fontSize: 14,
    textAlign: 'center',
  },
  message: {
    color: palette.textPrimary,
    fontSize: 16,
    lineHeight: 24,
    textAlign: 'center',
  },
  button: {
    backgroundColor: palette.accent,
    borderRadius: radius.lg,
    paddingHorizontal: spacing.xl,
    paddingVertical: spacing.md,
    marginTop: spacing.md,
  },
  buttonText: {
    color: palette.background,
    fontSize: 16,
    fontWeight: '700',
  },
  infoBox: {
    backgroundColor: palette.card,
    borderRadius: radius.md,
    padding: spacing.md,
    marginTop: spacing.md,
    width: '100%',
  },
  infoTitle: {
    color: palette.warning,
    fontSize: 14,
    fontWeight: '700',
    marginBottom: spacing.sm,
  },
  infoText: {
    color: palette.textSecondary,
    fontSize: 14,
    lineHeight: 22,
  },
});

export default PermissionScreen;
