import { PropsWithChildren } from 'react';
import { StatusBar } from 'expo-status-bar';
import { SafeAreaProvider } from 'react-native-safe-area-context';

import { useBlePermissions } from '@/hooks/useBlePermissions';
import { useBleTransport } from '@/hooks/useBleTransport';
import { useKnownDeviceBootstrap } from '@/hooks/useKnownDeviceBootstrap';
import HomeScreen from '@/screens/HomeScreen';
import PermissionScreen from '@/screens/PermissionScreen';

const Providers = ({ children }: PropsWithChildren) => {
  const { status, isGranted, requestPermissions } = useBlePermissions();
  const ready = useKnownDeviceBootstrap();
  useBleTransport({ ready: ready && isGranted });

  return (
    <SafeAreaProvider>
      <StatusBar style="light" />
      {!isGranted ? (
        <PermissionScreen status={status} onRetry={requestPermissions} />
      ) : (
        children
      )}
    </SafeAreaProvider>
  );
};

const AppProviders = () => (
  <Providers>
    <HomeScreen />
  </Providers>
);

export default AppProviders;
