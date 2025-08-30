#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
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
#include <Arduino.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ModbusClientTCPasync.h>
#include "debug.h"
#include "wifi_utils.h"
#include "esp_task_wdt.h" // Include ESP task watchdog header

#ifdef REROUTE_DEBUG
EspSoftwareSerial::UART debugSerial;
#else
#define debugSerial Serial
#endif

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);
AsyncWebServer webServer(80);
Config config;
Preferences prefs;
DNSServer dnsServer;
AsyncWiFiManager wm(&webServer, &dnsServer);

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
// Button hold variables for WiFi reset
unsigned long buttonHoldStartTime = 0;
bool buttonHolding = false;
const unsigned long WIFI_RESET_HOLD_TIME = 9000; // 9 seconds to reset WiFi
const unsigned long SCREEN_SWITCH_DELAY = 1000; // 1 second delay before WiFi reset countdown
int countdownSeconds = 0;
// Multi-screen display variables
int currentScreen = 0; // 0 = screen1 (default), 1 = screen2
const int NUM_SCREENS = 2;

#define WIFI_RSSI_THRESHOLD -80  // RSSI threshold to trigger reconnection (dBm)
#define WIFI_CHECK_INTERVAL 300000 // Check WiFi signal strength every 5 minutes
#define WIFI_CONNECTION_VERIFY_COUNT 3 // Number of times to verify WiFi is actually disconnected

// Add WiFi event handler to track connection status
bool wifiConnected = false;
bool wifiDisconnectDetected = false;
unsigned long lastWiFiConnectionTime = 0; // Track when WiFi was last connected
bool inConfigPortal = false; // Track if we're in config portal mode
// Removed WiFi mutex - simplified event handling

void WiFiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            dbgln("[WiFi] Connected to AP");
            wifiConnected = true;
            wifiDisconnectDetected = false;
            lastWiFiConnectionTime = millis();
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            dbgln("[WiFi] Disconnected from AP");
            wifiDisconnectDetected = true;
            wifiConnected = false;
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            static char ipBuffer[32];
        snprintf(ipBuffer, sizeof(ipBuffer), "[WiFi] Got IP: %s", WiFi.localIP().toString().c_str());
        dbgln(ipBuffer);
            wifiConnected = true;
            if (lastWiFiConnectionTime == 0) {
                lastWiFiConnectionTime = millis();
            }
            break;
        default:
            break;
    }
}

// Callback for when we enter Access Point mode
void configModeCallback(AsyncWiFiManager *myWiFiManager) {
    dbgln("[WiFiManager] Entered config mode");
    dbgln("AP SSID: " + myWiFiManager->getConfigPortalSSID());
    inConfigPortal = true; // Set flag to indicate we're in config portal
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 14, myWiFiManager->getConfigPortalSSID().c_str());
    u8g2.setFont(u8g2_font_ncenB10_tr);  // Slightly smaller
    u8g2.drawStr(0, 32, "Setup Wifi");
    u8g2.sendBuffer();
}

void handleButton() {
  int reading = digitalRead(buttonPin);
  
  // Debounce logic
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      
      // Button just pressed (falling edge)
      if (buttonState == LOW) {
        if (!buttonHolding) {
          // Start hold timer
          buttonHoldStartTime = millis();
          buttonHolding = true;
        }
      }
      // Button just released (rising edge)
      else if (buttonState == HIGH) {
        if (buttonHolding) {
          unsigned long holdDuration = millis() - buttonHoldStartTime;
          buttonHolding = false;
          countdownSeconds = 0;
          
          // Check hold duration to determine action
          if (holdDuration < SCREEN_SWITCH_DELAY) {
            // Short press (< 1 second) - switch screens
            currentScreen = (currentScreen + 1) % NUM_SCREENS;
          }
          // If held longer than 1 second but less than 6, just cancel (no action)
          // WiFi reset happens in the hold timing section below
        }
      }
    }
  }
  
  // Handle button hold timing and countdown (only after 1 second delay)
  if (buttonHolding && buttonState == LOW) {
    unsigned long holdDuration = millis() - buttonHoldStartTime;
    
    // Only start countdown after 1-second delay
    if (holdDuration >= SCREEN_SWITCH_DELAY) {
      unsigned long countdownDuration = holdDuration - SCREEN_SWITCH_DELAY;
      unsigned long remainingTime = WIFI_RESET_HOLD_TIME - SCREEN_SWITCH_DELAY - countdownDuration;
      int newCountdown = (remainingTime + 999) / 1000; // Round up to nearest second
      
      // Update countdown display
      if (newCountdown != countdownSeconds && newCountdown >= 0) {
        countdownSeconds = newCountdown;
      }
      
      // Check if full hold time has been reached (6 seconds total)
      if (holdDuration >= WIFI_RESET_HOLD_TIME) {
        // Reset WiFi configuration
        dbgln("[Button] WiFi reset triggered by 6-second hold");
        wm.resetSettings();
        
        // Reset button state
        buttonHolding = false;
        countdownSeconds = 0;
        
        // Restart ESP32 to apply changes
        ESP.restart();
      }
    }
  }
  
  lastButtonState = reading;
}

