#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Logging.h>
#include <U8g2lib.h>
#include "ModbusCache.h"
#include "config.h"
#include "pages.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_wifi.h"  // For esp_wifi_restore()
#include <cmath> // Include cmath for acos

#ifdef REROUTE_DEBUG
EspSoftwareSerial::UART debugSerial;
#else
#define debugSerial Serial
#endif

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
AsyncWebServer webServer(80);
Config config;
Preferences prefs;
WiFiManager wm;

std::vector<ModbusRegister> dynamicRegisters = {
    {0, RegisterType::INT32, "Volts", 0.1, UnitType::V},
    {2, RegisterType::INT32, "Amps", 0.001, UnitType::A},
    {4, RegisterType::INT32, "Watts", 0.1, UnitType::W},
    {6, RegisterType::INT32, "VA", 0.1, UnitType::VA},
    {8, RegisterType::INT32, "Volt Amp Reactive", 0.1, UnitType::var},
    {10, RegisterType::INT32, "W Demand", 0.1, UnitType::W},
    {12, RegisterType::INT32, "W Demand Peak", 0.1, UnitType::W},
    {14, RegisterType::INT16, "Power Factor", 0.001, UnitType::PF},
    {15, RegisterType::INT16, "Frequency", 0.1, UnitType::Hz},
    {16, RegisterType::INT32, "Energy kWh (+)", 0.1, UnitType::KWh},
    {18, RegisterType::INT32, "Reactive Power Kvarh (+)", 0.1, UnitType::KVarh},
    {20, RegisterType::INT32, "kWh (+) PARTIAL", 0.1, UnitType::KWh},
    {22, RegisterType::INT32, "Kvarh (+) PARTIAL", 0.1, UnitType::KVarh},
    {32, RegisterType::INT32, "Energy kWh (-)", 0.1, UnitType::KWh},
    {34, RegisterType::INT32, "Reactive Power Kvarh (-)", 0.1, UnitType::KVarh},
};

std::vector<ModbusRegister> staticRegisters = {
    {11, RegisterType::INT16, "Carlo Gavazzi Controls identification code"},
    {770, RegisterType::UINT16, "Version"},
    {771, RegisterType::UINT16, "Revision"},
    {4112, RegisterType::UINT32, "Integration Time for dmd calc"},
    {4355, RegisterType::INT16, "Measurement mode"},
    {8193, RegisterType::UINT16, "RS485 baud rate"},
    {20480, RegisterType::UINT16, "Serial number 1"},
    {20481, RegisterType::UINT16, "Serial number 2"},
    {20482, RegisterType::UINT16, "Serial number 3"},
    {20483, RegisterType::UINT16, "Serial number 4"},
    {20484, RegisterType::UINT16, "Serial number 5"},
    {20485, RegisterType::UINT16, "Serial number 6"},
    {20486, RegisterType::UINT16, "Serial number 7"},
};

/*
Easton SDM120 registers
Modicom, Parameter, Length(bytes), units, format, high, low
30001,Voltage,4,V,Float,00,00
30007,Current,4,A,Float,00,06
30013,Active power,4,W,Float,00,0C
30019,Apparent power,4,VA,Float,00,12
30025,Reactive power,4,VAr,Float,00,18
30031,Power factor,4,None,Float,00,1E
30071,Frequency,4,Hz,Float,00,46
30073,Import active energy,4,kWh,Float,00,48
30075,Export active energy,4,kWh,Float,00,4A
30077,Import reactive energy,4,kvarh,Float,00,4C
30079,Export reactive energy,4,kvarh,Float,00,4E
30085,Total system power demand,4,W,Float,00,54
30087,Maximum total system power demand,4,W,Float,00,56
30089,Import system power demand,4,W,Float,00,58
30091,Maximum Import system power demand,4,W,Float,00,5A
30093,Export system power demand,4,W,Float,00,5C
30095,Maximum Export system power demand,4,W,Float,00,5E
30259,current demand,4,A,Float,01,02
30265,Maximum current demand,4,A,Float,01,08
30343,Total active energy,4,kWh,Float,01,56
30345,Total reactive energy,4,Kvarh,Float,01,58

*/
// Now we make a map from the SDM120 back to the main registers above

#ifdef SDM120

std::function<double(ModbusCache*, double)> calc_angle = [](ModbusCache* modbusCache, double param){
  dbgln("calc_angle for power factor: " + String(param,4));
  return static_cast<double>(acos(param) * (180.0 / M_PI));
};

