#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Logging.h>

//#include <ModbusBridgeWiFi.h>

#include "ModbusCache.h"
#include "config.h"
#include "pages.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <cmath> // Include cmath for acos

#ifdef REROUTE_DEBUG
    //SoftwareSerial debugSerial(SSERIAL_RX, SSERIAL_TX);
    EspSoftwareSerial::UART debugSerial;
    //#define Serial debugSerial
# else
    
    #define debugSerial Serial
    
#endif


/*
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
*/

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
WiFiManager wm;

void setup_uart() {
    //uart_set_pin(UART_NUM_0, 18, 19, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    //uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
        uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    // Configure UART parameters
    uart_param_config(UART_NUM_0, &uart_config);
    uart_set_pin(UART_NUM_0, 18, 19, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
}

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
    {8193,RegisterType::UINT16, "RS485 baud rate"},
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

String serverIPStr;
uint16_t serverPort;

ModbusCache* modbusCache = nullptr;

void setup() {
#ifdef REROUTE_DEBUG
  setup_uart();
  //esp_log_level_set("*", ESP_LOG_NONE);
  debugSerial.begin(38400, EspSoftwareSerial::SWSERIAL_8N1, SSERIAL_RX, SSERIAL_TX, false, 256, 256);
#else
  debugSerial.begin(115200);
#endif
  dbgln();
  dbgln("[config] load")
  prefs.begin("modbusRtuGw");
  config.begin(&prefs);
  //debugSerial.end();
  //debugSerial.begin(config.getSerialBaudRate());
  dbgln("[wifi] start");
  WiFi.mode(WIFI_STA);
  wm.setClass("invert");
  auto reboot = false;
  wm.setAPCallback([&reboot](WiFiManager *wifiManager){reboot = true;});
  wm.setConnectTimeout(20);
  wm.setConnectRetries(100);
  if (wm.getWiFiIsSaved()) wm.setConfigPortalTimeout(180);

  wm.autoConnect();
  if (reboot){
    ESP.restart();
  }
  dbgln("[wifi] finished");

  MBUlogLvl = LOG_LEVEL_WARNING;

  dbgln("[modbusCache] begin");

  serverIPStr = config.getTargetIP();
  serverPort = config.getTcpPort2();
  modbusCache = new ModbusCache(dynamicRegisters, staticRegisters, serverIPStr, serverPort);
    //modbusAddressList, addressCount, modbusAddressListStatic, addressStaticCount, serverIPStr, serverPort);// Initialize ModbusCache if the configuration is valid
  
  modbusCache->begin();
  
  //if(!config.getClientIsRTU()) {
    dbgln("[modbusCache] call createEmulatedServer");
    modbusCache->createEmulatedServer(sdm120Registers);
  //}
  
  dbgln("[modbusCache] finished");

  setupPages(&webServer, modbusCache, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");
}

void loop() {
    if (modbusCache) {
        modbusCache->update();
    }
    yield();
}
