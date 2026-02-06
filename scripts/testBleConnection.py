#!/usr/bin/env python3
"""
TLTB BLE Controller - Control relays via Bluetooth from PC
Install: pip install bleak
Run: python scripts/testBleConnection.py
"""

import asyncio
import base64
import json
import sys
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "0000a11c-0000-1000-8000-00805f9b34fb"
STATUS_CHAR_UUID = "0000a11d-0000-1000-8000-00805f9b34fb"
CONTROL_CHAR_UUID = "0000a11e-0000-1000-8000-00805f9b34fb"

RELAY_NAMES = {
    "1": ("relay-left", "Left Turn"),
    "2": ("relay-right", "Right Turn"),
    "3": ("relay-brake", "Brake"),
    "4": ("relay-tail", "Tail"),
    "5": ("relay-marker", "Marker/Rev"),
    "6": ("relay-aux", "Aux/Elec Brakes"),
}

current_status = {}
relay_states = {}

def decode_status(data):
    """Decode status notification"""
    try:
        json_str = base64.b64decode(data).decode('utf-8')
        return json.loads(json_str)
    except:
        try:
            return json.loads(data.decode('utf-8'))
        except:
            return None

def notification_handler(sender, data):
    """Handle incoming BLE notifications"""
    global current_status, relay_states
    
    parsed = decode_status(data)
    if parsed:
        current_status = parsed
        
        # Decode relay mask
        mask = parsed.get('relayMask', 0)
        relay_ids = [id for id, _ in RELAY_NAMES.values()]
        for i, relay_id in enumerate(relay_ids):
            relay_states[relay_id] = (mask & (1 << i)) != 0
        
        # Clear screen and show status
        print("\033[2J\033[H")  # Clear screen
        print("=" * 60)
        print("TLTB Controller Status")
        print("=" * 60)
        print(f"Mode: {parsed.get('mode', 'Unknown')}")
        print(f"Active Label: {parsed.get('activeLabel', 'OFF')}")
        print(f"12V Enabled: {'Yes' if (parsed.get('statusFlags', 0) & 1) else 'No'}")
        print(f"Fault Mask: {parsed.get('faultMask', 0)}")
        if parsed.get('loadAmps') is not None:
            print(f"Load: {parsed.get('loadAmps'):.1f}A")
        if parsed.get('srcVoltage') is not None:
            print(f"Source: {parsed.get('srcVoltage'):.1f}V")
        if parsed.get('outVoltage') is not None:
            print(f"Output: {parsed.get('outVoltage'):.1f}V")
        
        print("\nRelay States:")
        print("-" * 60)
        for key, (relay_id, name) in RELAY_NAMES.items():
            state = "ON " if relay_states.get(relay_id, False) else "OFF"
            print(f"  [{key}] {name:20s} [{state}]")
        
        print("\n" + "=" * 60)
        print("Commands: [1-6]=Toggle relay  [r]=Refresh  [q]=Quit")
        print("=" * 60)

async def send_command(client, command):
    """Send a command to the control characteristic"""
    json_str = json.dumps(command)
    json_bytes = json_str.encode('utf-8')
    base64_bytes = base64.b64encode(json_bytes)
    
    await client.write_gatt_char(CONTROL_CHAR_UUID, base64_bytes, response=False)

async def handle_input(client):
    """Handle user input for relay control"""
    loop = asyncio.get_event_loop()
    
    while True:
        try:
            # Non-blocking input
            cmd = await loop.run_in_executor(None, input, "")
            cmd = cmd.strip().lower()
            
            if cmd == 'q':
                print("\n[BLE] Shutting down...")
                return False
            elif cmd == 'r':
                print("[BLE] Requesting refresh...")
                await send_command(client, {"type": "refresh"})
            elif cmd in RELAY_NAMES:
                relay_id, name = RELAY_NAMES[cmd]
                current_state = relay_states.get(relay_id, False)
                new_state = not current_state
                print(f"[BLE] Toggling {name} -> {'ON' if new_state else 'OFF'}...")
                await send_command(client, {
                    "type": "relay",
                    "relayId": relay_id,
                    "state": new_state
                })
            else:
                print(f"[BLE] Unknown command: {cmd}")
        except KeyboardInterrupt:
            return False
        except Exception as e:
            print(f"[BLE] Input error: {e}")
            return False

async def main():
    print("[BLE] Scanning for TLTB Controller...")
    
    devices = await BleakScanner.discover()
    target = None
    
    for device in devices:
        if device.name == "TLTB Controller":
            target = device
            print(f"[BLE] ✓ Found: {device.name} ({device.address})")
            break
    
    if not target:
        print("[BLE] ✗ TLTB Controller not found!")
        print("[BLE] Available devices:")
        for device in devices:
            print(f"  - {device.name or 'Unknown'} ({device.address})")
        return
    
    print(f"[BLE] Connecting to {target.address}...")
    
    async with BleakClient(target.address) as client:
        print(f"[BLE] ✓ Connected! (MTU: {client.mtu_size})")
        
        # Subscribe to notifications
        await client.start_notify(STATUS_CHAR_UUID, notification_handler)
        print(f"[BLE] ✓ Subscribed to notifications")
        
        # Wait a moment for initial status
        await asyncio.sleep(0.5)
        
        # Request a refresh to get current status
        await send_command(client, {"type": "refresh"})
        await asyncio.sleep(0.5)
        
        # Handle user input
        await handle_input(client)
        
        await client.stop_notify(STATUS_CHAR_UUID)

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[BLE] Interrupted")
