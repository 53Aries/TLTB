import { memo, useEffect } from 'react';
import { Modal, Pressable, StyleSheet, Text, View } from 'react-native';

import { useDeviceStore } from '@/state/deviceStore';
import { palette } from '@/theme/colors';
import { radius, spacing } from '@/theme/layout';

const ProtectionFaultModal = () => {
  const homeStatus = useDeviceStore((state) => state.homeStatus);
  const protectionFaultAcknowledged = useDeviceStore((state) => state.protectionFaultAcknowledged);
  const clearProtectionFault = useDeviceStore((state) => state.clearProtectionFault);
  const resetFaultAcknowledgment = useDeviceStore((state) => state.resetFaultAcknowledgment);

  const lvpLatched = homeStatus?.lvpLatched ?? false;
  const outvLatched = homeStatus?.outvLatched ?? false;
  const hasFault = lvpLatched || outvLatched;

  // Reset acknowledgment when fault state changes
  useEffect(() => {
    if (hasFault) {
      resetFaultAcknowledgment();
    }
  }, [lvpLatched, outvLatched, hasFault, resetFaultAcknowledgment]);

  if (!homeStatus || !hasFault || protectionFaultAcknowledged) {
    return null;
  }

  const faultTitle = lvpLatched && outvLatched 
    ? 'MULTIPLE PROTECTION FAULTS'
    : lvpLatched 
    ? 'LOW VOLTAGE PROTECTION' 
    : 'OUTPUT VOLTAGE FAULT';
    
  const faultMessage = lvpLatched && outvLatched
    ? 'Both low voltage and output voltage faults detected. All relays have been disabled for safety.'
    : lvpLatched
    ? 'Input voltage dropped below safe threshold. All relays have been disabled to protect the battery.'
    : 'Output voltage fault detected. All relays have been disabled for safety.';

  return (
    <Modal visible={true} transparent animationType="fade">
      <View style={styles.overlay}>
        <View style={styles.modal}>
          <View style={styles.header}>
            <Text style={styles.icon}>⚠️</Text>
            <Text style={styles.title}>{faultTitle}</Text>
          </View>

          <View style={styles.content}>
            <Text style={styles.message}>{faultMessage}</Text>

            <View style={styles.infoBox}>
              <Text style={styles.infoTitle}>To Clear This Fault:</Text>
              <Text style={styles.infoText}>
                1. Address the underlying issue{'\n'}
                2. Cycle the rotary encoder to OFF position{'\n'}
                3. Return to desired mode
              </Text>
            </View>
          </View>

          <Pressable style={styles.button} onPress={clearProtectionFault}>
            <Text style={styles.buttonText}>Acknowledge</Text>
          </Pressable>
        </View>
      </View>
    </Modal>
  );
};

const styles = StyleSheet.create({
  overlay: {
    flex: 1,
    backgroundColor: 'rgba(0, 0, 0, 0.8)',
    justifyContent: 'center',
    alignItems: 'center',
    padding: spacing.xl,
  },
  modal: {
    backgroundColor: palette.surface,
    borderRadius: radius.xl,
    borderWidth: 3,
    borderColor: palette.danger,
    width: '100%',
    maxWidth: 400,
    overflow: 'hidden',
  },
  header: {
    backgroundColor: palette.danger,
    padding: spacing.xl,
    alignItems: 'center',
  },
  icon: {
    fontSize: 48,
    marginBottom: spacing.sm,
  },
  title: {
    color: palette.textPrimary,
    fontSize: 20,
    fontWeight: '800',
    textAlign: 'center',
    letterSpacing: 1,
  },
  content: {
    padding: spacing.xl,
  },
  message: {
    color: palette.textPrimary,
    fontSize: 16,
    lineHeight: 24,
    marginBottom: spacing.lg,
  },
  infoBox: {
    backgroundColor: palette.card,
    padding: spacing.md,
    borderRadius: radius.md,
    borderLeftWidth: 4,
    borderLeftColor: palette.warning,
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
  button: {
    backgroundColor: palette.danger,
    padding: spacing.lg,
    alignItems: 'center',
  },
  buttonText: {
    color: palette.textPrimary,
    fontSize: 16,
    fontWeight: '700',
  },
});

export default memo(ProtectionFaultModal);
