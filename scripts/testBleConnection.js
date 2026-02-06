#!/usr/bin/env node
/**
 * Quick BLE connection test for Windows PC
 * Install: npm install @abandonware/noble
 * Run: node scripts/testBleConnection.js
 */

const noble = require('@abandonware/noble');

const SERVICE_UUID = '0000a11c00001000800000805f9b34fb';
const STATUS_CHAR_UUID = '0000a11d00001000800000805f9b34fb';
const CONTROL_CHAR_UUID = '0000a11e00001000800000805f9b34fb';

let targetDevice = null;

console.log('[BLE Test] Starting scan for TLTB Controller...');

noble.on('stateChange', (state) => {
  console.log(`[BLE Test] Bluetooth state: ${state}`);
  if (state === 'poweredOn') {
    console.log('[BLE Test] Starting scan...');
    noble.startScanning([SERVICE_UUID], false);
  } else {
    noble.stopScanning();
  }
});

noble.on('discover', async (peripheral) => {
  console.log(`[BLE Test] Found device: ${peripheral.advertisement.localName || 'Unknown'} (${peripheral.address})`);
  
  if (peripheral.advertisement.localName === 'TLTB Controller' || 
      peripheral.advertisement.serviceUuids?.includes(SERVICE_UUID)) {
    console.log('[BLE Test] ✓ Found TLTB Controller!');
    noble.stopScanning();
    
    targetDevice = peripheral;
    
    peripheral.on('disconnect', () => {
      console.log('[BLE Test] ✗ Disconnected from device');
      process.exit(0);
    });
    
    try {
      console.log('[BLE Test] Connecting...');
      await peripheral.connectAsync();
      console.log('[BLE Test] ✓ Connected!');
      
      console.log('[BLE Test] Discovering services and characteristics...');
      const { characteristics } = await peripheral.discoverSomeServicesAndCharacteristicsAsync(
        [SERVICE_UUID],
        [STATUS_CHAR_UUID, CONTROL_CHAR_UUID]
      );
      
      console.log(`[BLE Test] ✓ Found ${characteristics.length} characteristics`);
      
      const statusChar = characteristics.find(c => c.uuid === STATUS_CHAR_UUID);
      const controlChar = characteristics.find(c => c.uuid === CONTROL_CHAR_UUID);
      
      if (!statusChar) {
        console.error('[BLE Test] ✗ Status characteristic not found!');
        return;
      }
      
      console.log('[BLE Test] ✓ Found status characteristic');
      console.log(`[BLE Test] Properties: ${statusChar.properties.join(', ')}`);
      
      // Read initial value
      console.log('[BLE Test] Reading initial value...');
      const initialValue = await statusChar.readAsync();
      console.log(`[BLE Test] Initial value (${initialValue.length} bytes):`);
      console.log(initialValue.toString('base64'));
      
      try {
        const decoded = JSON.parse(Buffer.from(initialValue.toString('utf8'), 'base64').toString('utf8'));
        console.log('[BLE Test] Decoded JSON:', JSON.stringify(decoded, null, 2));
      } catch (e) {
        console.log('[BLE Test] Could not decode as base64 JSON, trying raw parse...');
        try {
          const decoded = JSON.parse(initialValue.toString('utf8'));
          console.log('[BLE Test] Decoded JSON (raw):', JSON.stringify(decoded, null, 2));
        } catch (e2) {
          console.log('[BLE Test] Parse error:', e2.message);
        }
      }
      
      // Subscribe to notifications
      console.log('[BLE Test] Subscribing to notifications...');
      statusChar.on('data', (data, isNotification) => {
        console.log(`\n[BLE Test] ${isNotification ? 'Notification' : 'Read'} received (${data.length} bytes)`);
        console.log('Base64:', data.toString('base64').substring(0, 80) + '...');
        
        try {
          const decoded = JSON.parse(Buffer.from(data.toString('utf8'), 'base64').toString('utf8'));
          console.log('Decoded:', JSON.stringify(decoded, null, 2));
        } catch (e) {
          try {
            const decoded = JSON.parse(data.toString('utf8'));
            console.log('Decoded (raw):', JSON.stringify(decoded, null, 2));
          } catch (e2) {
            console.log('Parse error:', e2.message);
          }
        }
      });
      
      await statusChar.subscribeAsync();
      console.log('[BLE Test] ✓ Subscribed to notifications!');
      console.log('[BLE Test] Waiting for notifications... (Press Ctrl+C to exit)');
      
    } catch (error) {
      console.error('[BLE Test] ✗ Error:', error.message);
      console.error(error);
      process.exit(1);
    }
  }
});

process.on('SIGINT', () => {
  console.log('\n[BLE Test] Shutting down...');
  if (targetDevice) {
    targetDevice.disconnect();
  }
  process.exit(0);
});