std::function<double(ModbusCache*, double)> calc_total_energy = [](ModbusCache* modbusCache, double param){
  float totalImportEnergy = modbusCache->getRegisterScaledValue(16);
  float totalExportEnergy = modbusCache->getRegisterScaledValue(32);
  return static_cast<double>(totalImportEnergy + totalExportEnergy);
};

std::function<double(ModbusCache*, double)> calc_total_reactive = [](ModbusCache* modbusCache, double param){
  float totalImport = modbusCache->getRegisterScaledValue(18);
  float totalExport = modbusCache->getRegisterScaledValue(34);
  return static_cast<double>(totalImport + totalExport);
};

std::function<double(ModbusCache*, double)> invert_sign = [](ModbusCache* modbusCache, double param){
  return 0 - param;
};

std::vector<ModbusRegister> sdm120Registers = {
  {0, RegisterType::FLOAT, "Volts", 1, UnitType::V, 0},
  {6, RegisterType::FLOAT, "Amps", 1, UnitType::A, 2},
  {12, RegisterType::FLOAT, "Watts", 1, UnitType::W, 4},
  {18, RegisterType::FLOAT, "VA", 1, UnitType::VA, 6},
  {24, RegisterType::FLOAT, "Volt Amp Reactive", 1, UnitType::var, 8},
  {30, RegisterType::FLOAT, "Power Factor", 1, UnitType::PF, 14},
  {36, RegisterType::FLOAT, "Phase Angle", 1, UnitType::PF, 14, calc_angle},
  {70, RegisterType::FLOAT, "Frequency", 1, UnitType::Hz, 15},
  {72, RegisterType::FLOAT, "Energy kWh (+)", 1, UnitType::KWh, 16},
  {74, RegisterType::FLOAT, "Energy kWh (-)", 1, UnitType::KWh, 32, invert_sign},
  {76, RegisterType::FLOAT, "Reactive Power Kvarh (+)", 1, UnitType::KVarh, 18},
  {78, RegisterType::FLOAT, "Reactive Power Kvarh (-)", 1, UnitType::KVarh, 34, invert_sign},
  {84, RegisterType::FLOAT, "W Demand", 1, UnitType::W, 10},
  {86, RegisterType::FLOAT, "W Demand Peak", 1, UnitType::W, 12},
  {88, RegisterType::FLOAT, "kWh (+) PARTIAL", 1, UnitType::KWh, 20},
  {90, RegisterType::FLOAT, "Kvarh (+) PARTIAL", 1, UnitType::KVarh, 22},
  {92, RegisterType::FLOAT, "kWh (-) PARTIAL", 1, UnitType::KWh, 34, invert_sign},
  {342, RegisterType::FLOAT, "kWh Energy Total", 1, UnitType::KWh, 16, calc_total_energy},
  {344, RegisterType::FLOAT, "Reactive Power Total", 1, UnitType::KVarh, 18, calc_total_reactive},
};

#endif

String serverIPStr;
uint16_t serverPort;
ModbusCache *modbusCache = nullptr;
int wattsRegisterAddress = -1;
volatile int displayMode = 0;
const int buttonPin = 13;
// Debounce variables
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50; // 50ms debounce delay
int lastButtonState = HIGH; // Assume button not pressed at start
int buttonState;

#define WIFI_RSSI_THRESHOLD -80  // RSSI threshold to trigger reconnection (dBm)
#define WIFI_CHECK_INTERVAL 300000 // Check WiFi signal strength every 5 minutes
#define WIFI_CONNECTION_VERIFY_COUNT 3 // Number of times to verify WiFi is actually disconnected

// Add WiFi event handler to track connection status
bool wifiConnected = false;
bool wifiDisconnectDetected = false;
unsigned long lastWiFiConnectionTime = 0; // Track when WiFi was last connected

void WiFiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            dbgln("[WiFi] Connected to AP");
            wifiConnected = true;
            wifiDisconnectDetected = false;
            lastWiFiConnectionTime = millis(); // Record connection time
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            dbgln("[WiFi] Disconnected from AP");
            wifiDisconnectDetected = true;
            wifiConnected = false;
            lastWiFiConnectionTime = 0; // Reset connection time on disconnection
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            dbgln("[WiFi] Got IP: " + WiFi.localIP().toString());
            wifiConnected = true;
            if (lastWiFiConnectionTime == 0) { // If not set by CONNECTED event
                lastWiFiConnectionTime = millis(); // Record connection time
            }
            break;
        case ARDUINO_EVENT_WIFI_SCAN_DONE:
            dbgln("[WiFi] Scan completed");
            break;
        default:
            break;
    }
}

