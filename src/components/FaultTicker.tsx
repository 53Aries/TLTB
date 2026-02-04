import { memo } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';

import { useDeviceStore } from '@/state/deviceStore';
import { palette } from '@/theme/colors';
import { radius, spacing } from '@/theme/layout';

const FaultTicker = () => {
  const homeStatus = useDeviceStore((state) => state.homeStatus);

  if (!homeStatus) {
    return null;
  }

  if (homeStatus.faultMask === 0 || homeStatus.faultMessages.length === 0) {
    return (
      <View style={[styles.container, styles.hintContainer]}>
        <Text style={styles.hintText}>OK = Switch Mode</Text>
      </View>
    );
  }

  return (
    <View style={styles.container}>
      <ScrollView horizontal showsHorizontalScrollIndicator={false}>
        <Text style={styles.tickerText}>
          {homeStatus.faultMessages.join('   |   ')}
        </Text>
      </ScrollView>
    </View>
  );
};

const styles = StyleSheet.create({
  container: {
    borderRadius: radius.md,
    padding: spacing.md,
    backgroundColor: palette.danger,
  },
  tickerText: {
    color: palette.textPrimary,
    fontWeight: '600',
  },
  hintContainer: {
    backgroundColor: palette.surface,
  },
  hintText: {
    color: palette.warning,
    textAlign: 'center',
    fontWeight: '600',
  },
});

export default memo(FaultTicker);