void updateDisplay() {
    dbgln("Updating display...");
    
    u8g2.clearBuffer();
    
    // Check if we're in button hold countdown mode (only after 1 second hold)
    if (buttonHolding && countdownSeconds > 0) {
        // Show WiFi reset countdown in the lower area to avoid color bands
        u8g2.setFont(u8g2_font_ncenB14_tr);
        u8g2.drawStr(0, 40, "WiFi Reset");
        
        char countdownBuffer[32];
        snprintf(countdownBuffer, sizeof(countdownBuffer), "Hold: %d", countdownSeconds);
        u8g2.drawStr(0, 55, countdownBuffer);
        
        u8g2.setFont(u8g2_font_ncenB08_tr);
        u8g2.drawStr(0, 64, "Release to cancel");
    }
    else {
        // Normal display modes based on currentScreen
        if (currentScreen == 0) {
            // Screen 1 - Original display with power data
            if (wattsRegisterAddress != -1) {
                // Get the "Watts" value from the Modbus cache
                float wattsValue = modbusCache->getRegisterScaledValue(wattsRegisterAddress);
                static char ssidBuffer[64];
                static char ipBuffer[32];
                snprintf(ssidBuffer, sizeof(ssidBuffer), "SSID: %s", WiFi.SSID().c_str());
                snprintf(ipBuffer, sizeof(ipBuffer), "IP: %s", WiFi.localIP().toString().c_str());

                u8g2.setFont(u8g2_font_ncenB14_tr);

                // Display the wattage if data is operational, otherwise show "No Data"
                if (modbusCache->getIsOperational()) {
                    static char wattsBuffer[32];
                    snprintf(wattsBuffer, sizeof(wattsBuffer), "%.1f W", wattsValue);
                    u8g2.drawStr(0, 16, wattsBuffer);
                } else {
                    u8g2.drawStr(0, 16, "No data");
                }

                u8g2.setFont(u8g2_font_ncenB08_tr);
                u8g2.drawStr(0, 30, "Grid Power");

                // Display WiFi SSID and IP address
                u8g2.drawStr(0, 45, ssidBuffer);
                u8g2.drawStr(0, 60, ipBuffer);
            }
        }
        else if (currentScreen == 1) {
            // Screen 2 - Detailed electrical measurements
            u8g2.setFont(u8g2_font_ncenB08_tr); // Use small font to fit all data
            
            if (modbusCache && modbusCache->getIsOperational()) {
                // Get electrical values from Modbus cache
                float volts = modbusCache->getRegisterScaledValue(0);        // Volts register
                float amps = modbusCache->getRegisterScaledValue(2);         // Amps register  
                float watts = modbusCache->getRegisterScaledValue(wattsRegisterAddress); // Watts register
                float powerFactor = modbusCache->getRegisterScaledValue(14); // Power Factor register
                float energyKwh = modbusCache->getRegisterScaledValue(16);   // Energy kWh (+) register
                
                // Create display buffers
                static char line1[32], line2[32], line3[32], line4[32], line5[32];
                
                // Format each line with values
                snprintf(line1, sizeof(line1), "Volts: %.1fV", volts);
                snprintf(line2, sizeof(line2), "Amps: %.3fA", amps);
                snprintf(line3, sizeof(line3), "Watts: %.1fW", watts);
                snprintf(line4, sizeof(line4), "PF: %.3f", powerFactor);
                snprintf(line5, sizeof(line5), "Energy: %.1fkWh", energyKwh);
                
                // Draw each line, using every available line on the display
                u8g2.drawStr(0, 12, line1);  // Line 1
                u8g2.drawStr(0, 24, line2);  // Line 2  
                u8g2.drawStr(0, 36, line3);  // Line 3
                u8g2.drawStr(0, 48, line4);  // Line 4
                u8g2.drawStr(0, 60, line5);  // Line 5
            } else {
                // Display "No data" if not operational
                u8g2.drawStr(0, 32, "No Modbus Data");
            }
        }
    }
    
    u8g2.sendBuffer();
}

