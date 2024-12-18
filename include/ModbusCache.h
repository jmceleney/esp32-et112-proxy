#ifndef MODBUSCACHE_H
#define MODBUSCACHE_H

#include <Arduino.h>
#include <ModbusServerRTU.h>
#include "ModbusServerTCPasync.h"
#include "config.h"
#include <WiFi.h>
#include <ModbusTCPWrapper.h>
#include <ModbusRTUWrapper.h>
#include <map>
#include <set>
#include <queue>
#include <optional>
#include <IPAddress.h>
#include <unordered_set>
#include <SoftwareSerial.h>
#include <deque> // Required for std::deque
#include <algorithm> // For std::min and std::max
#include <cmath> // For sqrtf

#define MAX_REGISTERS 400

enum class RegisterType {
    UINT16,
    INT16,
    UINT32,
    INT32,
    FLOAT
};

static String typeString(RegisterType type) {
    switch (type) {
        case RegisterType::UINT16: return "UINT16";
        case RegisterType::INT16: return "INT16";
        case RegisterType::UINT32: return "UINT32";
        case RegisterType::INT32: return "INT32";
        case RegisterType::FLOAT: return "FLOAT";
        default: return "Unknown RegisterType";
    }
};

struct Uint16Pair {
    uint16_t highWord;
    uint16_t lowWord;
};

enum class UnitType {
    V,
    A,
    W,
    PF,
    Hz,
    KWh,
    KVarh,
    VA,
    var
    // Add more units as needed
};

class ModbusCache; // Forward declaration

struct ModbusRegister {
    uint16_t address;
    RegisterType type;
    String description;
    std::optional<float> scalingFactor;
    std::optional<UnitType> unit;
    std::optional<uint16_t> backendAddress;
    std::optional<std::function<double(ModbusCache*, double)>> transformFunction;

    ModbusRegister(uint16_t addr, RegisterType t, const String& desc,
                   std::optional<float> scale = std::nullopt,
                   std::optional<UnitType> unitType = std::nullopt,
                   std::optional<uint16_t> backendAddr = std::nullopt,
                   std::optional<std::function<double(ModbusCache*, double)>> transformFunc = std::nullopt)
        : address(addr), type(t), description(desc), scalingFactor(scale),
          unit(unitType), backendAddress(backendAddr), transformFunction(transformFunc) {}
};

struct ScaledWaterMarks {
    float highWaterMark;
    float lowWaterMark;
};

class ModbusCache {
public:
    ModbusCache(const std::vector<ModbusRegister>& dynamicRegisters, 
            const std::vector<ModbusRegister>& staticRegisters, 
            const String& serverIPStr, 
            uint16_t port);
    void begin();
    void resetConnection();
    void update();
    void addRegister(const ModbusRegister& reg); // Add a register to the cache
    std::vector<uint16_t> getRegisterValues(uint16_t startAddress, uint16_t count);
    //uint16_t getRegisterValue(uint16_t address);
    uint16_t update_interval = 500;
    bool checkNewRegisterValue(uint16_t address, uint32_t proposedRawValue);
    void setRegisterValue(uint16_t address, uint32_t value, bool is32Bit = false);
    static ModbusMessage respondFromCache(ModbusMessage request);
    // Getter methods
    ModbusServerRTU& getModbusRTUServer();
    ModbusClientRTU* getModbusRTUClient();
    ModbusClientTCPasync* getModbusTCPClient();
    bool getIsOperational() const {
        return isOperational;
    }
    bool getDynamicRegistersFetched() const {
        return dynamicRegistersFetched;
    }
    bool getStaticRegistersFetched() const {
        return staticRegistersFetched;
    }
    std::set<uint16_t> getDynamicRegisterAddresses() const {
        return dynamicRegisterAddresses;
    }
    std::set<uint16_t> getUnexpectedRegisters() const {
        return unexpectedRegisters;
    }
    uint32_t getInsaneCounter() const {
        return insaneCounter;
    }
    std::optional<ModbusRegister> getRegisterDefinition(uint16_t address) const {
        auto it = registerDefinitions.find(address);
        if (it != registerDefinitions.end()) {
            return it->second; // Found the register definition
        }
        return std::nullopt; // Register definition not found
    }
    float getScaledValueFromRegister(const ModbusRegister& reg, uint32_t rawValue);
    float getRegisterScaledValue(uint16_t address);
    ScaledWaterMarks getRegisterWaterMarks(uint16_t address);
    String formatRegisterValue(const ModbusRegister& reg, float value);
    String formatRegisterValue(uint16_t address, float value);
    String getFormattedRegisterValue(uint16_t address);
    String getCGBaudRate();
    void setCGBaudRate(uint16_t baudRateValue);
    std::pair<String, String> getFormattedWaterMarks(uint16_t address);
    void createEmulatedServer(const std::vector<ModbusRegister>& registers);
    // Getters for the metrics
    unsigned long getMinLatency() const { return minLatency; }
    unsigned long getMaxLatency() const { return maxLatency; }
    float getAverageLatency() const { return averageLatency; }
    float getStdDeviation() const {
        if (latencies.size() < 2) {
            return 0.0f; // Return 0 if no data is available
        }
        size_t sampleCount = latencies.size(); // Use the actual number of samples

        float variance = (sumLatencySquared / sampleCount) - (averageLatency * averageLatency);
        return variance > 0 ? sqrtf(variance) : 0.0f;
    }
    void updateLatencyStats(unsigned long latency);

private:
    std::vector<ModbusRegister> registers; // All registers
    std::map<uint16_t, uint16_t> register16BitValues; // Values for 16-bit registers
    std::map<uint16_t, uint32_t> register32BitValues; // Values for 32-bit registers
    std::map<uint16_t, uint32_t> highWaterMarks; // Use uint32_t to accommodate 32-bit registers
    std::map<uint16_t, uint32_t> lowWaterMarks; // Same as above
    std::set<uint16_t> dynamicRegisterAddresses; // Addresses of dynamic registers
    std::set<uint16_t> staticRegisterAddresses; // Addresses of static registers
    std::set<uint16_t> unexpectedRegisters; // Addresses of registers not defined in the cache
    uint32_t insaneCounter = 0;

