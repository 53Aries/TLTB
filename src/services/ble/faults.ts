export interface FaultDescriptor {
  bit: number;
  text: string;
}

export const faultCatalog: FaultDescriptor[] = [
  { bit: 1 << 0, text: 'Load INA missing' },
  { bit: 1 << 1, text: 'Src INA missing' },
  { bit: 1 << 2, text: 'Wi-Fi disconnected' },
  { bit: 1 << 3, text: 'RF missing' },
];

export const buildFaultMessages = (mask: number, fallback?: string[]) => {
  if (fallback && fallback.length > 0) {
    return fallback;
  }

  if (!mask) {
    return [];
  }

  return faultCatalog.filter((fault) => (mask & fault.bit) !== 0).map((fault) => fault.text);
};
