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
    ModbusCache(const uint16_t* addressList, size_t addressCount, const uint16_t* addressListStatic, size_t addressStaticCount, String& serverIPStr, uint16_t serverPort);
    void begin();
    void update();
    uint16_t getRegisterValue(uint16_t address);
    uint16_t update_interval = 500;
    void setRegisterValue(uint16_t address, uint16_t value);
    static ModbusMessage respondFromCache(ModbusMessage request);
    // Getter methods
    ModbusServerRTU& getModbusRTUServer();
    ModbusClientTCP& getModbusTCPClient();
    float getVoltage();
    float getAmps();
    float getWatts();
    float getPowerFactor();
    float getFrequency();
    bool getIsOperational() const {
        return isOperational;
    }

private:
    const uint16_t* addressList;
    size_t addressCount;
    const uint16_t* addressListStatic;
    size_t addressStaticCount;
    String serverIPString;
    IPAddress currentIPAddress;
    unsigned long lastPollStart = 0;  // Time of the last poll start
    IPAddress serverIP; // IP address of the Modbus TCP server
    uint16_t serverPort; // Port number of the Modbus TCP server
    ModbusServerRTU modbusRTUServer;
    ModbusClientTCP modbusTCPClient;
    uint16_t registerValues[MAX_REGISTERS];
    std::vector<uint16_t> unifiedRegisterList; // Unified list of registers
    std::map<uint16_t, size_t> registerIndexMap; // Map for quick lookup of register index
    void buildUnifiedRegisterList();
    void fetchFromRemote(const uint16_t* regList, size_t regListSize);
    void sendModbusRequest(uint16_t startAddress, uint16_t regCount);
    static ModbusCache* instance;
    WiFiClient wifiClient;
    void ensureTCPConnection();
    static void handleData(ModbusMessage response, uint32_t token);
    static void handleError(Error err, uint32_t token);// Static instance pointer
    void purgeToken(uint32_t token);
    //std::map<uint32_t, std::pair<uint16_t, uint16_t>> requestMap; // Map to store token -> (startAddress, regCount)
    std::map<uint32_t, std::tuple<uint16_t, uint16_t, unsigned long>> requestMap; // Map to store token -> (startAddress, regCount, timestamp)
    std::queue<uint32_t> insertionOrder; // Queue to store the order in which requests were made
    unsigned long lastSuccessfulUpdate;
    bool isOperational;
    void updateServerStatusBasedOnCommFailure();
    // Utility method to read a 32-bit value from two 16-bit registers
    uint32_t read32BitValue(uint16_t address);
    int32_t read32BitSignedValue(uint16_t address);

    // Hard-coded register addresses
    static const uint16_t VOLTAGE_ADDR = 0;
    static const uint16_t AMPS_ADDR = 2;
    static const uint16_t WATTS_ADDR = 4;
    static const uint16_t PF_ADDR = 14;
    static const uint16_t FREQ_ADDR = 15;
    std::map<uint16_t, bool> staticRegisterFetchStatus; // Map to track static register fetch status
    bool staticRegistersFetched = false; // Flag to indicate completion of static register fetching
    SemaphoreHandle_t mutex;
};

#endif // MODBUSCACHE_H