    std::map<uint16_t, const ModbusRegister> registerDefinitions;

    void updateWaterMarks(uint16_t address, uint32_t value, bool is32Bit);
    bool isStaticRegister(uint16_t registerNumber) {
        // Check if the register number is in the staticRegisterAddresses set
        return staticRegisterAddresses.find(registerNumber) != staticRegisterAddresses.end();
    }

    bool isDynamicRegister(uint16_t registerNumber) {
        // Check if the register number is in the dynamicRegisterAddresses set
        return dynamicRegisterAddresses.find(registerNumber) != dynamicRegisterAddresses.end();
    }

    bool is32BitRegisterType(const ModbusRegister& reg) {
        // Check if the register type is UINT32, INT32, or FLOAT
        return reg.type == RegisterType::UINT32 || reg.type == RegisterType::INT32 || reg.type == RegisterType::FLOAT;
    }
    bool is32BitRegister(uint16_t address) {
        auto it = registerDefinitions.find(address);
        if (it != registerDefinitions.end()) {
            // Check if the register type is either UINT32 or INT32
            return is32BitRegisterType(it->second);
        }
        return false; // Address not found or not a 32-bit register
    }

    bool is16BitRegister(uint16_t address) {
        auto it = registerDefinitions.find(address);
        if (it != registerDefinitions.end()) {
            // Check if the register type is either UINT16 or INT16
            return it->second.type == RegisterType::UINT16 || it->second.type == RegisterType::INT16;
        }
        return false; // Address not found or not a 16-bit register
    }

    // This next function take two RegisterType arguments (source, and destination), and a 32-bit value
    // It returns a pair of 16-bit values in the correct order for modbus (low word first, high word second)

    Uint16Pair convertValue(const ModbusRegister& source, const ModbusRegister& destination, uint32_t value);
    uint16_t read16BitRegister(uint16_t address);
    uint32_t read32BitRegister(uint16_t address);
    void initializeRegisters(const std::vector<ModbusRegister>& dynamicRegisters, 
                                      const std::vector<ModbusRegister>& staticRegisters);
    void write16BitRegister(uint16_t address, uint16_t value);
    void write32BitRegister(uint16_t address, uint32_t value);
    Uint16Pair split32BitRegister(uint32_t value);
    String serverIPString;
    IPAddress currentIPAddress;
    unsigned long lastPollStart = 0;  // Time of the last poll start
    unsigned long lastPollEnd = 0;
    IPAddress serverIP; // IP address of the Modbus TCP server
    uint16_t serverPort; // Port number of the Modbus TCP server
    ModbusServerRTU modbusRTUServer;
    ModbusServerRTU modbusRTUEmulator;
    ModbusServerTCPasync MBserver;
    ModbusClientRTU* modbusRTUClient;
    ModbusClientTCPasync* modbusTCPClient;
    void fetchFromRemote(const std::set<uint16_t>& regAddresses);
    void sendModbusRequest(uint16_t startAddress, uint16_t regCount);
    static ModbusCache* instance;
    WiFiClient wifiClient;
    void ensureTCPConnection();
    static void handleData(ModbusMessage response, uint32_t token);
    void processResponsePayload(ModbusMessage& response, uint16_t startAddress, uint16_t regCount);
    static void handleError(Error error, uint32_t token);// Static instance pointer
    void purgeToken(uint32_t token);
    std::map<uint32_t, std::tuple<uint16_t, uint16_t, unsigned long>> requestMap; // Map to store token -> (startAddress, regCount, timestamp)
    std::queue<uint32_t> insertionOrder; // Queue to store the order in which requests were made
    unsigned long lastSuccessfulUpdate = 0;
    bool isOperational;
    void updateServerStatus();
    std::unordered_set<uint16_t> fetchedStaticRegisters;
    std::unordered_set<uint16_t> fetchedDynamicRegisters;
    bool staticRegistersFetched = false; // Flag to indicate completion of static register fetching
    bool dynamicRegistersFetched = false; // Flag to indicate completion of dynamic register fetching
    SemaphoreHandle_t mutex;

    std::deque<unsigned long> latencies; // Sliding window to store recent latencies
    size_t maxLatencySamples = 100;     // Maximum number of latency samples to track
    unsigned long minLatency = ULONG_MAX; // Minimum latency (in milliseconds)
    unsigned long maxLatency = 0;         // Maximum latency (in milliseconds)
    float averageLatency = 0.0f;          // Average latency (as a float for efficiency)
    float sumLatencySquared = 0.0f;       // Sum of squared latencies (for variance and std dev)
};

#endif // MODBUSCACHE_H
