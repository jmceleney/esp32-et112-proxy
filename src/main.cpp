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
#include <esp_err.h>

extern "C" void app_error_handler(esp_err_t error)
{
    // Print the exception details to the serial interface
    Serial.print("Exception: ");
    Serial.println(esp_err_to_name(error));
    Serial.flush();
    delay(2000);  // Delay to allow the serial output to be sent
    esp_restart(); // Restart the ESP32
}

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
ModbusClientRTU *MBclient;
ModbusBridgeWiFi MBbridge;
WiFiManager wm;

const uint16_t modbusAddressList[] = {
    0, 1, 2, 3, 4, 5,  // Volts*10, Amps*100, Watts*10
    11,                // [300012] Carlo Gavazzi Controls identification code
    14,15,             // PF (INT16), Freq (INT16)
    16, 17, 18, 19,    // kWh (+) TOT * 10, Kvarh (+) TOT * 10
    32,33,34,35,       // kWh (-) TOT * 10, Kvarh (-) TOT * 10
};

// 
const uint16_t modbusAddressListStatic[] = {
    
    770,771, // Version, Revision
    4355,    // Measurement mode
    // Range 20480-20486 - Serial number
    20480, 20481, 20482, 20483, 20484, 20485, 20486
};

String serverIPStr;
uint16_t serverPort;

const size_t addressCount = sizeof(modbusAddressList) / sizeof(modbusAddressList[0]);
const size_t addressStaticCount = sizeof(modbusAddressListStatic) / sizeof(modbusAddressListStatic[0]);
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
  dbgln("[modbus bridge setup] start");

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

  MBclient = new ModbusClientRTU(config.getModbusRtsPin(), 10); // queuelimit 10
  MBclient->setTimeout(1000);
  MBclient->begin(modbusSerial, 1);

  for (uint8_t i = 1; i < 248; i++)
  {
    MBbridge.attachServer(i, i, ANY_FUNCTION_CODE, MBclient);
  }

  MBbridge.start(config.getTcpPort(), 20, config.getTcpTimeout());
  dbgln("[modbus bridge setup] finished");
  dbgln("[modbusCache] begin");

  serverIPStr = config.getTargetIP();
  serverPort = config.getTcpPort2();
  modbusCache = new ModbusCache(modbusAddressList, addressCount, modbusAddressListStatic, addressStaticCount, serverIPStr, serverPort);// Initialize ModbusCache if the configuration is valid
  
  modbusCache->begin();    
  
  dbgln("[modbusCache] finished");

  setupPages(&webServer, MBclient, modbusCache, &MBbridge, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");
}

void loop() {
    if (modbusCache) {
        modbusCache->update();
    }
    yield();
}
