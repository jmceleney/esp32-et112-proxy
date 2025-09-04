# ESP32 Device Upgrade Guide

## Overview

This guide walks you through upgrading older ESP32 devices to the modern web interface using the Node.js upload tool.

⚠️ **IMPORTANT**: Older devices require a two-stage upgrade process using the command-line upload tool.

## What You'll Need

- Legacy ESP32 device with working web interface
- Computer with PlatformIO installed  
- Device IP address or hostname
- Reliable WiFi connection

## Build Files Required

### Build firmware (filesystem included automatically)
```bash
# Build firmware - filesystem built automatically with matching timestamp
pio run -e esp32release
```
**Creates**: 
- `.pio/build/esp32release/firmware.bin` (1.1MB)
- `.pio/build/esp32release/littlefs.bin` (1.4MB)
- `.pio/build/esp32release/firmware_combined.bin` (3.9MB)
- `firmware_combined.bin` (copy in project root)

## Two-Stage Upgrade Process

### Stage 1: Upload Firmware

1. **Use Node.js upload tool**
   ```bash
   node scripts/upload_device.js firmware <device-ip> .pio/build/esp32release/firmware.bin
   ```
   - Replace `<device-ip>` with your device's IP address
   - Tool shows upload progress and handles errors
   - Device will reboot (takes ~30 seconds)

3. **Verify Stage 1 success**
   - Device should reconnect to WiFi
   - Access device IP - you'll see basic web interface  
   - ✅ Stage 1 complete when device is accessible

### Stage 2: Upload Filesystem

1. **Access device after reboot**
   - Open browser to same IP address
   - Device **automatically redirects** to `/filesystem-upload`
   - No need to find special URLs!

2. **Upload filesystem**
   ```bash
   node scripts/upload_device.js filesystem <device-ip> .pio/build/esp32release/littlefs.bin
   ```
   - Tool shows upload progress
   - Wait for upload completion (~2-3 minutes)

3. **Final reboot**
   - Device reboots automatically after upload
   - Takes ~45 seconds for full initialization
   - ✅ Upgrade complete!

## Verification

After successful upgrade:

1. **Visit device IP address**
   - Should load modern Preact dashboard
   - Real-time power monitoring display
   - Mobile-responsive interface

2. **Check all features work**
   - Navigation menu functions
   - Configuration pages accessible  
   - Debug logs available
   - OTA updates work with combined files

## Troubleshooting

### Stage 1 Issues

**Problem**: Device doesn't reboot after firmware upload
- **Solution**: Wait 2-3 minutes, then power cycle device manually

**Problem**: Can't access device after Stage 1
- **Solution**: Check WiFi connection, try IP scanner to find device

### Stage 2 Issues

**Problem**: Not redirected to upload page  
- **Solution**: Manually visit `http://[device-ip]/filesystem-upload`

**Problem**: Upload fails or times out
- **Solution**: Check file size (should be ~1.4MB), retry upload

**Problem**: Device not accessible after Stage 2
- **Solution**: Wait full 60 seconds, power cycle if needed

### Recovery Options

**If device becomes unresponsive:**
1. Power cycle device
2. If still unresponsive, connect via USB/serial
3. Re-flash using esptool: `esptool.py write_flash 0x0 firmware.bin`

## File Locations

After running build commands, files are located at:

```
📁 .pio/build/esp32release/
  ├── 📄 firmware.bin        ← Stage 1 upload file
  └── 📄 littlefs.bin        ← Stage 2 upload file
```

## Future Updates

Once upgraded, devices accept **combined firmware** files for single-step updates:

```bash
# Build creates combined firmware automatically
pio run -e esp32release
# Creates: firmware_combined.bin (ready to upload)
```

Upload `firmware_combined.bin` via the modern web interface or Node.js upload tool for atomic firmware+filesystem updates.

## Expected Timeline

- **Stage 1**: 5-10 minutes (build + upload + reboot)  
- **Stage 2**: 10-15 minutes (build + upload + reboot)
- **Total time**: 15-25 minutes per device

## Tips for Multiple Devices

1. **Build once for all devices**
   ```bash
   pio run -e esp32release  # Creates all needed files automatically
   ```

2. **Upgrade devices sequentially**
   - Complete both stages on Device 1
   - Then move to Device 2, etc.
   - Don't start Stage 2 on multiple devices simultaneously

3. **Keep device inventory**
   - Track IP addresses and upgrade status
   - Test each device's functionality before moving to next

---

## Success Indicators ✅

**Stage 1 Complete**: Device shows basic web interface at IP address
**Stage 2 Complete**: Device shows modern Preact dashboard with real-time data
**Full Success**: All configuration and monitoring features work correctly

The upgrade is **irreversible** but provides significant functionality improvements and a modern web interface experience.