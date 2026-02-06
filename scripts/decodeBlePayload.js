#!/usr/bin/env node
const fs = require('fs');
const path = require('path');
const Ajv = require('ajv');

const schemaPath = path.resolve(__dirname, '..', 'docs', 'ble_payload_schema.json');
let schema = null;
try {
  schema = JSON.parse(fs.readFileSync(schemaPath, 'utf8'));
} catch (error) {
  console.error('Failed to load BLE schema. Run "npm run export:ble-schema" first.');
  console.error(error.message);
  process.exit(1);
}

const args = process.argv.slice(2);
let rawInput = null;
let inputSource = null;
const leftovers = [];

const showUsage = () => {
  console.log('Usage: npm run decode:ble-payload -- (--value <base64> | --file <path>)');
  console.log('  --value, -v   Base64 payload captured from BLE notifications');
  console.log('  --file,  -f   Path to a file containing the Base64 payload');
  console.log('If the input is already JSON, the script will parse it directly.');
};

for (let i = 0; i < args.length; i += 1) {
  const arg = args[i];
  if (arg === '--value' || arg === '-v') {
    rawInput = args[i + 1];
    i += 1;
    inputSource = 'value';
  } else if (arg === '--file' || arg === '-f') {
    const provided = args[i + 1];
    if (!provided) {
      console.error('Missing file path after --file.');
      process.exit(1);
    }
    const absolutePath = path.resolve(process.cwd(), provided);
    if (!fs.existsSync(absolutePath)) {
      console.error(`File not found: ${absolutePath}`);
      process.exit(1);
    }
    rawInput = fs.readFileSync(absolutePath, 'utf8').trim();
    inputSource = absolutePath;
    i += 1;
  } else if (arg === '--help' || arg === '-h') {
    showUsage();
    process.exit(0);
  } else if (arg === '--') {
    continue;
  } else {
    leftovers.push(arg);
  }
}

if (!rawInput && leftovers.length > 0) {
  const candidate = leftovers[0];
  const absolutePath = path.resolve(process.cwd(), candidate);
  if (fs.existsSync(absolutePath)) {
    rawInput = fs.readFileSync(absolutePath, 'utf8').trim();
    inputSource = absolutePath;
  } else {
    rawInput = candidate;
    inputSource = 'value';
  }
}

if (!rawInput) {
  console.error('No payload provided.');
  showUsage();
  process.exit(1);
}

const normalizeBase64 = (value) => value.replace(/\s+/g, '').replace(/=+$/, '');

const tryDecodeBase64 = (value) => {
  try {
    const buffer = Buffer.from(value, 'base64');
    if (normalizeBase64(buffer.toString('base64')) === normalizeBase64(value)) {
      return buffer.toString('utf8');
    }
  } catch (error) {
    return null;
  }
  return null;
};

let decodedJson = tryDecodeBase64(rawInput.trim());
if (!decodedJson) {
  decodedJson = rawInput;
}

let payload;
try {
  payload = JSON.parse(decodedJson);
} catch (error) {
  console.error('Input is neither valid Base64 JSON nor plain JSON.');
  console.error(error.message);
  process.exit(1);
}

const ajv = new Ajv({ allErrors: true, strict: false });
const validate = ajv.compile(schema);
const valid = validate(payload);

if (!valid) {
  console.error('Payload decoded but failed schema validation:');
  for (const err of validate.errors ?? []) {
    console.error(`- ${err.instancePath || '/'} ${err.message}`);
  }
  process.exit(1);
}

console.log(`Decoded payload (${inputSource ?? 'value'}):`);
console.log(JSON.stringify(payload, null, 2));
process.exit(0);