// Callback for when we enter Access Point mode
void configModeCallback(WiFiManager *myWiFiManager) {
    dbgln("[WiFiManager] Entered config mode");
    dbgln("AP SSID: " + myWiFiManager->getConfigPortalSSID());
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 14, myWiFiManager->getConfigPortalSSID().c_str());
    u8g2.setFont(u8g2_font_ncenB10_tr);  // Slightly smaller
    u8g2.drawStr(0, 32, "Setup Wifi");
    u8g2.sendBuffer();
}

void handleButton() {
  // Read the button state
  int reading = digitalRead(buttonPin);

  // Check if the button state has changed
  if (reading != lastButtonState) {
    lastDebounceTime = millis(); // Reset the debounce timer
  }

  // If the debounce delay has passed and the state is stable
  if ((millis() - lastDebounceTime) > debounceDelay) {
    // If the state is different from the previous stable state
    if (reading != buttonState) {
      buttonState = reading;

      // Only act on the falling edge (button pressed)
      if (buttonState == LOW) {
        // Increment mode and wrap around
        displayMode = (displayMode + 1) % 4;

        // Print the current mode for debugging
        Serial.print("displayMode changed to: ");
        Serial.println(displayMode);
      }
    }
  }

  // Save the reading for next loop
  lastButtonState = reading;
}

void updateDisplay() {
    dbgln("Updating display...");
    if (wattsRegisterAddress != -1) {
        // Get the "Watts" value from the Modbus cache
        float wattsValue = modbusCache->getRegisterScaledValue(wattsRegisterAddress);
        String ssidString = "SSID: " + WiFi.SSID();
        String ipString = "IP: " + WiFi.localIP().toString();

        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_ncenB14_tr);

        // Display the wattage if data is operational, otherwise show "No Data"
        if (modbusCache->getIsOperational()) {
            String wattsString = String(wattsValue, 1) + " W";
            u8g2.drawStr(0, 16, wattsString.c_str());
        } else {
            u8g2.drawStr(0, 16, "No data");
        }

        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(0, 30, "Grid Power");

        // Display WiFi SSID and IP address
        u8g2.drawStr(0, 45, ssidString.c_str());
        u8g2.drawStr(0, 60, ipString.c_str());

        u8g2.sendBuffer();
    }
}

