import { RelayChannel } from '@/types/device';

export const RELAY_CHANNELS: RelayChannel[] = [
  { id: 'relay-left', label: 'Left Turn', description: 'Driver side indicator' },
  { id: 'relay-right', label: 'Right Turn', description: 'Passenger side indicator' },
  { id: 'relay-brake', label: 'Brake', description: 'Brake lamp output' },
  { id: 'relay-tail', label: 'Tail', description: 'Tail / running lamps' },
  { id: 'relay-marker', label: 'Marker / Rev', description: 'Marker or reverse (RV mode)' },
  { id: 'relay-aux', label: 'Aux / Ele Brakes', description: 'Auxiliary or electric brakes' },
];

export const RELAY_IDS = RELAY_CHANNELS.map((channel) => channel.id);
