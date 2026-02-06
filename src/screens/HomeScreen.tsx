import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import FaultTicker from '@/components/FaultTicker';
import HomeStatusPanel from '@/components/HomeStatusPanel';
import ProtectionFaultModal from '@/components/ProtectionFaultModal';
import RelayGrid from '@/components/RelayGrid';
import StatusBanner from '@/components/StatusBanner';
import { palette } from '@/theme/colors';
import { spacing } from '@/theme/layout';

const HomeScreen = () => {
  return (
    <SafeAreaView style={styles.safeArea}>
      <ScrollView contentContainerStyle={styles.content}>
        <View>
          <Text style={styles.heading}>TLTB Controller</Text>
          <Text style={styles.subheading}>Control relays and monitor telemetry over BLE.</Text>
        </View>

        <StatusBanner />
        <HomeStatusPanel />

        <View style={styles.sectionHeader}>
          <Text style={styles.sectionTitle}>Relay Deck</Text>
          <Text style={styles.sectionSubtitle}>Left/Right, brake, tail, marker, and aux outputs.</Text>
        </View>

        <RelayGrid />
        <FaultTicker />
      </ScrollView>
      <ProtectionFaultModal />
    </SafeAreaView>
  );
};

const styles = StyleSheet.create({
  safeArea: {
    flex: 1,
    backgroundColor: palette.background,
  },
  content: {
    padding: spacing.lg,
    gap: spacing.lg,
  },
  heading: {
    color: palette.textPrimary,
    fontSize: 28,
    fontWeight: '700',
  },
  subheading: {
    color: palette.textSecondary,
    fontSize: 14,
  },
  sectionHeader: {
    gap: spacing.xs,
  },
  sectionTitle: {
    color: palette.textPrimary,
    fontSize: 18,
    fontWeight: '600',
  },
  sectionSubtitle: {
    color: palette.textSecondary,
    fontSize: 13,
  },
});

export default HomeScreen;
