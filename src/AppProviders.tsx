import { PropsWithChildren } from 'react';
import { StatusBar } from 'expo-status-bar';
import { SafeAreaProvider } from 'react-native-safe-area-context';

import { useBleTransport } from '@/hooks/useBleTransport';
import { useKnownDeviceBootstrap } from '@/hooks/useKnownDeviceBootstrap';
import HomeScreen from '@/screens/HomeScreen';

const Providers = ({ children }: PropsWithChildren) => {
  const ready = useKnownDeviceBootstrap();
  useBleTransport({ ready });

  return (
    <SafeAreaProvider>
      <StatusBar style="light" />
      {children}
    </SafeAreaProvider>
  );
};

const AppProviders = () => (
  <Providers>
    <HomeScreen />
  </Providers>
);

export default AppProviders;
