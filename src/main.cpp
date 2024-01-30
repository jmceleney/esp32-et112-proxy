#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Logging.h>
#include <ModbusBridgeWiFi.h>
#include <ModbusClientRTU.h>
#include "ModbusCache.h"
#include "config.h"
#include "pages.h"
//#include <HardwareSerial.h>
//#include <WiFiServer.h>

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
ModbusClientRTU *MBclient;
ModbusBridgeWiFi MBbridge;
WiFiManager wm;

const uint16_t modbusAddressList[] = {
    // Range 0-35
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 
    30, 31, 32, 33, 34, 35,
    771,
    4355,
    // Range 20480-20486
    20480, 20481, 20482, 20483, 20484, 20485, 20486
};
IPAddress serverIP;
uint16_t serverPort;

const size_t addressCount = sizeof(modbusAddressList) / sizeof(modbusAddressList[0]);
ModbusCache* modbusCache = nullptr;

void setup() {
  debugSerial.begin(115200);
  dbgln();
  dbgln("[config] load")
  prefs.begin("modbusRtuGw");
  config.begin(&prefs);
  debugSerial.end();
  debugSerial.begin(config.getSerialBaudRate(), config.getSerialConfig());
  dbgln("[wifi] start");
  WiFi.mode(WIFI_STA);
  wm.setClass("invert");
  auto reboot = false;
  wm.setAPCallback([&reboot](WiFiManager *wifiManager){reboot = true;});
  wm.autoConnect();
  if (reboot){
    ESP.restart();
  }
  dbgln("[wifi] finished");

  dbgln("[modbus] start");

  MBUlogLvl = LOG_LEVEL_WARNING;
  RTUutils::prepareHardwareSerial(modbusSerial);
#if defined(RX_PIN) && defined(TX_PIN)
  // use rx and tx-pins if defined in platformio.ini
  modbusSerial.begin(config.getModbusBaudRate(), config.getModbusConfig(), RX_PIN, TX_PIN );
  dbgln("Use user defined RX/TX pins");
#else
  // otherwise use default pins for hardware-serial2
  modbusSerial.begin(config.getModbusBaudRate(), config.getModbusConfig());
#endif

  MBclient = new ModbusClientRTU(config.getModbusRtsPin());
  MBclient->setTimeout(1000);
  MBclient->begin(modbusSerial, -1);

  for (uint8_t i = 1; i < 248; i++)
  {
    MBbridge.attachServer(i, i, ANY_FUNCTION_CODE, MBclient);
  }

  MBbridge.start(config.getTcpPort(), 10, config.getTcpTimeout());

  dbgln("[modbus] finished");
  dbgln("[modbusCache] begin");
  bool validConfig = true;

    // Check and parse the server IP
  if (!serverIP.fromString(config.getTargetIP())) {
      Serial.println("Error: Invalid server IP address.");
      validConfig = false;
  }

  // Get the server port
  serverPort = config.getTcpPort2();
  if (serverPort == 0) {
      Serial.println("Error: Invalid server port.");
      validConfig = false;
  }

  modbusCache = new ModbusCache(modbusAddressList, addressCount, validConfig ? serverIP : IPAddress(127, 0, 0, 1), validConfig ? serverPort : 502);// Initialize ModbusCache if the configuration is valid
  if (validConfig) {
    // Log IP address and port
    dbgln("Server IP: " + serverIP.toString());
    dbgln("Server port: " + String(serverPort));
    modbusCache->begin();    
  }
  
  dbgln("[modbusCache] finished");

  ModbusServerRTU& modbusRTUServer = modbusCache->getModbusRTUServer();
  ModbusClientTCP& modbusTCPClient = modbusCache->getModbusTCPClient();

  setupPages(&webServer, MBclient, &modbusTCPClient, &modbusRTUServer, &MBbridge, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");
}

void loop() {
  static unsigned long lastUpdateTime = 0; // Stores the last time `update` was called
    const unsigned long updateInterval = 400; // Update interval in milliseconds

    unsigned long currentMillis = millis();

    // Check if `updateInterval` time has passed since last update
    if (currentMillis - lastUpdateTime >= updateInterval) {
        // Save the last update time
        lastUpdateTime = currentMillis;

        if (modbusCache) {
            modbusCache->update();
        }
    }
}
