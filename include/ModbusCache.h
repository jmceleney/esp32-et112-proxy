#ifndef MODBUSCACHE_H
#define MODBUSCACHE_H

#include <Arduino.h>
#include <ModbusServerRTU.h>
#include "config.h"
#include <WiFi.h>
#include <ModbusClientTCP.h>
#include <map>
#include <IPAddress.h>


#define MAX_REGISTERS 400

class ModbusCache {
public:
    ModbusCache(const uint16_t* addressList, size_t addressCount, IPAddress serverIP, uint16_t serverPort);
    void begin();
    void update();
    uint16_t getRegisterValue(uint16_t address);
    void setRegisterValue(uint16_t address, uint16_t value);
    static ModbusMessage respondFromCache(ModbusMessage request);
    // Getter methods
    ModbusServerRTU& getModbusRTUServer();
    ModbusClientTCP& getModbusTCPClient();

private:
    const uint16_t* addressList;
    size_t addressCount;
    IPAddress serverIP; // IP address of the Modbus TCP server
    uint16_t serverPort; // Port number of the Modbus TCP server
    ModbusServerRTU modbusRTUServer;
    ModbusClientTCP modbusTCPClient;
    uint16_t registerValues[MAX_REGISTERS];
    void fetchFromRemote();
    void sendModbusRequest(uint16_t startAddress, uint16_t regCount);
    static ModbusCache* instance;
    WiFiClient wifiClient;
    void ensureTCPConnection();
    static void handleData(ModbusMessage response, uint32_t token);
    static void handleError(Error err, uint32_t token);// Static instance pointer
    std::map<uint32_t, std::pair<uint16_t, uint16_t>> requestMap; // Map to store token -> (startAddress, regCount)
};

#endif // MODBUSCACHE_H