// Declare these functions as non-static so they can be accessed from other files
bool forceWiFiReset() {
    dbgln("[WiFi] Performing WiFi reset due to connection issues...");
    
    // Disconnect from current network but don't clear credentials
    WiFi.disconnect();  // Changed from disconnect(true) to preserve credentials
    delay(1000);  // Give it time to disconnect
    
    // Only restore WiFi settings if we've had persistent issues
    static int resetCount = 0;
    if (++resetCount >= 3) {
        dbgln("[WiFi] Multiple reset attempts, performing full WiFi restore");
        esp_wifi_restore();
        resetCount = 0;
        delay(1000);
    }
    
    // Set WiFi mode to STA
    WiFi.mode(WIFI_STA);
    delay(500);
    
    // Get SSID and password from WiFiManager
    String ssid = wm.getConfiguredSTASSID();
    String password = wm.getConfiguredSTAPassword();
    
    if (ssid.length() == 0 || password.length() == 0) {
        dbgln("[WiFi] No SSID or password available, cannot reset WiFi");
        return false;
    }
    
    // Simple connection attempt without scanning
    dbgln("[WiFi] Attempting connection to " + ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        dbg(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        dbgln("\n[WiFi] Successfully connected to " + WiFi.SSID() + 
              " with RSSI " + String(WiFi.RSSI()) + "dBm");
        resetCount = 0;  // Reset counter on successful connection
        return true;
    } else {
        dbgln("\n[WiFi] Failed to connect");
        return false;
    }
}

bool connectToStrongestAP() {
    dbgln("[WiFi] Scanning for strongest AP...");
    
    // Get SSID and password from WiFiManager
    String targetSSID = wm.getConfiguredSTASSID();
    String password = wm.getConfiguredSTAPassword();
    
    if (targetSSID.length() == 0 || password.length() == 0) {
        dbgln("[WiFi] No SSID or password available for scanning");
        return false;
    }
    
    // Disconnect first
    WiFi.disconnect();
    delay(500);
    
    // Scan for networks
    int networkCount = WiFi.scanNetworks();
    if (networkCount == 0) {
        dbgln("[WiFi] No networks found during scan");
        return false;
    }
    
    // Find all matching SSIDs and select the strongest one
    int bestRSSI = -999;
    int bestNetworkIndex = -1;
    
    dbgln("[WiFi] Found " + String(networkCount) + " networks:");
    for (int i = 0; i < networkCount; i++) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        dbgln("  " + String(i) + ": " + ssid + " (" + String(rssi) + "dBm)");
        
        // Check if this network matches our target SSID and has better signal
        if (ssid.equals(targetSSID) && rssi > bestRSSI) {
            bestRSSI = rssi;
            bestNetworkIndex = i;
        }
    }
    
    if (bestNetworkIndex == -1) {
        dbgln("[WiFi] Target SSID '" + targetSSID + "' not found in scan results");
        WiFi.scanDelete(); // Clean up scan results
        return false;
    }
    
    // Connect to the strongest AP
    dbgln("[WiFi] Connecting to strongest AP: " + targetSSID + " (RSSI: " + String(bestRSSI) + "dBm)");
    
    // Use the specific BSSID to avoid fixation on weaker APs
    uint8_t* bssid = WiFi.BSSID(bestNetworkIndex);
    WiFi.begin(targetSSID.c_str(), password.c_str(), 0, bssid);
    
    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        dbg(".");
        attempts++;
    }
    
    WiFi.scanDelete(); // Clean up scan results
    
    if (WiFi.status() == WL_CONNECTED) {
        dbgln("\n[WiFi] Successfully connected to strongest AP: " + WiFi.SSID() + 
              " with RSSI " + String(WiFi.RSSI()) + "dBm");
        return true;
    } else {
        dbgln("\n[WiFi] Failed to connect to strongest AP");
        return false;
    }
}

