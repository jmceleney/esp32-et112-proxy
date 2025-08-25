# WiFi Subsystem Requirements

## 1. Purpose
This document defines the functional and non-functional requirements for the WiFi subsystem of the ESP32 ET112 Modbus proxy device. The WiFi subsystem is critical for enabling remote configuration, monitoring, and serving cached Modbus data over TCP.

## 2. Functional Requirements

### 2.1 Connection Management

#### 2.1.1 Initial Configuration
- **FR-WIFI-001**: The system SHALL provide a method for initial WiFi credential configuration without requiring pre-existing network connectivity
- **FR-WIFI-002**: The system SHALL persist WiFi credentials across power cycles and reboots
- **FR-WIFI-003**: The system SHALL support WPA2 Personal authentication as a minimum
- **FR-WIFI-004**: The system SHALL store at least one set of WiFi credentials

#### 2.1.2 Connection Establishment
- **FR-WIFI-005**: The system SHALL automatically attempt connection to configured networks on boot
- **FR-WIFI-006**: The system SHALL verify successful connection before considering WiFi operational
- **FR-WIFI-007**: The system SHALL obtain and maintain a valid IP address via DHCP
- **FR-WIFI-008**: The system SHALL set and advertise a configurable hostname via mDNS

#### 2.1.3 Connection Monitoring
- **FR-WIFI-009**: The system SHALL continuously monitor WiFi connection status
- **FR-WIFI-010**: The system SHALL track signal strength (RSSI) of the active connection
- **FR-WIFI-011**: The system SHALL detect genuine disconnection events (not transient glitches)
- **FR-WIFI-012**: The system SHALL maintain timestamps of connection establishment and loss

### 2.2 Recovery and Resilience

#### 2.2.1 Automatic Recovery
- **FR-WIFI-013**: The system SHALL automatically attempt reconnection upon connection loss
- **FR-WIFI-014**: The system SHALL implement progressive recovery strategies for persistent failures
- **FR-WIFI-015**: The system SHALL NOT clear stored credentials during automatic recovery attempts
- **FR-WIFI-016**: The system SHALL limit reconnection frequency to prevent network flooding

#### 2.2.2 Manual Recovery
- **FR-WIFI-017**: The system SHALL provide a user-initiated method to clear stored credentials
- **FR-WIFI-018**: The system SHALL provide a user-initiated method to trigger reconnection
- **FR-WIFI-019**: The system SHALL enter configuration mode after user-initiated credential clearing

### 2.3 Status Reporting

#### 2.3.1 Visual Indicators
- **FR-WIFI-020**: The system SHALL display current SSID on the OLED display when connected
- **FR-WIFI-021**: The system SHALL display current IP address on the OLED display when connected
- **FR-WIFI-022**: The system SHALL indicate configuration mode status on the OLED display

#### 2.3.2 Web Interface
- **FR-WIFI-023**: The system SHALL expose current connection status via web interface
- **FR-WIFI-024**: The system SHALL expose signal strength metrics via web interface
- **FR-WIFI-025**: The system SHALL expose connection uptime via web interface
- **FR-WIFI-026**: The system SHALL provide WiFi quality assessment based on signal strength

#### 2.3.3 Metrics
- **FR-WIFI-027**: The system SHALL expose WiFi RSSI via Prometheus metrics endpoint
- **FR-WIFI-028**: The system SHALL log WiFi connection and disconnection events
- **FR-WIFI-029**: The system SHALL log recovery attempts and their outcomes

## 3. Non-Functional Requirements

### 3.1 Performance

- **NFR-WIFI-001**: WiFi operations SHALL NOT block Modbus data polling for more than 100ms
- **NFR-WIFI-002**: The system SHALL establish initial connection within 20 seconds of boot when credentials are stored
- **NFR-WIFI-003**: The system SHALL detect genuine disconnection within 10 seconds of occurrence
- **NFR-WIFI-004**: Signal strength monitoring SHALL occur at intervals no shorter than 30 seconds

