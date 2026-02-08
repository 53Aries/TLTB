#!/usr/bin/env python3
"""Analyze ESP32 firmware binary format"""

with open('.pio/build/esp32s3-devkitc1/firmware.bin', 'rb') as f:
    data = f.read()

print(f"Total firmware size: {len(data)} bytes")
print(f"\nESP32 Image Header:")
print(f"  Magic: 0x{data[0]:02X}")
seg_count = data[1]
print(f"  Segments: {seg_count}")
print(f"  SPI mode: 0x{data[2]:02X}")
print(f"  Flash config: 0x{data[3]:02X}")
entry = int.from_bytes(data[4:8], 'little')
print(f"  Entry point: 0x{entry:08X}")

print(f"\nSegment Analysis:")
offset = 24  # Segments start after 24-byte header

for i in range(seg_count):
    if offset + 8 > len(data):
        print(f"Segment {i}: ERROR - offset {offset} beyond file length")
        break
    
    addr = int.from_bytes(data[offset:offset+4], 'little')
    length = int.from_bytes(data[offset+4:offset+8], 'little')
    
    print(f"  Segment {i}:")
    print(f"    Header offset: {offset}")
    print(f"    Load address: 0x{addr:08X}")
    print(f"    Length: {length} (0x{length:08X})")
    
    if length > 0x200000:  # > 2MB
        print(f"    WARNING: Suspicious length!")
        # Show what the bytes actually are
        print(f"    Raw length bytes: {' '.join(f'{b:02X}' for b in data[offset+4:offset+8])}")
        break
    
    data_start = offset + 8
    data_end = data_start + length
    print(f"    Data: {data_start} to {data_end}")
    
    offset = data_end

print(f"\nExpected image end (before padding/SHA256): {offset}")
print(f"Actual file size: {len(data)}")
print(f"Difference (padding + SHA256): {len(data) - offset} bytes")

# Check last 32 bytes (should be SHA256)
print(f"\nLast 32 bytes (SHA256 hash):")
sha256 = data[-32:]
print(' '.join(f'{b:02X}' for b in sha256))

# Search for the problematic value
search_bytes = bytes([0x6D, 0x69, 0x6E, 0x67])  # "ming" in little-endian
pos = data.find(search_bytes)
print(f"\nSearching for 0x676E696D (little-endian: '6D 69 6E 67'):")
if pos >= 0:
    print(f"  Found at offset: {pos}")
    print(f"  Context (32 bytes): {' '.join(f'{b:02X}' for b in data[max(0,pos-16):pos+16])}")
else:
    print(f"  Not found")