bool isWiFiActuallyDisconnected() {
    // Trust the event handler's disconnect detection
    if (wifiDisconnectDetected) {
        return true;
    }
    
    // Simple status check - no multiple verification needed
    return WiFi.status() != WL_CONNECTED;
}

void setup() {
#ifdef REROUTE_DEBUG
    debugSerial.begin(57600, EspSoftwareSerial::SWSERIAL_8N1, SSERIAL_RX, SSERIAL_TX, false, 512, 512);
    debugSerial.enableIntTx(false);
#else
    debugSerial.begin(115200);
#endif

    // Configure ESP Task Watchdog
    dbgln("[setup] Configuring task watchdog");
    // Increase timeout to 20 seconds and disable panic
    esp_task_wdt_init(20, false); // 20 second timeout, don't panic on timeout
    esp_task_wdt_delete(NULL); // Remove current task (setup/loop) from watchdog monitoring
    
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

    // Configure hostname
    String hostname = config.getHostname();
    WiFi.setHostname(hostname.c_str());
    
    // Let WiFiManager handle all WiFi configuration
    // Don't interfere with WiFi settings here
    
    // Get MAC address for unique AP name
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[20];
    snprintf(apName, sizeof(apName), "ESP32_%02X%02X", mac[4], mac[5]);
    dbgln("[WiFiManager] AP name will be: " + String(apName));
    
    // Configure WiFiManager
    wm.setAPCallback(configModeCallback);
    // Note: ESPAsyncWiFiManager doesn't support setClass, setShowStaticFields, setShowDnsFields
    wm.setConnectTimeout(20);
    wm.setMinimumSignalQuality(20); // Set minimum signal quality
    
    // Configure save callback
    wm.setSaveConfigCallback([]() {
        dbgln("[WiFiManager] Configuration saved, will restart");
        inConfigPortal = false;
    });
    
    // Use autoConnect - it handles everything:
    // 1. Tries to connect with saved credentials
    // 2. If no credentials or connection fails, automatically starts config portal
    // 3. The portal runs at 192.168.4.1
    
    dbgln("[WiFiManager] Starting WiFi configuration...");
    
    // Set config portal timeout (3 minutes)
    wm.setConfigPortalTimeout(180);
    
    // This single call handles everything
    bool connected = wm.autoConnect(apName);
    
    if (!connected) {
        // Portal timed out without connection
        dbgln("[WiFiManager] Failed to connect and portal timed out");
        dbgln("[WiFiManager] Starting unlimited config portal...");
        
        inConfigPortal = true;
        
        // Disable watchdog for unlimited portal
        esp_task_wdt_delete(NULL);
        
        // Set no timeout and restart portal
        wm.setConfigPortalTimeout(0);
        
        // Start portal again with no timeout
        if (wm.startConfigPortal(apName)) {
            dbgln("[WiFiManager] Configuration saved, restarting...");
        } else {
            dbgln("[WiFiManager] Portal exited without saving");
        }
        
        ESP.restart();
    }
    
    // If we get here, we connected successfully
    inConfigPortal = false;
    dbgln("[WiFiManager] Successfully connected to WiFi");
    
    // Configure WiFi for stable operation now that we're connected
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.setSleep(false);
    
    // Set WiFi task to higher priority than UART - disable power saving for consistent performance
    esp_wifi_set_ps(WIFI_PS_NONE);
    dbgln("[WiFi] Power saving disabled for stability");
    
    // Lower main task priority to give WiFi higher priority
#ifdef CONFIG_FREERTOS_UNICORE
    vTaskPrioritySet(NULL, 1); // Lower main task priority on single core
    dbgln("[WiFi] Lowered main task priority for single core");
#else
    // On dual core, WiFi typically runs on core 0, app on core 1 by default
    vTaskPrioritySet(NULL, 1); // Lower main task priority
    dbgln("[WiFi] Lowered main task priority for WiFi stability");
#endif
    
    // Initialize WiFi connection time if connected
    if (WiFi.status() == WL_CONNECTED && lastWiFiConnectionTime == 0) {
        lastWiFiConnectionTime = millis();
        static char connBuffer[128];
        snprintf(connBuffer, sizeof(connBuffer), "[WiFi] Connected to: %s (RSSI: %ddBm)", WiFi.SSID().c_str(), WiFi.RSSI());
        dbgln(connBuffer);
    }

    dbgln("[wifi] finished");

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

    // Setup web server pages - AsyncWiFiManager shares the same server
    setupPages(&webServer, modbusCache, &config, &wm);
    
    // Start the web server
    webServer.begin();
    dbgln("[webServer] Started web server");
    
    dbgln("[setup] finished");
}