### 3.2 Reliability

- **NFR-WIFI-005**: The system SHALL maintain Modbus cache availability during WiFi reconnection attempts
- **NFR-WIFI-006**: The system SHALL continue Modbus RTU operations during WiFi outages
- **NFR-WIFI-007**: The system SHALL NOT corrupt stored credentials during power loss
- **NFR-WIFI-008**: The system SHALL handle WiFi/UART hardware conflicts without data corruption

### 3.3 Compatibility

- **NFR-WIFI-009**: The system SHALL operate with 2.4GHz 802.11 b/g/n networks
- **NFR-WIFI-010**: The system SHALL coexist with AsyncWebServer on port 80
- **NFR-WIFI-011**: The system SHALL coexist with Modbus TCP server on port 502
- **NFR-WIFI-012**: The system SHALL support networks with hidden SSIDs

### 3.4 Resource Constraints

- **NFR-WIFI-013**: WiFi subsystem SHALL operate within ESP32 Core 0 when dual-core separation is required
- **NFR-WIFI-014**: WiFi operations SHALL NOT cause heap fragmentation leading to allocation failures
- **NFR-WIFI-015**: WiFi subsystem SHALL NOT consume more than 20KB of heap memory for internal buffers

### 3.5 Usability

- **NFR-WIFI-016**: Configuration portal SHALL be accessible without special software or drivers
- **NFR-WIFI-017**: Configuration portal SHALL be mobile-device friendly
- **NFR-WIFI-018**: WiFi status information SHALL be accessible without authentication
- **NFR-WIFI-019**: Manual recovery actions SHALL require explicit user confirmation

### 3.6 Maintainability

- **NFR-WIFI-020**: All WiFi state changes SHALL be logged with appropriate severity levels
- **NFR-WIFI-021**: WiFi subsystem SHALL provide diagnostic information for troubleshooting
- **NFR-WIFI-022**: Configuration portal SHALL display device identifying information (MAC, hostname)

## 4. Constraints

### 4.1 Hardware Constraints
- **CON-WIFI-001**: The system operates on ESP32 DevKit V1 hardware
- **CON-WIFI-002**: WiFi shares the ESP32's single 2.4GHz radio with no external antenna

### 4.2 Environmental Constraints
- **CON-WIFI-003**: The system must operate in industrial environments with potential RF interference
- **CON-WIFI-004**: The system must maintain operation with WiFi signal strength as low as -80dBm

### 4.3 Integration Constraints
- **CON-WIFI-005**: WiFi must coexist with Hardware Serial2 UART operations
- **CON-WIFI-006**: WiFi must not interfere with 30-second watchdog timer operation
- **CON-WIFI-007**: WiFi recovery must not trigger the 60-second no-data reboot mechanism

## 5. Quality Attributes

### 5.1 Availability
The WiFi subsystem shall maintain 99% availability when within range of a configured access point with signal strength better than -80dBm.

### 5.2 Recoverability
The WiFi subsystem shall recover from transient network failures within 30 seconds and from persistent failures within 5 minutes without manual intervention.

### 5.3 Observability
The WiFi subsystem shall provide sufficient logging and metrics to diagnose connection issues without requiring additional debug builds or serial console access.

## 6. Acceptance Criteria

### 6.1 Basic Operation
- Device connects to configured network within 20 seconds of boot
- Device maintains stable connection for 24 hours under normal conditions
- Device recovers from AP reboot within 2 minutes

### 6.2 Failure Scenarios
- Device recovers from temporary RF interference within 1 minute
- Device handles AP channel changes without manual intervention
- Device continues Modbus operations during WiFi outage

### 6.3 User Experience
- Configuration portal accessible within 30 seconds of credential reset
- Status page shows accurate real-time connection information
- Manual recovery completes within 30 seconds of user action