// Function to completely reset WiFi configuration and connect to the strongest AP
bool forceWiFiReset() {
    dbgln("[WiFi] Performing WiFi reset due to connection issues...");
    
    // Disconnect from current network
    WiFi.disconnect(true);  // true = disable and clear credentials
    delay(500);
    
    // Clear WiFi settings from NVS to avoid BSSID fixation
    // This is only done when there's an actual connection issue
    esp_wifi_restore();
    delay(500);
    
    // Set WiFi mode to STA
    WiFi.mode(WIFI_STA);
    delay(500);
    
    // Get SSID and password from WiFiManager
    String ssid = wm.getWiFiSSID();
    String password = wm.getWiFiPass();
    
    if (ssid.length() == 0 || password.length() == 0) {
        dbgln("[WiFi] No SSID or password available, cannot reset WiFi");
        return false;
    }
    
    // Try direct connection first without scanning
    dbgln("[WiFi] Attempting direct connection to " + ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Wait briefly for connection
    int quickAttempts = 0;
    while (WiFi.status() != WL_CONNECTED && quickAttempts < 10) {
        delay(300);
        dbg(".");
        quickAttempts++;
    }
    
    // If direct connection succeeded, return success
    if (WiFi.status() == WL_CONNECTED) {
        dbgln("\n[WiFi] Successfully connected to " + WiFi.SSID() + 
              " with RSSI " + String(WiFi.RSSI()) + "dBm");
        return true;
    }
    
    // Only scan if direct connection failed
    dbgln("\n[WiFi] Direct connection failed, scanning for networks...");
    int numNetworks = WiFi.scanNetworks(false, true);  // async=false, show_hidden=true
    
    if (numNetworks <= 0) {
        dbgln("[WiFi] No networks found or scan failed");
        // Try a standard connection again as last resort
        WiFi.begin(ssid.c_str(), password.c_str());
        return false;
    }
    
    // Find the strongest AP with our SSID
    int strongestRSSI = -100;
    int strongestIndex = -1;
    
    for (int i = 0; i < numNetworks; i++) {
        if (WiFi.SSID(i) == ssid) {
            int rssi = WiFi.RSSI(i);
            String bssid = WiFi.BSSIDstr(i);
            int channel = WiFi.channel(i);
            
            dbgln("[WiFi] Found " + ssid + " with BSSID " + bssid + 
                  " on channel " + String(channel) + 
                  " with RSSI " + String(rssi) + "dBm");
            
            if (rssi > strongestRSSI) {
                strongestRSSI = rssi;
                strongestIndex = i;
            }
        }
    }
    
    if (strongestIndex == -1) {
        dbgln("[WiFi] No APs with SSID " + ssid + " found");
        // Try a standard connection
        WiFi.begin(ssid.c_str(), password.c_str());
        return false;
    }
    
    // Get the strongest AP's details
    String bssid = WiFi.BSSIDstr(strongestIndex);
    int channel = WiFi.channel(strongestIndex);
    
    dbgln("[WiFi] Connecting to strongest AP: " + bssid + 
          " on channel " + String(channel) + 
          " with RSSI " + String(strongestRSSI) + "dBm");
    
    // Convert string BSSID to uint8_t array
    uint8_t bssidBytes[6];
    sscanf(bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", 
           &bssidBytes[0], &bssidBytes[1], &bssidBytes[2], 
           &bssidBytes[3], &bssidBytes[4], &bssidBytes[5]);
    
    // Connect to the specific BSSID and channel
    WiFi.begin(ssid.c_str(), password.c_str(), channel, bssidBytes);
    
    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { // Reduced from 30 to 20
        delay(300); // Reduced from 500 to 300
        dbg(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        dbgln("\n[WiFi] Successfully connected to " + WiFi.SSID() + 
              " with BSSID " + WiFi.BSSIDstr() + 
              " on channel " + String(WiFi.channel()) + 
              " with RSSI " + String(WiFi.RSSI()) + "dBm");
        
        return true;
    } else {
        dbgln("\n[WiFi] Failed to connect to the strongest AP");
        // Try a standard connection as a last resort
        WiFi.begin(ssid.c_str(), password.c_str());
        return false;
    }
}

// Function to scan for the strongest AP with the same SSID and connect to it
bool connectToStrongestAP() {
    dbgln("[WiFi] Scanning for the strongest AP due to poor signal...");
    
    String currentSSID = WiFi.SSID();
    if (currentSSID.length() == 0) {
        currentSSID = wm.getWiFiSSID();
    }
    
    if (currentSSID.length() == 0) {
        dbgln("[WiFi] No SSID configured, cannot scan for strongest AP");
        return false;
    }
    
    // Check current signal strength first
    int currentRSSI = WiFi.RSSI();
    
    // Only proceed with scanning if signal is below threshold
    if (currentRSSI > WIFI_RSSI_THRESHOLD && WiFi.status() == WL_CONNECTED) {
        dbgln("[WiFi] Current signal strength (" + String(currentRSSI) + "dBm) is acceptable, skipping scan");
        return true;
    }
    
    // Use synchronous scan instead of async
    int numNetworks = WiFi.scanNetworks(false, true);  // async=false, show_hidden=true
    
    if (numNetworks <= 0) {
        dbgln("[WiFi] No networks found or scan failed");
        return false;
    }
    
    int strongestRSSI = -100;
    int strongestIndex = -1;
    
    for (int i = 0; i < numNetworks; i++) {
        if (WiFi.SSID(i) == currentSSID) {
            int rssi = WiFi.RSSI(i);
            String bssid = WiFi.BSSIDstr(i);
            int channel = WiFi.channel(i);
            
            dbgln("[WiFi] Found " + currentSSID + " with BSSID " + bssid + 
                  " on channel " + String(channel) + 
                  " with RSSI " + String(rssi) + "dBm");
            
            if (rssi > strongestRSSI) {
                strongestRSSI = rssi;
                strongestIndex = i;
            }
        }
    }
    
    if (strongestIndex == -1) {
        dbgln("[WiFi] No APs with SSID " + currentSSID + " found");
        return false;
    }
    
    // If we're already connected to the strongest AP, no need to reconnect
    if (WiFi.status() == WL_CONNECTED && WiFi.BSSIDstr() == WiFi.BSSIDstr(strongestIndex)) {
        dbgln("[WiFi] Already connected to the strongest AP");
        return true;
    }
    
    // Only reconnect if the strongest AP has significantly better signal (at least 10dBm better)
    if (WiFi.status() == WL_CONNECTED && (strongestRSSI - currentRSSI < 10)) {
        dbgln("[WiFi] Strongest AP signal (" + String(strongestRSSI) + "dBm) not significantly better than current (" + 
              String(currentRSSI) + "dBm), staying connected");
        return true;
    }
    
    // Connect to the strongest AP
    String password = wm.getWiFiPass();
    String bssid = WiFi.BSSIDstr(strongestIndex);
    int channel = WiFi.channel(strongestIndex);
    
    dbgln("[WiFi] Connecting to the strongest AP with BSSID " + bssid + 
          " on channel " + String(channel) + 
          " with RSSI " + String(strongestRSSI) + "dBm");
    
    // Convert string BSSID to uint8_t array
    uint8_t bssidBytes[6];
    sscanf(bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", 
           &bssidBytes[0], &bssidBytes[1], &bssidBytes[2], 
           &bssidBytes[3], &bssidBytes[4], &bssidBytes[5]);
    
    // Connect to the specific BSSID and channel
    WiFi.begin(currentSSID.c_str(), password.c_str(), channel, bssidBytes);
    
    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { // Reduced from 30 to 20
        delay(300); // Reduced from 500 to 300
        dbg(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        dbgln("\n[WiFi] Connected to " + WiFi.SSID() + 
              " with BSSID " + WiFi.BSSIDstr() + 
              " on channel " + String(WiFi.channel()) + 
              " with RSSI " + String(WiFi.RSSI()) + "dBm");
        
        return true;
    } else {
        dbgln("\n[WiFi] Failed to connect to the strongest AP, falling back to normal connection");
        WiFi.begin(currentSSID.c_str(), password.c_str());
        return false;
    }
}

// Function to verify WiFi is actually disconnected by checking multiple times
bool isWiFiActuallyDisconnected() {
    if (wifiDisconnectDetected) {
        // If we detected a disconnect event, trust it
        return true;
    }
    
    // First quick check
    if (WiFi.status() == WL_CONNECTED) {
        return false;
    }
    
    // Check multiple times to confirm disconnection, but with shorter delays
    int disconnectedCount = 0;
    for (int i = 0; i < WIFI_CONNECTION_VERIFY_COUNT; i++) {
        if (WiFi.status() != WL_CONNECTED) {
            disconnectedCount++;
        }
        delay(50); // Reduced from 100ms to 50ms
    }
    
    // Only consider disconnected if all checks failed
    return disconnectedCount == WIFI_CONNECTION_VERIFY_COUNT;
}

void setup() {
#ifdef REROUTE_DEBUG
    debugSerial.begin(57600, EspSoftwareSerial::SWSERIAL_8N1, SSERIAL_RX, SSERIAL_TX, false, 512, 512);
    debugSerial.enableIntTx(false);
#else
    debugSerial.begin(115200);
#endif
    dbgln("[config] load");
    prefs.begin("modbusRtuGw");
    config.begin(&prefs);
    pinMode(buttonPin, INPUT_PULLUP); // Use internal pull-up resistor

    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 32, "Initializing...");
    u8g2.sendBuffer();

    dbgln("[wifi] start");

    // Register WiFi event handler
    WiFi.onEvent(WiFiEventHandler);

    // Prevent BSSID fixation
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(true);
    
    String hostname = config.getHostname();
    WiFi.setHostname(hostname.c_str());
    
    // Set clean connect to prevent BSSID fixation
    wm.setCleanConnect(true);

    wm.setAPCallback(configModeCallback); // Set callback for AP mode
    wm.setClass("invert");
    wm.setShowStaticFields(true);
    wm.setShowDnsFields(true);
    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(180); 
    wm.setBreakAfterConfig(true); // Ensure we save config after portal use

    // First, try a complete WiFi reset and connect to the strongest AP
    bool connected = forceWiFiReset();
    
    // If that fails, fall back to WiFiManager
    if (!connected) {
        dbgln("[WiFi] Force reset failed, falling back to WiFiManager");
        
        // Attempt to connect or start the config portal
        if (!wm.autoConnect("ESP32_AP")) {
            dbgln("[WiFiManager] Failed to connect, starting captive portal...");
            wm.startConfigPortal("ESP32_AP");
            ESP.restart(); // Optionally restart after config portal closes
        }
        
        // After connecting with WiFiManager, try to find and connect to the strongest AP
        if (WiFi.status() == WL_CONNECTED) {
            dbgln("[WiFi] Connected via WiFiManager, now finding the strongest AP");
            connectToStrongestAP();
            
            // Initialize WiFi connection time if not already set by event handler
            if (lastWiFiConnectionTime == 0) {
                lastWiFiConnectionTime = millis();
                dbgln("[WiFi] Initialized connection time: " + String(lastWiFiConnectionTime));
            }
        }
    }

    dbgln("[wifi] finished");
    delay(2000);

    MBUlogLvl = LOG_LEVEL_WARNING;

    dbgln("[modbusCache] begin");

    serverIPStr = config.getTargetIP();
    serverPort = config.getTcpPort2();
    modbusCache = new ModbusCache(dynamicRegisters, staticRegisters, serverIPStr, serverPort);
    modbusCache->begin();

#ifdef SDM120
    if(!config.getClientIsRTU()) {
        dbgln("[modbusCache] call createEmulatedServer");
        modbusCache->createEmulatedServer(sdm120Registers);
    }
#endif
    // Find the register address for "Watts"
    for (const auto &reg : dynamicRegisters) {
        if (reg.description == "Watts") {
            wattsRegisterAddress = reg.address;
            break;
        }
    }

    dbgln("[modbusCache] finished");

    setupPages(&webServer, modbusCache, &config, &wm);
    webServer.begin();
    dbgln("[setup] finished");
}

void loop() {
    static unsigned long lastUpdateTime = 0;
    static unsigned long lastWiFiCheckTime = 0;
    static bool inAPMode = false;
    static unsigned long lastDisconnectionCheckTime = 0;
    static int reconnectionAttempts = 0;
    const int maxReconnectionAttempts = 3;
    static unsigned long reconnectionBackoff = 5000; // Start with 5 seconds
    unsigned long currentTime = millis();

    // Update the Modbus Cache
    if (modbusCache) {
        modbusCache->update();
    }

    // Check WiFi connection status and manage AP mode
    if (currentTime - lastDisconnectionCheckTime >= 10000) {
        lastDisconnectionCheckTime = currentTime;
        
        if (isWiFiActuallyDisconnected() && !inAPMode) {
            reconnectionAttempts++;
            dbgln("[WiFi] Not connected. Attempt " + String(reconnectionAttempts) + " of " + String(maxReconnectionAttempts));
            
            if (reconnectionAttempts <= maxReconnectionAttempts) {
                // Try a complete WiFi reset
                bool connected = forceWiFiReset();
                
                if (connected) {
                    reconnectionAttempts = 0;
                    reconnectionBackoff = 5000; // Reset on success
                } else {
                    // Increase backoff time for next attempt (cap at 2 minutes)
                    reconnectionBackoff = min(reconnectionBackoff * 2, 120000UL);
                }
            } else {
                // After max attempts, switch to AP mode
                dbgln("[WiFi] Max reconnection attempts reached, switching to AP mode");
                wm.startWebPortal();
                inAPMode = true;
                reconnectionAttempts = 0;
            }
        } else if (WiFi.status() == WL_CONNECTED) {
            // Reset counter if we're connected
            reconnectionAttempts = 0;
        }
    }

    // If the WiFi is connected and we were in AP mode, switch to STA mode
    if (WiFi.status() == WL_CONNECTED && inAPMode) {
        dbgln("[WiFi] Connected to WiFi. Switching to STA mode.");
        inAPMode = false;
        
        // Reset the WiFi check times
        lastWiFiCheckTime = currentTime;
        wifiDisconnectDetected = false;
    }

    // Get current time
    currentTime = millis();
    
    // Only check WiFi signal strength if we're connected
    if (WiFi.status() == WL_CONNECTED && currentTime - lastWiFiCheckTime >= WIFI_CHECK_INTERVAL) {
        lastWiFiCheckTime = currentTime;
        int rssi = WiFi.RSSI();
        dbgln("[WiFi] Current RSSI: " + String(rssi) + "dBm, BSSID: " + WiFi.BSSIDstr() + 
              ", Channel: " + String(WiFi.channel()));
        
        // Only take action if signal is critically weak
        if (rssi < WIFI_RSSI_THRESHOLD) {
            dbgln("[WiFi] Signal strength critically low, scanning for stronger AP");
            connectToStrongestAP();
        }
    }

    handleButton();
    // Update the OLED
    if (currentTime - lastUpdateTime >= 200) {
        lastUpdateTime = currentTime;
        updateDisplay();
    }

    wm.process(); // Non-blocking WiFiManager process
    yield();
}
