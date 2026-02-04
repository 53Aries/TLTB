import { appConfig } from '@/config/appConfig';

import { useMockBleSession } from './useMockBleSession';
import { useTltbBleSession } from './useTltbBleSession';

interface UseBleTransportOptions {
  ready?: boolean;
}

export const useBleTransport = ({ ready = true }: UseBleTransportOptions = {}) => {
  useMockBleSession({ enabled: ready && appConfig.useMockBleTransport });
  useTltbBleSession({ enabled: ready && !appConfig.useMockBleTransport });
};
