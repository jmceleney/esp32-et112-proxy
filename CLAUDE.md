# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based Modbus RTU/TCP proxy/gateway specifically designed for Carlo Gavazzi ET112 energy meters used with Victron CerboGX systems. The device acts as a caching proxy with three simultaneous Modbus personalities: RTU Client (polls ET112), TCP Server, and RTU Server (serves cached data).

## Build Commands

```bash
# Build debug version (automatically includes filesystem)
pio run -e esp32debug

# Build release version (automatically includes filesystem)
pio run -e esp32release

# Upload firmware to ESP32
pio run -e esp32debug -t upload

# Upload filesystem (web files) to ESP32 - REQUIRED for web UI
pio run -e esp32debug -t uploadfs

# Complete build and upload (firmware + filesystem)
pio run -e esp32debug -t upload && pio run -e esp32debug -t uploadfs

# Monitor serial output
pio run -e esp32debug -t monitor

# Clean build files
pio run -t clean
```

### Web Interface

The ESP32 serves a modern Preact-based web interface with:
- Real-time power monitoring dashboard at `/` (or `/app`)
- Auto-updating metrics (Watts, Amps, Volts) refreshing every 2 seconds
- Modern responsive design with mobile support  
- All configuration and debug interfaces accessible from navigation

**Important**: The build system automatically ensures the filesystem is up-to-date with your latest changes. Just run `pio run -e esp32debug` and both firmware and filesystem will be built with matching timestamps.

The web files are served from the ESP32's LittleFS filesystem, not embedded in firmware.

## Upgrading Older Devices

For devices running old firmware without the modern web interface, use the Node.js upload tool for a two-stage upgrade:

1. **Build firmware (filesystem included automatically):**
   ```bash
   pio run -e esp32release
   ```

2. **Upload firmware first:**
   ```bash
   node scripts/upload_device.js firmware <device-ip> .pio/build/esp32release/firmware.bin
   ```
   Device will reboot with new OTA system but no web interface.

3. **Upload filesystem:**
   ```bash
   node scripts/upload_device.js filesystem <device-ip> .pio/build/esp32release/littlefs.bin
   ```
   Device reboots with complete modern interface.

After upgrade, devices can accept combined firmware files (`firmware_combined.bin`) for single-step updates.

## Architecture

### Core Components

**ModbusCache** (`include/ModbusCache.h`, `src/ModbusCache.cpp`)
- Central caching layer managing all Modbus register data
- Handles concurrent access from RTU/TCP clients and servers
- Implements automatic cache invalidation (2-second timeout)
- Manages polling intervals and latency metrics
- Thread-safe with mutex protection for token management

**Main Application** (`src/main.cpp`)
- Initializes WiFi, OLED display, web server, and all Modbus interfaces
- Defines ET112 register mappings (dynamic and static registers)
- Implements watchdog timer (30-second timeout before reboot)
- Manages OTA updates and mDNS service

**Configuration** (`include/config.h`, `src/config.cpp`)
- Persistent storage using ESP32 Preferences
- Manages primary/secondary Modbus configurations
- WiFi settings and polling intervals
- Debug buffer for troubleshooting

**Web Interface** (`include/pages.h`, `src/pages.cpp`)
- AsyncWebServer on port 80
- Configuration pages for primary/secondary Modbus settings
- Real-time register display and debug interface
- Prometheus metrics endpoint at `/metrics`
- Hidden baud rate configuration at `/baudrate`

### Modbus Register Mappings

The system tracks specific ET112 registers required by Victron CerboGX:
- Voltage, Current, Power (Active/Reactive/Apparent)
- Energy meters (import/export kWh)
- Power factor, Frequency
- Demand values

Register addresses are defined in `dynamicRegisters` and `staticRegisters` vectors in main.cpp.

## Key Implementation Details

### Timing and Millis
Critical: Never cache `millis()` for timestamp comparisons. Always use fresh `millis()` calls when calculating time differences to avoid race conditions.

### Modbus Communication
- Hardware Serial2 (UART2) for RTU communication
- Configurable baud rates (default 9600, recommended 38400)
- Automatic flow control support with XY-017 RS485 modules
- TCP server on standard Modbus port 502

### WiFi Management
- WiFiManager for initial setup (creates AP if no config)
- Automatic reconnection on WiFi loss
- mDNS support for hostname-based access

### Debug Features
- Circular debug buffer accessible via web interface
- Configurable log levels (LOG_LEVEL define)
- Serial output redirectable via REROUTE_DEBUG

## Build System

The project uses an intelligent build system with automatic timestamp synchronization:

### Pre-build (`scripts/pre_build.py`):
- Determines build timestamp from most recent source file modification
- Generates `version.h` and `data/web/version.json` with identical timestamps
- Extracts git information for firmware versioning
- Eliminates timestamp inconsistencies across multi-stage builds

### Post-build (`scripts/create_combined_firmware.py`):
- Automatically builds filesystem if missing or outdated
- Creates combined firmware+filesystem binary for atomic updates
- Ensures filesystem timestamp matches firmware timestamp
- Copies combined binary to project root for easy access

### Key Features:
- **One command builds everything** - No manual `buildfs` required
- **Timestamp consistency** - Firmware and filesystem always match
- **Smart rebuilding** - Only rebuilds filesystem when source files change
- **Atomic updates** - Combined binaries prevent version mismatches

## Hardware Configuration

- ESP32 DevKit V1 with optional 0.96" OLED (SSD1306)
- TTL to RS485 modules (one per Modbus RTU network)
- Power can be sourced from CerboGX RS485 USB cable (5V)
- UART2 pins for RS485 communication

## Remote OTA Upload Tool

A Node.js script is available for uploading firmware and filesystem images to ESP32 devices via HTTP, useful for devices with broken or missing web interfaces.

### Installation
```bash
npm install  # Install required dependencies (form-data, node-fetch)
```

### Usage
```bash
# Upload firmware
node scripts/upload_device.js firmware <device-ip> <firmware-file>

# Upload filesystem
node scripts/upload_device.js filesystem <device-ip> <filesystem-file>

# Examples
node scripts/upload_device.js firmware 192.168.1.100 .pio/build/esp32release/firmware.bin
node scripts/upload_device.js filesystem 192.168.1.100 .pio/build/esp32release/littlefs.bin

# For modern devices (single-step update)
node scripts/upload_device.js firmware 192.168.1.100 firmware_combined.bin
```

### Features
- Works with both legacy and current firmware versions
- Shows upload progress percentage
- Handles device reboots automatically
- Clear error messages for troubleshooting
- Supports both firmware (`/update`) and filesystem (`/upload-filesystem`) endpoints

### Use Cases
1. **Recovering devices with broken web interfaces** - Upload new firmware/filesystem without web UI
2. **Batch updates** - Script can be integrated into automation tools
3. **Older device upgrades** - Two-stage upgrade process for devices without modern web interface
4. **Development/testing** - Quick command-line firmware deployment

## Testing

No automated test framework configured. Testing requires:
1. Physical ET112 meter or Modbus simulator
2. Verification via web interface register display
3. Monitoring debug output for communication errors

## Dependencies

Key libraries (managed via PlatformIO):
- WiFiManager: AP configuration portal
- ESPAsyncWebServer: Async web server
- eModbus: Modbus protocol implementation
- ArduinoJson: Configuration serialization
- U8g2: OLED display driver