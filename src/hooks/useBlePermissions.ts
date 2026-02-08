import { useEffect, useState } from 'react';
import { Platform, PermissionsAndroid, Alert } from 'react-native';

export type PermissionStatus = 'checking' | 'granted' | 'denied' | 'blocked';

export const useBlePermissions = () => {
  const [status, setStatus] = useState<PermissionStatus>('checking');

  const requestPermissions = async () => {
    if (Platform.OS !== 'android') {
      setStatus('granted');
      return;
    }

    try {
      const androidVersion = Platform.Version as number;

      // Android 12+ (API 31+) requires BLUETOOTH_SCAN and BLUETOOTH_CONNECT
      if (androidVersion >= 31) {
        const permissions = [
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
          PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        ];

        const granted = await PermissionsAndroid.requestMultiple(permissions);

        const scanGranted = granted['android.permission.BLUETOOTH_SCAN'] === 'granted';
        const connectGranted = granted['android.permission.BLUETOOTH_CONNECT'] === 'granted';

        if (scanGranted && connectGranted) {
          setStatus('granted');
        } else {
          const neverAskAgain =
            granted['android.permission.BLUETOOTH_SCAN'] === 'never_ask_again' ||
            granted['android.permission.BLUETOOTH_CONNECT'] === 'never_ask_again';

          setStatus(neverAskAgain ? 'blocked' : 'denied');
          showPermissionAlert(neverAskAgain);
        }
      } else {
        // Android 10-11 needs ACCESS_FINE_LOCATION for BLE scanning
        const granted = await PermissionsAndroid.request(
          PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
          {
            title: 'Location Permission',
            message: 'TLTB needs location access to scan for Bluetooth devices.',
            buttonPositive: 'OK',
          }
        );

        if (granted === 'granted') {
          setStatus('granted');
        } else {
          setStatus(granted === 'never_ask_again' ? 'blocked' : 'denied');
          showPermissionAlert(granted === 'never_ask_again');
        }
      }
    } catch (error) {
      console.error('[Permissions] Error requesting BLE permissions:', error);
      setStatus('denied');
    }
  };

  const showPermissionAlert = (isBlocked: boolean) => {
    if (isBlocked) {
      Alert.alert(
        'Bluetooth Permission Required',
        'TLTB needs Bluetooth permissions to connect to your controller. Please enable them in Settings.',
        [
          { text: 'Cancel', style: 'cancel' },
          {
            text: 'Open Settings',
            onPress: () => {
              // Note: Linking.openSettings() would go here in production
              console.log('[Permissions] Would open settings');
            },
          },
        ]
      );
    } else {
      Alert.alert(
        'Bluetooth Permission Required',
        'TLTB needs Bluetooth permissions to connect to your controller.',
        [
          { text: 'Cancel', style: 'cancel' },
          { text: 'Grant Permission', onPress: requestPermissions },
        ]
      );
    }
  };

  useEffect(() => {
    requestPermissions();
  }, []);

  return {
    status,
    requestPermissions,
    isGranted: status === 'granted',
    isDenied: status === 'denied',
    isBlocked: status === 'blocked',
    isChecking: status === 'checking',
  };
};
