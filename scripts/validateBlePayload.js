#!/usr/bin/env node
const fs = require('fs');
const path = require('path');
const Ajv = require('ajv');

const schemaPath = path.resolve(__dirname, '..', 'docs', 'ble_payload_schema.json');
const schemaExists = fs.existsSync(schemaPath);
if (!schemaExists) {
  console.error('BLE payload schema not found. Run "npm run export:ble-schema" first.');
  process.exit(1);
}

const schema = JSON.parse(fs.readFileSync(schemaPath, 'utf8'));
const ajv = new Ajv({ allErrors: true, strict: false });
const validate = ajv.compile(schema);

const [, , payloadPath] = process.argv;
if (!payloadPath) {
  console.error('Usage: npm run validate:ble-payload -- <path-to-payload.json>');
  process.exit(1);
}

const resolvedPayloadPath = path.resolve(process.cwd(), payloadPath);
if (!fs.existsSync(resolvedPayloadPath)) {
  console.error(`Payload file not found: ${resolvedPayloadPath}`);
  process.exit(1);
}

const payload = JSON.parse(fs.readFileSync(resolvedPayloadPath, 'utf8'));
const isValid = validate(payload);

if (isValid) {
  console.log('Payload matches BleStatusPayload schema.');
  process.exit(0);
}

console.error('Payload failed schema validation:');
for (const error of validate.errors ?? []) {
  console.error(`- ${error.instancePath || '/'} ${error.message}`);
}
process.exit(1);
