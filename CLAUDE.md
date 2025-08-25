# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-based Modbus RTU/TCP proxy/gateway specifically designed for Carlo Gavazzi ET112 energy meters used with Victron CerboGX systems. The device acts as a caching proxy with three simultaneous Modbus personalities: RTU Client (polls ET112), TCP Server, and RTU Server (serves cached data).

## Build Commands

```bash
# Build debug version (default)
pio run -e esp32debug

# Build release version
pio run -e esp32release

# Build and create git snapshot (auto-commits on success)
./build_snapshot.sh

# Upload firmware to ESP32
pio run -e esp32debug -t upload

# Monitor serial output
pio run -e esp32debug -t monitor

# Clean build files
pio run -t clean
```

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

## Git Integration

The project includes automatic git commit functionality:
- `scripts/pre_build.py`: Extracts git info for firmware versioning
- `scripts/post_build.py`: Auto-commits after successful builds
- Creates timestamped build tags automatically

## Hardware Configuration

- ESP32 DevKit V1 with optional 0.96" OLED (SSD1306)
- TTL to RS485 modules (one per Modbus RTU network)
- Power can be sourced from CerboGX RS485 USB cable (5V)
- UART2 pins for RS485 communication

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