# Quick BLE test using Windows.Devices.Bluetooth
# Run with: powershell -ExecutionPolicy Bypass -File scripts\testBleWindows.ps1

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(-1) | Out-Null
    $netTask.Result
}

Write-Host "[BLE Test] Loading Windows Bluetooth APIs..." -ForegroundColor Cyan

[Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristic, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
[Windows.Storage.Streams.DataReader, Windows.Storage.Streams, ContentType = WindowsRuntime] | Out-Null

$SERVICE_UUID = "0000a11c-0000-1000-8000-00805f9b34fb"
$STATUS_CHAR_UUID = "0000a11d-0000-1000-8000-00805f9b34fb"

Write-Host "[BLE Test] Scanning for TLTB Controller..." -ForegroundColor Cyan

$selector = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelectorFromDeviceName("TLTB Controller")
$devices = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])

if ($devices.Count -eq 0) {
    Write-Host "[BLE Test] No TLTB Controller found. Make sure:" -ForegroundColor Red
    Write-Host "  1. ESP32 is powered and advertising" -ForegroundColor Yellow
    Write-Host "  2. Bluetooth is enabled on this PC" -ForegroundColor Yellow
    Write-Host "  3. Device name is exactly 'TLTB Controller'" -ForegroundColor Yellow
    exit 1
}

Write-Host "[BLE Test] Found $($devices.Count) device(s)" -ForegroundColor Green

$deviceInfo = $devices[0]
Write-Host "[BLE Test] Connecting to: $($deviceInfo.Name) ($($deviceInfo.Id))" -ForegroundColor Cyan

try {
    $device = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromIdAsync($deviceInfo.Id)) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
    
    if ($device -eq $null) {
        Write-Host "[BLE Test] Failed to connect to device" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "[BLE Test] Connected! Status: $($device.ConnectionStatus)" -ForegroundColor Green
    
    Write-Host "[BLE Test] Discovering GATT services..." -ForegroundColor Cyan
    $servicesResult = Await ($device.GetGattServicesAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult])
    
    Write-Host "[BLE Test] Found $($servicesResult.Services.Count) services" -ForegroundColor Green
    
    $targetService = $servicesResult.Services | Where-Object { $_.Uuid -eq $SERVICE_UUID }
    
    if ($targetService -eq $null) {
        Write-Host "[BLE Test] Service $SERVICE_UUID not found!" -ForegroundColor Red
        Write-Host "[BLE Test] Available services:" -ForegroundColor Yellow
        foreach ($svc in $servicesResult.Services) {
            Write-Host "  - $($svc.Uuid)" -ForegroundColor Yellow
        }
        exit 1
    }
    
    Write-Host "[BLE Test] Found TLTB service" -ForegroundColor Green
    
    Write-Host "[BLE Test] Discovering characteristics..." -ForegroundColor Cyan
    $charsResult = Await ($targetService.GetCharacteristicsAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult])
    
    Write-Host "[BLE Test] Found $($charsResult.Characteristics.Count) characteristics" -ForegroundColor Green
    
    $statusChar = $charsResult.Characteristics | Where-Object { $_.Uuid -eq $STATUS_CHAR_UUID }
    
    if ($statusChar -eq $null) {
        Write-Host "[BLE Test] Status characteristic not found!" -ForegroundColor Red
        exit 1
    }
    
    Write-Host "[BLE Test] Found status characteristic" -ForegroundColor Green
    Write-Host "[BLE Test] Properties: $($statusChar.CharacteristicProperties)" -ForegroundColor Cyan
    
    # Read initial value
    Write-Host "[BLE Test] Reading initial value..." -ForegroundColor Cyan
    $readResult = Await ($statusChar.ReadValueAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattReadResult])
    
    if ($readResult.Status -eq [Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]::Success) {
        $reader = [Windows.Storage.Streams.DataReader]::FromBuffer($readResult.Value)
        $bytes = New-Object byte[] $readResult.Value.Length
        $reader.ReadBytes($bytes)
        
        Write-Host "[BLE Test] Read $($bytes.Length) bytes" -ForegroundColor Green
        $base64 = [Convert]::ToBase64String($bytes)
        Write-Host "[BLE Test] Base64: $($base64.Substring(0, [Math]::Min(80, $base64.Length)))..." -ForegroundColor Cyan
        
        try {
            $jsonBytes = [Convert]::FromBase64String([System.Text.Encoding]::UTF8.GetString($bytes))
            $json = [System.Text.Encoding]::UTF8.GetString($jsonBytes)
            $parsed = $json | ConvertFrom-Json
            Write-Host "[BLE Test] Decoded JSON:" -ForegroundColor Green
            Write-Host ($parsed | ConvertTo-Json -Depth 10) -ForegroundColor White
        } catch {
            Write-Host "[BLE Test] Failed to decode: $_" -ForegroundColor Red
            Write-Host "[BLE Test] Raw UTF-8: $([System.Text.Encoding]::UTF8.GetString($bytes))" -ForegroundColor Yellow
        }
    } else {
        Write-Host "[BLE Test] Read failed: $($readResult.Status)" -ForegroundColor Red
    }
    
    # Subscribe to notifications
    Write-Host "[BLE Test] Subscribing to notifications..." -ForegroundColor Cyan
    $cccdResult = Await ($statusChar.WriteClientCharacteristicConfigurationDescriptorAsync([Windows.Devices.Bluetooth.GenericAttributeProfile.GattClientCharacteristicConfigurationDescriptorValue]::Notify)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus])
    
    if ($cccdResult -eq [Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]::Success) {
        Write-Host "[BLE Test] Subscribed successfully!" -ForegroundColor Green
        
        $eventHandler = {
            param($sender, $args)
            $result = Await ($sender.ReadValueAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattReadResult])
            if ($result.Status -eq [Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]::Success) {
                $reader = [Windows.Storage.Streams.DataReader]::FromBuffer($result.Value)
                $bytes = New-Object byte[] $result.Value.Length
                $reader.ReadBytes($bytes)
                
                Write-Host "`n[BLE Test] Notification received ($($bytes.Length) bytes)" -ForegroundColor Green
                try {
                    $jsonBytes = [Convert]::FromBase64String([System.Text.Encoding]::UTF8.GetString($bytes))
                    $json = [System.Text.Encoding]::UTF8.GetString($jsonBytes)
                    $parsed = $json | ConvertFrom-Json
                    Write-Host ($parsed | ConvertTo-Json -Compress) -ForegroundColor White
                } catch {
                    Write-Host "[BLE Test] Decode error: $_" -ForegroundColor Red
                }
            }
        }
        
        Register-ObjectEvent -InputObject $statusChar -EventName ValueChanged -Action $eventHandler | Out-Null
        
        Write-Host "[BLE Test] Listening for notifications... (Press Ctrl+C to exit)" -ForegroundColor Cyan
        
        while ($true) {
            Start-Sleep -Seconds 1
        }
    } else {
        Write-Host "[BLE Test] Failed to subscribe: $cccdResult" -ForegroundColor Red
    }
    
} catch {
    Write-Host "[BLE Test] Error: $_" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
} finally {
    if ($device) {
        $device.Dispose()
    }
}
