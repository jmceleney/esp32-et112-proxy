#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Logging.h>
#include <ModbusBridgeWiFi.h>
#include <ModbusClientRTU.h>
#include <ModbusServerRTU.h>
#include "ModbusClientTCP.h"
#include "config.h"
#include "pages.h"
#include <HardwareSerial.h>

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
ModbusClientRTU *MBclient;
ModbusBridgeWiFi MBbridge;
WiFiManager wm;
HardwareSerial modbusSerial1(1); 
WiFiClient wifiClient;
ModbusMessage globalResponse;
volatile bool responseReceived = false;
uint32_t globalToken = 0;

ModbusServerRTU modbusRTUServer(2000);
ModbusClientTCP modbusTCPClient(wifiClient);

ModbusMessage forwardRequest(ModbusMessage request) {
  // Forward request to Modbus TCP server
  modbusTCPClient.addRequest(request, globalToken++);

  // Wait for response
  unsigned long startTime = millis();
  while (!responseReceived && millis() - startTime < TIMEOUT) {
    // Run background tasks or yield to avoid WDT reset
    yield(); 
  }

  responseReceived = false; // Reset the flag
  return globalResponse; // Return the response
}
void handleData(ModbusMessage response, uint32_t token) {
  globalResponse = response;
  responseReceived = true;
}

void handleError(Error err, uint32_t token) {
  Serial.printf("Error: %s\n", (const char*)ModbusError(err));
}
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
  MBclient->begin(modbusSerial, 1);
  for (uint8_t i = 1; i < 248; i++)
  {
    MBbridge.attachServer(i, i, ANY_FUNCTION_CODE, MBclient);
  }

  MBbridge.start(config.getTcpPort(), 10, config.getTcpTimeout());

  // Now set up a server (slave)
  RTUutils::prepareHardwareSerial(modbusSerial1);
  modbusSerial1.begin(config.getModbusBaudRate(), config.getModbusConfig(), 25, 26);
  modbusRTUServer.begin(modbusSerial1, 1);
  modbusRTUServer.registerWorker(1,ANY_FUNCTION_CODE, &forwardRequest);

  // Now setup our TCP client to speak on loopback
  modbusTCPClient.begin();
  modbusTCPClient.setTarget(IPAddress(127, 0, 0, 1), config.getTcpPort());
  // Register data and error handlers
  modbusTCPClient.onDataHandler(&handleData);
  modbusTCPClient.onErrorHandler(&handleError);

  dbgln("[modbus] finished");
  setupPages(&webServer, MBclient, &MBbridge, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");
}

void loop() {

}