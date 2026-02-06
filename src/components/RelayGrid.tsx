import { StyleSheet, View } from 'react-native';

import { RELAY_CHANNELS } from '@/constants/relays';
import { useDeviceStore } from '@/state/deviceStore';
import { palette } from '@/theme/colors';
import { spacing } from '@/theme/layout';

import RelayToggleButton from './RelayToggleButton';

const RelayGrid = () => {
  const relays = useDeviceStore((state) => state.relays);
  const requestRelayToggle = useDeviceStore((state) => state.requestRelayToggle);
  const connectionState = useDeviceStore((state) => state.connectionState);
  const disabled = connectionState !== 'connected';

  return (
    <View style={styles.grid}>
      {RELAY_CHANNELS.map((relay) => (
        <RelayToggleButton
          key={relay.id}
          label={relay.label}
          description={relay.description}
          isOn={relays[relay.id]?.isOn ?? false}
          disabled={disabled}
          onPress={() => requestRelayToggle(relay.id)}
        />
      ))}
    </View>
  );
};

const styles = StyleSheet.create({
  grid: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    justifyContent: 'space-between',
    rowGap: spacing.md,
    backgroundColor: palette.surface,
    padding: spacing.md,
    borderRadius: 24,
  },
});

export default RelayGrid;