void loop() {
    // If we're in config portal mode, just handle WiFiManager and return
    if (inConfigPortal) {
        wm.loop();
        delay(10);
        return;
    }
    
    static unsigned long lastUpdateTime = 0;
    static unsigned long lastHeapCheck = 0;
    unsigned long currentTime = millis();
    
    // Check heap memory every 30 seconds
    if (currentTime - lastHeapCheck >= 30000) {
        lastHeapCheck = currentTime;
        static char heapBuffer[64];
        snprintf(heapBuffer, sizeof(heapBuffer), "[main] Free heap: %u bytes", ESP.getFreeHeap());
        dbgln(heapBuffer);
    }
    
    // Active WiFi monitoring and recovery
    static unsigned long lastWiFiStatusLog = 0;
    static unsigned long lastWiFiReconnectAttempt = 0;
    static int wifiReconnectAttempts = 0;
    
    // Check WiFi status more frequently
    if (currentTime - lastWiFiStatusLog >= 30000) { // Check every 30 seconds
        lastWiFiStatusLog = currentTime;
        if (WiFi.status() == WL_CONNECTED) {
            static char wifiBuffer[128];
            snprintf(wifiBuffer, sizeof(wifiBuffer), "[WiFi] Connected to: %s (RSSI: %ddBm)", WiFi.SSID().c_str(), WiFi.RSSI());
            dbgln(wifiBuffer);
            wifiReconnectAttempts = 0; // Reset counter on successful connection
        } else {
            dbgln("[WiFi] Disconnected - initiating recovery");
        }
        yield(); // Give WiFi stack CPU time after heavy string operations and WiFi calls
    }
    
    // Active WiFi recovery when disconnected
    if (WiFi.status() != WL_CONNECTED) {
        if (currentTime - lastWiFiReconnectAttempt >= 5000) { // Try every 5 seconds
            lastWiFiReconnectAttempt = currentTime;
            wifiReconnectAttempts++;
            
            dbgln("[WiFi] Connection lost, attempting recovery (attempt " + String(wifiReconnectAttempts) + ")");
            
            if (wifiReconnectAttempts <= 2) {
                // First attempts: Simple reconnect
                WiFi.reconnect();
                yield();
            } else if (wifiReconnectAttempts <= 4) {
                // Next attempts: Force WiFi reset
                if (forceWiFiReset()) {
                    wifiReconnectAttempts = 0;
                }
                yield();
            } else if (wifiReconnectAttempts <= 6) {
                // Try connecting to strongest AP
                if (connectToStrongestAP()) {
                    wifiReconnectAttempts = 0;
                }
                yield();
            } else {
                // Final resort - restart device
                logErrln("[WiFi] Multiple reconnection failures, restarting device");
                delay(100);
                ESP.restart();
            }
        }
    }
    
    // Check if we need to reboot due to no data for 60 seconds
    if (modbusCache) {
        // Get current timestamp
        currentTime = millis();
        
        // Get last update time safely
        unsigned long lastUpdate = modbusCache->getLastSuccessfulUpdate();
        
        // Track connection problems across multiple loop iterations
        static unsigned long noUpdatesSince = 0;
        static unsigned long lastStatusCheck = 0;
        static int connectionProblemCounter = 0;
        
        // Handle uint32_t overflow/underflow conditions safely
        unsigned long timeSinceLastUpdate;
        if (currentTime >= lastUpdate) {
            timeSinceLastUpdate = currentTime - lastUpdate;
        } else {
            // This case handles millis() overflow or lastUpdate being incorrectly set to a future time
            static char timeBuffer[128];
            snprintf(timeBuffer, sizeof(timeBuffer), "[main] Time calculation error: current=%lu, lastUpdate=%lu", currentTime, lastUpdate);
            logErrln(timeBuffer);
            // Use a more reasonable value instead of overflowing
            timeSinceLastUpdate = 0;
        }
        
        // Debug log status every 10 seconds
        if (currentTime - lastStatusCheck > 10000) {
            lastStatusCheck = currentTime;
            // This call is thread-safe as it uses proper getters
            bool isOp = modbusCache->getIsOperational();
            static char statusBuffer[256];
            snprintf(statusBuffer, sizeof(statusBuffer), "[main] Server operational: %s, Time since last update: %lu seconds, current: %lu, lastUpdate: %lu, connection problem counter: %d", 
                    isOp ? "YES" : "NO", timeSinceLastUpdate / 1000, currentTime, lastUpdate, connectionProblemCounter);
            dbgln(statusBuffer);
            yield(); // Give WiFi stack CPU time after heavy logging operations
            
            // If the server is non-operational, increment our problem counter
            if (!isOp) {
                connectionProblemCounter++;
                
                // If this is the first detection of a problem, note the time
                if (connectionProblemCounter == 1) {
                    noUpdatesSince = currentTime;
                }
                
                // Check if we've had sustained problems
                unsigned long sustainedProblemTime = 0;
                if (currentTime >= noUpdatesSince) {
                    sustainedProblemTime = currentTime - noUpdatesSince;
                }
                
                // Log the sustained problem duration
                if (connectionProblemCounter > 1) {
                    static char problemBuffer[128];
                    snprintf(problemBuffer, sizeof(problemBuffer), "[main] Non-operational state for %lu seconds, counter: %d", 
                            sustainedProblemTime / 1000, connectionProblemCounter);
                    logErrln(problemBuffer);
                    yield(); // Give WiFi stack CPU time after error logging
                }
                
                // Force reboot after 6 consecutive non-operational checks (approximately 60 seconds)
                // or if we've been non-operational for more than 60 seconds absolute time
                if (connectionProblemCounter >= 6 || sustainedProblemTime > 60000) {
                    logErrln("[main] Persistent connection problems detected. Rebooting device...");
                    delay(100);
                    ESP.restart();
                }
            } else {
                // Reset the counter if the server becomes operational again
                connectionProblemCounter = 0;
                noUpdatesSince = 0;
            }
            yield(); // Give WiFi stack CPU time after complex status checking block
        }
        
        // Original reboot logic - keep as a failsafe
        // If it's been more than 60 seconds with no data, reboot the device
        if (timeSinceLastUpdate > 60000 && timeSinceLastUpdate < 3600000) { // Between 1 minute and 1 hour
            static char rebootBuffer[128];
            snprintf(rebootBuffer, sizeof(rebootBuffer), "[main] No data received for %lu seconds. Rebooting device...", timeSinceLastUpdate / 1000);
            logErrln(rebootBuffer);
            delay(200); // Short delay to allow log message to be sent
            ESP.restart();
        }
    }

    // Update the Modbus Cache with WiFi-friendly timing
    if (modbusCache) {
        // Only update if WiFi is stable or we're not in active recovery
        if (WiFi.status() == WL_CONNECTED || wifiReconnectAttempts == 0) {
            modbusCache->update();
            yield(); // Ensure WiFi gets CPU time after Modbus operations
        } else {
            // During WiFi recovery, skip Modbus updates to prioritize reconnection
            yield();
            delay(10); // Give extra time to WiFi stack during recovery
        }
    }
    
    // Handle button input
    handleButton();
    
    // Update the OLED
    if (currentTime - lastUpdateTime >= 200) {
        lastUpdateTime = currentTime;
        updateDisplay();
        yield(); // Give WiFi stack CPU time after I2C OLED operations
    }

    // Only process WiFiManager when WiFi is stable to avoid conflicts
    if (WiFi.status() == WL_CONNECTED && !inConfigPortal) {
        wm.loop(); // Non-blocking WiFiManager process
    }
    yield(); // Give WiFi stack CPU time
    
    // Add small delay to ensure WiFi stack gets enough time
    delay(1); // 1ms delay ensures WiFi stack runs
    yield(); // Final yield before loop restart
}
