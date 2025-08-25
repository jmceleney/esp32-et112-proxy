#include "ModbusCache.h"
#include "debug.h"
#include "wifi_utils.h"
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <esp_wifi.h>  // Add this at the top with other includes

extern Config config;

// Global variables for token and response handling
uint32_t globalToken = 0;

// Constants for request management 
#define MAX_PENDING_REQUESTS 20  // Maximum number of pending requests allowed
#define REQUEST_TIMEOUT_MS 5000  // Timeout for requests in milliseconds
#define BACKOFF_TIME_MS 2000     // Time to wait after a timeout before sending new requests

// Queue management flags
static bool queueWasFull = false;
static unsigned long queueFullStartTime = 0;
static unsigned long lastQueueStatusLog = 0;

void printHex(const uint8_t *buffer, size_t length) {
    String hexOutput;
    hexOutput.reserve(length * 3); // Each byte takes 2 chars + 1 space
    
    for (size_t i = 0; i < length; ++i) {
        if (buffer[i] < 16) {
            hexOutput += "0"; // Add leading zero for values less than 16
        }
        hexOutput += String(buffer[i], HEX);
        
        if (i < length - 1) {
            hexOutput += " "; // Space between bytes for readability
        }
    }
    
    // Print the entire hex string at once
    dbgln(hexOutput);
}

// Initialize static instance pointer
ModbusCache *ModbusCache::instance = nullptr;

// Add this function before the ModbusCache constructor
void configAmazonFreeRTOS() {
    // Ensure WiFi task has higher priority on Core 0
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Set Modbus task priority lower than WiFi
    vTaskPrioritySet(NULL, tskIDLE_PRIORITY + 1);
}

ModbusCache::ModbusCache(const std::vector<ModbusRegister>& dynamicRegisters, 
                         const std::vector<ModbusRegister>& staticRegisters, 
                         const String& serverIPStr, 
                         uint16_t port) :
    serverIPString(serverIPStr),
    serverPort(port),
    modbusRTUServer(2000, config.getModbusRtsPin2()),
    modbusRTUEmulator(2000, -1),
    modbusRTUClient(nullptr),
    modbusTCPClient(nullptr), //,10) // queueLimit 10
    MBserver(),
    isOperational(false),
    lastLogMessage(""),
    repeatCount(0),
    lastLogTime(0),
    lastRequestTimeout(0)
{
    initializeRegisters(dynamicRegisters, staticRegisters);
    // We must define the client, even if we don't use it, to avoid a null pointer exception
    modbusRTUClient = new ModbusClientRTU(config.getModbusRtsPin(), 10); // queuelimit 10
    modbusRTUClient->setTimeout(1000);
    if(config.getClientIsRTU()) {
        RTUutils::prepareHardwareSerial(modbusClientSerial);
        #if defined(RX_PIN) && defined(TX_PIN)
            // use rx and tx-pins if defined in platformio.ini
            modbusClientSerial.begin(config.getModbusBaudRate(), config.getModbusConfig(), RX_PIN, TX_PIN );
            dbgln("Use user defined RX/TX pins");
        #else
            // otherwise use default pins for hardware-serial2
            modbusClientSerial.begin(config.getModbusBaudRate(), config.getModbusConfig());
        #endif
        
        // Running the RTU client on Core 1 (separate from WiFi on Core 0)
        // to reduce interference between UART and WiFi operations
        modbusRTUClient->begin(modbusClientSerial, RTU_client_core);
    }
    
    if (serverIPString == "127.0.0.1") {
        IPAddress newIP = WiFi.localIP();
        dbgln("Using Local IP address: " + newIP.toString());
        serverIP = newIP;
    } else if (!serverIP.fromString(serverIPString)) {
        logErrln("Error: Invalid IP address. Aborting operation.");
        // Enter a safe state, stop further processing, or reset the system
        while (true) {
            delay(1000); // Halt operation
        }
    }    
    modbusTCPClient = new ModbusClientTCPasync(serverIP, serverPort, 10); // Dynamically allocate ModbusClientTCPasync}
    
    instance = this; // Set the instance pointer to this object
}

void ModbusCache::begin() {
    dbgln("Begin modbusCache");
    
    // Configure FreeRTOS settings for optimal WiFi performance
    configAmazonFreeRTOS();
    
    // Replace standard mutex with recursive mutex
    mutex = xSemaphoreCreateRecursiveMutex();
    if (mutex == NULL) {
        logErrln("Failed to create recursive mutex");
    }
    
    // Add a counter for mutex time statistics
    mutexWaitingTime = 0;
    mutexHoldingTime = 0;
    mutexAcquisitionAttempts = 0;
    mutexAcquisitionFailures = 0;
    maxMutexHoldTime = 0;
    
    RTUutils::prepareHardwareSerial(modbusServerSerial);
    modbusServerSerial.begin(config.getModbusBaudRate2(), config.getModbusConfig2(), RTU_server_RX, RTU_server_TX); // Start the Serial communication
    
    // Modbus RTU server will run on Core 1 (separate from WiFi on Core 0)
    // This separation helps prevent WiFi disconnections caused by UART operations
    
    // Register worker function
    modbusRTUServer.registerWorker(1, ANY_FUNCTION_CODE, &ModbusCache::respondFromCache);
    MBserver.registerWorker(1, ANY_FUNCTION_CODE, &ModbusCache::respondFromCache);

    // Start the Modbus RTU server explicitly on Core 1
    modbusRTUServer.begin(modbusServerSerial, 1);  // Force Core 1
    
    // Optimize TCP server for multiple clients with multiple in-flight requests
    // Increase max connections from default 20 to 30 for better handling of multiple clients
    // Keep the timeout from config for consistency
    MBserver.start(config.getTcpPort3(), 30, config.getTcpTimeout());

    // Set update_interval from config
    update_interval = config.getPollingInterval();

    if(config.getClientIsRTU()) {
        modbusRTUClient->onDataHandler(&ModbusCache::handleData);
        modbusRTUClient->onErrorHandler(&ModbusCache::handleError);
    } else {
        modbusTCPClient->setMaxInflightRequests(10);
        dbgln("Setting up TCP client to [" + serverIP.toString() + "]:[" + String(serverPort)+"]");
        modbusTCPClient->connect(serverIP, serverPort);
        dbgln("TCP connect initiated");
        delay(500);
        modbusTCPClient->onDataHandler(&ModbusCache::handleData);
        modbusTCPClient->onErrorHandler(&ModbusCache::handleError);
    }
    // Initialize lastSuccessfulUpdate to current time
    unsigned long currentTime = millis();
    instance->lastSuccessfulUpdate = currentTime;
    
    // Debug the current millis value and the value of lastSuccessfulUpdate
    dbgln("[begin] Current millis: " + String(currentTime));
    dbgln("[begin] Last successful update: " + String(instance->lastSuccessfulUpdate));
    dbgln("[begin] Time difference: " + String(currentTime - instance->lastSuccessfulUpdate) + "ms");

    // Initialize poll groups for round-robin polling
    initializePollGroups();
}

void ModbusCache::resetConnection() {
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(100))) {
        // Purge all tokens
        requestMap.clear();
        insertionOrder.clear(); // Simply clear the vector instead of using std::swap
        xSemaphoreGiveRecursive(mutex);
    }

    dbgln("Clearing pending requests and resetting Modbus TCP client.");

    // Reset the TCP client
    if (modbusTCPClient) {
        modbusTCPClient->clearQueue();
        modbusTCPClient->resetCounts();
        modbusTCPClient->disconnect();
        
        delay(1000); // Give it time to disconnect
        
        dbgln("Reconnecting to " + serverIP.toString() + ":" + String(serverPort));
        modbusTCPClient->connect(serverIP, serverPort);
        delay(500);
    }

    // Reset relevant flags
    instance->lastSuccessfulUpdate = millis();
    dbgln("[resetConnection] Current millis: " + String(millis()));
    dbgln("[resetConnection] Last successful update: " + String(instance->lastSuccessfulUpdate));
    staticRegistersFetched = false;
    dynamicRegistersFetched = false;
}


void ModbusCache::initializeRegisters(const std::vector<ModbusRegister>& dynamicRegisters, 
                                      const std::vector<ModbusRegister>& staticRegisters) {
    for (const auto& reg : dynamicRegisters) {
        // Log what we're doing
        dbgln("Adding dynamic register at address: " + String(reg.address));
        registers.push_back(reg);
        registerDefinitions.insert({reg.address, reg});
        
        if (reg.type == RegisterType::UINT32 || reg.type == RegisterType::INT32 || reg.type == RegisterType::FLOAT) {
            register32BitValues[reg.address] = 0; // Initialize with default value
        } else {
            register16BitValues[reg.address] = 0; // Initialize with default value
        }
        dynamicRegisterAddresses.insert(reg.address);
    }

    for (const auto& reg : staticRegisters) {
        // Log what we're doing
        dbgln("Adding static register at address: " + String(reg.address));
        registers.push_back(reg);
        registerDefinitions.insert({reg.address, reg});

        if (reg.type == RegisterType::UINT32 || reg.type == RegisterType::INT32) {
            if (register32BitValues.find(reg.address) == register32BitValues.end()) {
                register32BitValues[reg.address] = 0; // Initialize if not already present
            }
        } else {
            if (register16BitValues.find(reg.address) == register16BitValues.end()) {
                register16BitValues[reg.address] = 0; // Initialize if not already present
            }
        }
        staticRegisterAddresses.insert(reg.address);
    }
    // Log which registers are 16 bit and 32 bit
    String reg16BitList = "16-bit registers: ";
    for (auto addr : register16BitValues) {
        reg16BitList += String(addr.first) + " ";
    }
    
    String reg32BitList = "32-bit registers: ";
    for (auto addr : register32BitValues) {
        reg32BitList += String(addr.first) + " ";
    }
    
    dbgln(reg16BitList + "\n" + reg32BitList);
}

std::vector<uint16_t> ModbusCache::getRegisterValues(uint16_t startAddress, uint16_t count) {
    // Pre-allocate the vector to avoid reallocations
    std::vector<uint16_t> values;
    values.reserve(count * 2); // Worst case: all registers are 32-bit (2 words each)
    
    uint16_t i = 0;
    uint16_t currentAddress = startAddress;
    
    while (i < count) {
        if (is32BitRegister(currentAddress)) {
            // Handle 32-bit register
            uint32_t value32 = read32BitRegister(currentAddress);
            Uint16Pair pair = split32BitRegister(value32);
            
            values.push_back(pair.lowWord);
            if (i + 1 < count) { // Only add high word if we haven't reached the requested count
                values.push_back(pair.highWord);
                i++; // Count the high word
            }
            
            currentAddress += 2; // Move to the next register after the 32-bit one
        } else {
            // Handle 16-bit register
            values.push_back(read16BitRegister(currentAddress));
            currentAddress++;
        }
        i++;
    }
    
    return values;
}

uint16_t ModbusCache::read16BitRegister(uint16_t address) {
    // Use find instead of double lookup for better performance
    auto it = register16BitValues.find(address);
    if (it != register16BitValues.end()) {
        return it->second;
    }
    
    // Add this address to the set of unexpected registers
    // Only in debug mode to avoid unnecessary operations in production
    #ifdef DEBUG_MODE
    unexpectedRegisters.insert(address);
    #endif
    
    return 0; // Placeholder for non-existent register
}

uint32_t ModbusCache::read32BitRegister(uint16_t address) {
    // Fast path check for 32-bit register
    if (is32BitRegister(address)) {
        // Use find instead of direct access for better error handling
        auto it = register32BitValues.find(address);
        if (it != register32BitValues.end()) {
            return it->second;
        }
    }
    
    // Log unexpected access only in debug mode
    #ifdef DEBUG_MODE
    dbgln("Attempted to read non-existent 32-bit register at address: " + String(address));
    #endif
    
    return 0; // Placeholder for non-existent register
}

Uint16Pair ModbusCache::split32BitRegister(uint32_t value) {
    Uint16Pair result;
    result.highWord = static_cast<uint16_t>(value >> 16); // Extract the high word
    result.lowWord = static_cast<uint16_t>(value & 0xFFFF); // Extract the low word
    return result;
}

void ModbusCache::logWithCollapsing(const String& message) {
    unsigned long currentTime = millis();
    
    // If this is the same message as the last one and within a reasonable time window
    if (message == lastLogMessage && (currentTime - lastLogTime) < 10000) { // 10 second window
        repeatCount++;
        
        // Only log every 50th occurrence or after 2 seconds since last log
        if (repeatCount % 200 == 0 || (currentTime - lastLogTime) >= 2000) {
            // Create a local String for the message
            String logMsg = message + " (repeated " + String(repeatCount) + " times)";
            dbgln(logMsg);
            lastLogTime = currentTime;
        }
    } else {
        // If we had previous repeated messages, show final count
        if (repeatCount > 1) {
            // Create a local String for the message
            String logMsg = lastLogMessage + " (repeated " + String(repeatCount) + " times total)";
            dbgln(logMsg);
        }
        
        // New message, reset counter
        lastLogMessage = message;
        repeatCount = 1;
        
        // Log this new message directly
        dbgln(message);
        lastLogTime = currentTime;
    }
}

void ModbusCache::update() {
    unsigned long currentMillis = millis();

    // First, purge any aged tokens to clean up timed-out requests
    purgeAgedTokens();

    if (currentMillis - lastPollStart >= update_interval) {
        dbgln("[update] Updating Modbus Cache");
        lastPollStart = currentMillis;
        
        // Initialize ranges if not done yet
        if (registerRanges.empty()) {
            dbgln("[update] Initializing register ranges");
            initializeRegisterRanges();
        }
        
        // First process static registers if not all fetched
        if (!staticRegistersFetched) {
            dbgln("[update] Processing static registers");
            for (auto& range : registerRanges) {
                if (range.isStatic) {
                    processRegisterRange(range);
                }
            }
            
            // Check if all static registers are fetched
            bool allStaticFetched = true;
            for (uint16_t addr : staticRegisterAddresses) {
                if (fetchedStaticRegisters.find(addr) == fetchedStaticRegisters.end()) {
                    allStaticFetched = false;
                    break;
                }
            }
            if (allStaticFetched) {
                staticRegistersFetched = true;
                dbgln("[update] All static registers fetched");
            }
        } else {
            // Process dynamic registers
            for (auto& range : registerRanges) {
                if (!range.isStatic) {
                    // Find the index of this range among dynamic ranges
                    size_t rangeIndex = 0;
                    size_t totalDynamicRanges = 0;
                    
                    // Count dynamic ranges and find current index
                    for (size_t i = 0; i < registerRanges.size(); i++) {
                        if (!registerRanges[i].isStatic) {
                            totalDynamicRanges++;
                            if (&registerRanges[i] == &range) {
                                rangeIndex = totalDynamicRanges;
                            }
                        }
                    }
                    
                    dbgln("[update] Processing dynamic register range " + String(rangeIndex) + " of " + String(totalDynamicRanges));
                    processRegisterRange(range);
                }
            }
        }
        
        updateServerStatus();
        dbgln("[update] Server status updated");
    }
}

void ModbusCache::initializeRegisterRanges() {
    registerRanges.clear();
    
    // First handle static registers
    if (!staticRegisterAddresses.empty()) {
        uint16_t startAddress = *staticRegisterAddresses.begin();
        uint16_t lastAddress = startAddress;
        uint16_t regCount = is32BitRegister(startAddress) ? 2 : 1;
        bool lastWas32Bit = is32BitRegister(startAddress);

        for (auto it = std::next(staticRegisterAddresses.begin()); it != staticRegisterAddresses.end(); ++it) {
            uint16_t currentAddress = *it;
            bool isCurrent32Bit = is32BitRegister(currentAddress);
            uint16_t expectedNextAddress = lastAddress + (lastWas32Bit ? 2 : 1);

            if (currentAddress == expectedNextAddress) {
                regCount += isCurrent32Bit ? 2 : 1;
            } else {
                // Add the current range
                registerRanges.push_back({startAddress, regCount, true, 0, false});
                // Start new range
                startAddress = currentAddress;
                regCount = isCurrent32Bit ? 2 : 1;
            }
            lastAddress = currentAddress;
            lastWas32Bit = isCurrent32Bit;
        }
        // Add the last range
        registerRanges.push_back({startAddress, regCount, true, 0, false});
    }

    // Then handle dynamic registers
    if (!dynamicRegisterAddresses.empty()) {
        uint16_t startAddress = *dynamicRegisterAddresses.begin();
        uint16_t lastAddress = startAddress;
        uint16_t regCount = is32BitRegister(startAddress) ? 2 : 1;
        bool lastWas32Bit = is32BitRegister(startAddress);

        for (auto it = std::next(dynamicRegisterAddresses.begin()); it != dynamicRegisterAddresses.end(); ++it) {
            uint16_t currentAddress = *it;
            bool isCurrent32Bit = is32BitRegister(currentAddress);
            uint16_t expectedNextAddress = lastAddress + (lastWas32Bit ? 2 : 1);

            if (currentAddress == expectedNextAddress) {
                regCount += isCurrent32Bit ? 2 : 1;
            } else {
                // Add the current range
                registerRanges.push_back({startAddress, regCount, false, 0, false});
                // Start new range
                startAddress = currentAddress;
                regCount = isCurrent32Bit ? 2 : 1;
            }
            lastAddress = currentAddress;
            lastWas32Bit = isCurrent32Bit;
        }
        // Add the last range
        registerRanges.push_back({startAddress, regCount, false, 0, false});
    }
}

void ModbusCache::processRegisterRange(RegisterRange& range) {
    unsigned long currentTime = millis();
    
    // Check if request is in flight
    if (range.inFlight) {
        unsigned long inFlightTime = currentTime - range.lastRequestTime;
        
        // Clear the inFlight flag if request has timed out
        if (inFlightTime > REQUEST_TIMEOUT_MS) {
            logErrln("[processRegisterRange] Request timed out after " + String(inFlightTime) + 
                    "ms, clearing inFlight flag and retrying");
            range.inFlight = false;
            // Don't return - continue to send new request
        } else {
            dbgln("[processRegisterRange] Request in flight for " + String(inFlightTime) + "ms, skipping");
            return;
        }
    }
    
    // If this is a static range that's already been fetched, skip it
    if (range.isStatic) {
        bool allFetched = true;
        for (uint16_t addr = range.startAddress; addr < range.startAddress + range.regCount; addr++) {
            if (fetchedStaticRegisters.find(addr) == fetchedStaticRegisters.end()) {
                allFetched = false;
                break;
            }
        }
        if (allFetched) {
            return;
        }
    }
    
    // Check if we need to wait before retrying
    if (currentTime - range.lastRequestTime < RETRY_DELAY_MS) {
        return;
    }
    
    // Send the request
    ModbusMessage request = ModbusMessage(1, 3, range.startAddress, range.regCount);
    uint32_t currentToken = globalToken++;
    
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(100))) {
        requestMap[currentToken] = std::make_tuple(range.startAddress, range.regCount, currentTime);
        xSemaphoreGiveRecursive(mutex);
    }
    
    if (config.getClientIsRTU()) {
        modbusRTUClient->addRequest(request, currentToken);
    } else {
        modbusTCPClient->addRequest(request, currentToken);
    }
    
    range.lastRequestTime = currentTime;
    range.inFlight = true;
    // Log the request time and token for debugging
    dbgln("[processRegisterRange] Sent request for range " + String(range.startAddress) + 
          "-" + String(range.startAddress + range.regCount - 1) + 
          " with token " + String(currentToken) + 
          " at time " + String(currentTime));
    delay(10);
}

void ModbusCache::updateWaterMarks(uint16_t address, uint32_t value, bool is32Bit) {
    auto it = registerDefinitions.find(address);
    if (it == registerDefinitions.end()) {
        // Handle error or log: register definition not found
        return;
    }
    const ModbusRegister& reg = it->second;

    // Convert the value to signed or unsigned based on the register definition before comparison
    if (is32Bit) {
        if (reg.type == RegisterType::UINT32) {
            // Directly compare and update without needing to read the current value
            if (highWaterMarks.find(address) == highWaterMarks.end() || value > highWaterMarks[address]) {
                highWaterMarks[address] = value;
            }
            if (lowWaterMarks.find(address) == lowWaterMarks.end() || value < lowWaterMarks[address]) {
                lowWaterMarks[address] = value;
            }
        } else if (reg.type == RegisterType::INT32) {
            int32_t signedValue = static_cast<int32_t>(value);
            // Direct comparison without reading the current value again
            if (highWaterMarks.find(address) == highWaterMarks.end() || signedValue > static_cast<int32_t>(highWaterMarks[address])) {
                highWaterMarks[address] = static_cast<uint32_t>(signedValue);
            }
            if (lowWaterMarks.find(address) == lowWaterMarks.end() || signedValue < static_cast<int32_t>(lowWaterMarks[address])) {
                lowWaterMarks[address] = static_cast<uint32_t>(signedValue);
            }
        }
    } else { // 16-bit logic
        if (reg.type == RegisterType::UINT16) {
            uint16_t newValue = static_cast<uint16_t>(value);
            // Again, direct comparison without the need for current value
            if (highWaterMarks.find(address) == highWaterMarks.end() || newValue > static_cast<uint16_t>(highWaterMarks[address])) {
                highWaterMarks[address] = newValue;
            }
            if (lowWaterMarks.find(address) == lowWaterMarks.end() || newValue < static_cast<uint16_t>(lowWaterMarks[address])) {
                lowWaterMarks[address] = newValue;
            }
        } else if (reg.type == RegisterType::INT16) {
            int16_t signedValue = static_cast<int16_t>(value);
            // And no need for current value in signed 16-bit logic
            if (highWaterMarks.find(address) == highWaterMarks.end() || signedValue > static_cast<int16_t>(highWaterMarks[address])) {
                highWaterMarks[address] = static_cast<uint16_t>(signedValue);
            }
            if (lowWaterMarks.find(address) == lowWaterMarks.end() || signedValue < static_cast<int16_t>(lowWaterMarks[address])) {
                lowWaterMarks[address] = static_cast<uint16_t>(signedValue);
            }
        }
    }
}

bool ModbusCache::checkNewRegisterValue(uint16_t address, uint32_t proposedRawValue) {
    auto regIt = registerDefinitions.find(address);
    if (regIt == registerDefinitions.end()) {
        return true;
    }
    const ModbusRegister& reg = regIt->second;
    float proposedValue = getScaledValueFromRegister(reg, proposedRawValue);
    float currentValue = getRegisterScaledValue(address);

    // If the register is uninitialized (current value is 0), accept the new value
    if (currentValue == 0.0) {
        return true;
    }

    switch (reg.unit.value_or(UnitType::var)) { // Assuming 'var' as default or no unit type
        case UnitType::KWh:
        case UnitType::KVarh: {
            float diff = std::abs(proposedValue - currentValue);
            // Accept only if the difference is <= 30
            return diff <= 30;
        }
        case UnitType::W:
        case UnitType::VA:
        case UnitType::var: {
            // Value must be within -25000 to +25000
            return proposedValue >= -25000 && proposedValue <= 25000;
        }
        case UnitType::Hz: {
            // Value must be within 40 to 65
            return proposedValue >= 40 && proposedValue <= 65;
        }
        case UnitType::A: {
            // Value must be within -150 to +150
            return proposedValue >= -150 && proposedValue <= 150;
        }
        case UnitType::V:
            // "V" should be a positive number between 205 and 265
            if (proposedValue < 205.0 || proposedValue > 265.0) return false;
            return true;
        default:
            // If unit type does not require specific checks, or is unrecognized, accept the value
            return true;
    }
}


void ModbusCache::setRegisterValue(uint16_t address, uint32_t value, bool is32Bit) {
    bool sane_value = checkNewRegisterValue(address, value);
    if (!sane_value) {
        insaneCounter++;
        logErrln("New value for register " + String(address) + " is not sane. Rejecting...");
        return;
    }
    if (is32Bit) {
        if (is32BitRegister(address)) {
            if (register32BitValues[address] != value) { // Check if value has changed
                register32BitValues[address] = value;
                updateWaterMarks(address, value, is32Bit);
            }
        } else {
            logErrln("Error: Attempt to write 32-bit value to non-32-bit register at address: " + String(address));
        }
    } else {
        if (is16BitRegister(address)) {
            uint16_t newValue = static_cast<uint16_t>(value);
            if (register16BitValues[address] != newValue) { // Check if value has changed
                register16BitValues[address] = newValue;
                updateWaterMarks(address, newValue, is32Bit);
            }
        } else {
            logErrln("Error: Attempt to write 16-bit value to non-16-bit register or 32-bit register at address: " + String(address));
        }
    }
}

void ModbusCache::updateServerStatus() {
    unsigned long currentTime = millis();
    bool shouldBeOperational = false;
    
    // Take mutex to safely check and update the operational state
    if (xSemaphoreTakeRecursive(instance->mutex, pdMS_TO_TICKS(100))) {
        // Handle uint32_t overflow/underflow conditions safely
        unsigned long timeSinceUpdate;
        if (currentTime >= instance->lastSuccessfulUpdate) {
            timeSinceUpdate = currentTime - instance->lastSuccessfulUpdate;
        } else {
            // This case handles millis() overflow or lastUpdate being incorrectly set
            logErrln("[updateServerStatus] Time calculation error: current=" + String(currentTime) + 
                    ", lastUpdate=" + String(instance->lastSuccessfulUpdate));
            // Don't change operational status based on incorrect time calculation
            timeSinceUpdate = 0;
        }
        
        // Use 2000ms instead of 1000ms to achieve the desired 2.5 second timeout
        bool timeout = (timeSinceUpdate > (update_interval + 2000));
        bool completed = (staticRegistersFetched && dynamicRegistersFetched);
        
        // Determine if server should be operational
        shouldBeOperational = !timeout && completed;
        
        // Only log and update if there's a state change
        if (shouldBeOperational != instance->isOperational.load()) {
            if (!shouldBeOperational) {
                // Transitioning to non-operational state
                dbgln("[updateServerStatus] No updates for " + String(timeSinceUpdate / 1000) + 
                      " seconds, marking server as non-operational (current: " + 
                      String(currentTime) + ", last: " + String(instance->lastSuccessfulUpdate) + ")");
            } else {
                // Transitioning to operational state
                dbgln("[updateServerStatus] Server is now operational");
            }
            
            // Update the state atomically
            instance->isOperational.store(shouldBeOperational);
        }
        
        xSemaphoreGiveRecursive(instance->mutex);
    } else {
        // Log mutex acquisition failure but don't change state
        logErrln("[updateServerStatus] Failed to acquire mutex to update server status");
    }
}

// Add a new method to check if we should throttle requests
bool ModbusCache::shouldThrottleRequests() {
    // If we have no poll groups, we should not throttle
    if (pollGroups.empty()) {
        return false;
    }
    
    unsigned long currentTime = millis();
    
    // Check if we have too many pending requests
    size_t pendingCount = 0;
    
    // Get the count of pending requests
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(50))) {
        pendingCount = requestMap.size();
        xSemaphoreGiveRecursive(mutex);
    } else {
        // If we can't get the mutex, assume we should throttle
        dbgln("[shouldThrottleRequests] Failed to acquire mutex, so we should throttle");
        return true;
    }
    
    // Calculate maximum requests based on client type
    size_t maxRequests = config.getClientIsRTU() ? 1 : MAX_PENDING_REQUESTS;
    
    // Calculate the throttling threshold based on how long the queue has been full
    float throttleThreshold;
    
    if (queueWasFull) {
        // If queue has been full, use progressive throttling thresholds
        unsigned long queueFullDuration = currentTime - queueFullStartTime;
        
        if (queueFullDuration > 10000) { // 10+ seconds
            // Very aggressive throttling - only allow 20% of max capacity
            throttleThreshold = 0.2;
        } else if (queueFullDuration > 5000) { // 5-10 seconds
            // More aggressive throttling - only allow 40% of max capacity
            throttleThreshold = 0.4;
        } else { // 0-5 seconds
            // Initial throttling - allow 60% of max capacity
            throttleThreshold = 0.6;
        }
    } else {
        // Normal operation - throttle at 80% of capacity
        throttleThreshold = 0.8;
    }
    
    // Check if we should throttle based on the calculated threshold
    if (pendingCount >= maxRequests * throttleThreshold) {
        // We have reached our throttling threshold
        static unsigned long lastThrottleLog = 0;
        if (currentTime - lastThrottleLog > 5000) {  // Log every 5 seconds to avoid spam
            lastThrottleLog = currentTime;
            logErrln("[throttle] Throttling at " + String(pendingCount) + 
                   " of " + String(maxRequests) + " requests (" + 
                   String(throttleThreshold * 100) + "% threshold)");
        }
        return true;
    }
    
    // No throttling needed
    return false;
}

// Modify the sendModbusRequest method to include throttling
void ModbusCache::sendModbusRequest(uint16_t startAddress, uint16_t regCount) {
    // Check if we should throttle requests
    if (shouldThrottleRequests()) {
        return;
    }
    
    // Add a small delay between requests (10ms as requested)
    delay(10);
    
    // Create the request - use the constructor pattern seen elsewhere in the code
    // Using slave ID 1 and function code 3 (Read Holding Registers)
    ModbusMessage request = ModbusMessage(1, 3, startAddress, regCount);
    
    // Get a new token
    uint32_t currentToken = globalToken++;
    bool mutexAcquired = false;
    
    // Store the request details with timestamp - use a timeout for mutex acquisition
    // to avoid blocking indefinitely if there's contention
    const TickType_t xTicksToWait = pdMS_TO_TICKS(100); // 100ms timeout
    
    // Record mutex acquisition attempt
    mutexAcquisitionAttempts++;
    unsigned long mutexWaitStartTime = millis();
    
    if (xSemaphoreTakeRecursive(mutex, xTicksToWait)) {
        mutexAcquired = true;
        
        // Record mutex acquisition time
        unsigned long mutexAcquiredTime = millis();
        mutexWaitingTime += (mutexAcquiredTime - mutexWaitStartTime);
        
        // Record internal operations
        unsigned long currentTime = millis();
        requestMap[currentToken] = std::make_tuple(startAddress, regCount, currentTime);
        insertionOrder.push_back(currentToken);
        
        // Record mutex release time
        unsigned long mutexReleaseTime = millis();
        unsigned long holdTime = mutexReleaseTime - mutexAcquiredTime;
        mutexHoldingTime += holdTime;
        
        // Update max hold time
        if (holdTime > maxMutexHoldTime) {
            maxMutexHoldTime = holdTime;
        }
        
        // Log if the hold time is significant
        if (holdTime > 50) {  // More than 50ms
            logErrln("[sendModbusRequest] Mutex held for " + String(holdTime) + "ms");
        }
        
        xSemaphoreGiveRecursive(mutex);
    } else {
        // If we couldn't acquire the mutex, log an error
        mutexAcquisitionFailures++;
        logErrln("[sendModbusRequest] Failed to acquire mutex within timeout. Request not sent.");
        return;
    }
    
    // Now do logging outside the mutex
    String hexData = "";
    for (size_t i = 0; i < request.size(); ++i) {
        if (request.data()[i] < 16) {
            hexData += "0"; // Add leading zero for values less than 16
        }
        hexData += String(request.data()[i], HEX);
        
        if (i < request.size() - 1) {
            hexData += " "; // Space between bytes for readability
        }
    }
    
    dbgln("[sendRequest:" + String(currentToken) + "] === Sending Request ===\n"
          "[sendRequest:" + String(currentToken) + "] Start Address: 0x" + String(startAddress, HEX) + " (" + String(startAddress) + ")\n"
          "[sendRequest:" + String(currentToken) + "] Register Count: " + String(regCount) + "\n"
          "[sendRequest:" + String(currentToken) + "] Token: " + String(currentToken) + "\n"
          "[sendRequest:" + String(currentToken) + "] Request data: \n" + hexData);
    
    // Send the request based on client type
    if (config.getClientIsRTU()) {
        modbusRTUClient->addRequest(request, currentToken);
    } else {
        modbusTCPClient->addRequest(request, currentToken);
    }
    
    // Yield to allow other tasks (especially network processing) to run
    yield();
}

void ModbusCache::updateLatencyStats(unsigned long latency) {
    // For the first value, initialize all stats
    if (latencies.empty()) {
        minLatency = latency;
        maxLatency = latency;
        averageLatency = latency;
        sumLatencySquared = static_cast<double>(latency) * latency;
        latencies.push_back(latency);
        return;
    }

    // Update min and max
    minLatency = std::min(minLatency, latency);
    maxLatency = std::max(maxLatency, latency);

    // Handle rolling window when full
    if (latencies.size() == maxLatencySamples) {
        unsigned long oldest = latencies.front();
        latencies.pop_front();

        // Update rolling average
        averageLatency = averageLatency + 
            (static_cast<double>(latency) - oldest) / maxLatencySamples;

        // Update sum of squares
        sumLatencySquared = sumLatencySquared +
            static_cast<double>(latency) * latency -
            static_cast<double>(oldest) * oldest;
    } else {
        // Update running average for non-full window
        size_t newSize = latencies.size() + 1;
        averageLatency = (averageLatency * latencies.size() + latency) / newSize;
        sumLatencySquared += static_cast<double>(latency) * latency;
    }

    latencies.push_back(latency);
}

// This function handles responses from the Modbus TCP client
void ModbusCache::handleData(ModbusMessage response, uint32_t token) {
    // Yield at the beginning of processing
    yield();
    
    // Variables to collect data under mutex
    bool requestFound = false;
    uint16_t startAddress = 0;
    uint16_t regCount = 0;
    unsigned long sentTimestamp = 0;
    unsigned long responseTime = 0;
    String statusReport;
    
    // Record mutex timing
    unsigned long mutexWaitStartTime = millis();
    bool mutexAcquired = false;
    
    if (xSemaphoreTakeRecursive(instance->mutex, pdMS_TO_TICKS(50))) { // Reduced timeout
        mutexAcquired = true;
        unsigned long mutexAcquiredTime = millis();
        instance->mutexWaitingTime += (mutexAcquiredTime - mutexWaitStartTime);
        instance->mutexAcquisitionAttempts++;
        
        // Quickly extract request data and clean up
        auto it = instance->requestMap.find(token);
        if (it != instance->requestMap.end()) {
            requestFound = true;
            startAddress = std::get<0>(it->second);
            regCount = std::get<1>(it->second);
            sentTimestamp = std::get<2>(it->second);
            
            // Calculate response time with fresh millis() call
            responseTime = millis() - sentTimestamp;
            
            // Find and mark the range as not in flight
            for (auto& range : instance->registerRanges) {
                if (range.startAddress == startAddress && range.regCount == regCount) {
                    range.inFlight = false;
                    break;
                }
            }
            
            // Process the response payload (this is the heavy operation)
            instance->processResponsePayload(response, startAddress, regCount);
            instance->lastSuccessfulUpdate = millis();
            
            // Update latency statistics  
            instance->updateLatencyStats(responseTime);
            
            // Clean up the request from the map
            instance->requestMap.erase(token);
            
            // Get status report - keep this brief
            statusReport = instance->getRequestMapStatus();
        }
        
        // Record mutex hold time
        unsigned long mutexReleaseTime = millis();
        instance->mutexHoldingTime += (mutexReleaseTime - mutexAcquiredTime);
        instance->maxMutexHoldTime = max(instance->maxMutexHoldTime, 
                                       mutexReleaseTime - mutexAcquiredTime);
        
        xSemaphoreGiveRecursive(instance->mutex);
    } else {
        // Failed to acquire mutex
        instance->mutexAcquisitionFailures++;
    }
    
    // Do all logging outside the mutex to avoid holding it during I/O
    if (requestFound) {
        dbgln("[handleData] Response time for token " + String(token) + ": " + String(responseTime) + " ms");
        dbgln(statusReport);
    }
    
    yield();
}

void ModbusCache::handleError(Error error, uint32_t token) {
    // ModbusError wraps the error code and provides a readable error message
    ModbusError me(error);
    
    // Log the error with more context
    String errorContext = "Error response: " + String((int)me, HEX) + " - " + String((const char *)me) + 
                         " token: " + String(token);
    
    // Check if this is a TCP-specific error
    bool isTCPError = (error == Error::IP_CONNECTION_FAILED || 
                      error == Error::TCP_HEAD_MISMATCH || 
                      error == Error::ILLEGAL_IP_OR_PORT ||
                      error == Error::TIMEOUT);
    
    if (isTCPError) {
        logErrln("[TCP Error] " + errorContext);
        // Set lastConnectionError to trigger TCP reconnection through instance
        instance->lastConnectionError = millis();
    } else {
        // For other errors (like CRC errors, illegal functions, etc.), just log them
        dbgln("[Modbus Error] " + errorContext);
    }
    
    // Find and mark the range as not in flight
    auto it = instance->requestMap.find(token);
    if (it != instance->requestMap.end()) {
        uint16_t startAddress = std::get<0>(it->second);
        uint16_t regCount = std::get<1>(it->second);
        
        for (auto& range : instance->registerRanges) {
            if (range.startAddress == startAddress && range.regCount == regCount) {
                range.inFlight = false;
                break;
            }
        }
        
        // Clean up the request from the map
        if (xSemaphoreTakeRecursive(instance->mutex, pdMS_TO_TICKS(100))) {
            instance->requestMap.erase(token);
            xSemaphoreGiveRecursive(instance->mutex);
        }
    }
}

// Improved method to schedule reconnection
void ModbusCache::scheduleReconnect() {
    unsigned long currentTime = millis();
    
    // Only attempt reconnection once every 10 seconds
    if (currentTime - lastReconnectAttempt < 10000) {
        logWithCollapsing("[scheduleReconnect] Reconnection attempted too recently, skipping");
        return;
    }
    
    lastReconnectAttempt = currentTime;
    
    if (modbusTCPClient != nullptr) {
        // First reset all pending requests to clear the queue
        resetAllPendingRequests();
        
        // Now handle the connection
        logErrln("[scheduleReconnect] Disconnecting TCP client");
        modbusTCPClient->disconnect();
        delay(300); // Give it time to disconnect
        
        // Try to reconnect multiple times if needed
        bool connected = false;
        for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
            logErrln("[scheduleReconnect] Reconnection attempt " + String(attempt));
            
            modbusTCPClient->connect(serverIP, serverPort);
            delay(500); // Give it time to connect
            
            // Here we would check if connected, but since the client doesn't have an isConnected method,
            // we'll just try multiple times
            
            if (attempt < 3) {
                delay(1000); // Wait between attempts
            }
        }
        
        logErrln("[scheduleReconnect] Reconnection attempts completed");
        
        // Reset connection error flags
        lastConnectionError = 0;
        // We're not using lastRequestTimeout for backoff anymore, but we'll leave it here and just set it to 0
        lastRequestTimeout = 0;
        
        // Reinitialize poll groups to start fresh
        pollGroups.clear();
        initializePollGroups();
    }
}

// Improved TCP connection management
void ModbusCache::ensureTCPConnection() {
    // Only perform this check for TCP clients
    if (config.getClientIsRTU() || modbusTCPClient == nullptr) {
        return;
    }
    
    // Get current time
    unsigned long currentTime = millis();
    
    // Only run this check periodically (every 5 seconds)
    if (currentTime - lastConnectionCheck < 5000) {
        return;
    }
    
    lastConnectionCheck = currentTime;
    
    // Check for various conditions that indicate we should reconnect
    bool shouldReconnect = false;
    
    // 1. If we have had a recent TCP-specific connection error
    if (lastConnectionError > 0 && currentTime - lastConnectionError > 5000) {
        logWithCollapsing("[ensureTCPConnection] Previous TCP connection error detected");
        shouldReconnect = true;
    }
    
    // 2. If we have too many pending requests and haven't reconnected recently
    size_t pendingCount = 0;
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(50))) {
        pendingCount = requestMap.size();
        xSemaphoreGiveRecursive(mutex);
    }
    
    if (pendingCount >= MAX_PENDING_REQUESTS && currentTime - lastReconnectAttempt > 30000) {
        logWithCollapsing("[ensureTCPConnection] Max pending requests reached, possible TCP connection issue");
        shouldReconnect = true;
    }
    
    // 3. If we haven't had a successful update in a long time
    if (currentTime - lastSuccessfulUpdate > 30000) {
        logErrln("[ensureTCPConnection] No successful updates for " + 
               String((currentTime - lastSuccessfulUpdate) / 1000) + " seconds");
        shouldReconnect = true;
    }
    
    // If any condition is met and we haven't tried reconnecting recently
    if (shouldReconnect && currentTime - lastReconnectAttempt > 30000) {
        logErrln("[ensureTCPConnection] TCP connection issues detected, initiating reconnect");
        lastReconnectAttempt = currentTime;
        scheduleReconnect();
    }
}

void ModbusCache::purgeToken(uint32_t token, bool mutexAlreadyHeld) {
    // Variables to store information for deferred logging
    bool tokenFound = false;
    unsigned long elapsed = 0;
    unsigned long currentTime = millis(); // Get time once, outside the mutex
    
    // Only take mutex if not already held
    bool mutexAcquired = mutexAlreadyHeld;
    if (!mutexAlreadyHeld) {
        mutexAcquired = (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    }
    
    if (mutexAcquired) {
        // Check if the token exists in the map
        auto it = requestMap.find(token);
        if (it != requestMap.end()) {
            tokenFound = true;
            
            // Check elapsed time
            unsigned long sentTime = std::get<2>(it->second);
            uint16_t startAddress = std::get<0>(it->second);
            uint16_t regCount = std::get<1>(it->second);
            
            // Handle potential millis() overflow
            elapsed = (currentTime >= sentTime) ? 
                     (currentTime - sentTime) : 
                     (ULONG_MAX - sentTime + currentTime + 1);
                                    
            if (elapsed > REQUEST_TIMEOUT_MS) {
                logErrln("[purgeToken:" + String(token) + "] Request timed out after " + 
                        String(elapsed) + "ms (timeout threshold: " + String(REQUEST_TIMEOUT_MS) + "ms)");
                
                // Mark the corresponding range as not in flight
                for (auto& range : registerRanges) {
                    if (range.startAddress == startAddress && range.regCount == regCount) {
                        range.inFlight = false;
                        break;
                    }
                }
            }
            
            // Remove from map
            requestMap.erase(it);
            
            // Faster removal from insertion order by swapping with last element
            auto orderIt = std::find(insertionOrder.begin(), insertionOrder.end(), token);
            if (orderIt != insertionOrder.end()) {
                // Swap with the last element and pop (avoids shifting elements)
                if (orderIt != insertionOrder.end() - 1) {
                    std::swap(*orderIt, insertionOrder.back());
                }
                insertionOrder.pop_back();
            }
        }
        
        // Only release mutex if we acquired it here
        if (!mutexAlreadyHeld) {
            xSemaphoreGiveRecursive(mutex);
        }
    } else {
        // If we couldn't acquire the mutex, log error and return
        logErrln("[purgeToken:" + String(token) + "] Failed to acquire mutex within timeout for token " + String(token));
        return;
    }
    
    // Now do all the logging outside the mutex
    if (tokenFound) {
        logWithCollapsing("[purgeToken:" + String(token) + "] Purged token " + String(token) + " after " + String(elapsed) + " ms");
    } else {
        // This is not an error; could be a token that was already purged
        logWithCollapsing("[purgeToken:" + String(token) + "] Token " + String(token) + " not found in map");
    }
}

// New method to purge aged tokens periodically, called less frequently
void ModbusCache::purgeAgedTokens() {
    std::vector<uint32_t> agedTokens;
    unsigned long currentTime = millis();
    
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(100))) {
        // Pre-allocate to avoid resizing inside the critical section
        agedTokens.reserve(requestMap.size());
        
        // First, check for aged tokens in the request map
        for (const auto& entry : requestMap) {
            uint32_t token = entry.first;
            unsigned long sentTime = std::get<2>(entry.second);
            
            // Handle potential millis() overflow
            unsigned long elapsed = (currentTime >= sentTime) ? 
                                  (currentTime - sentTime) : 
                                  (ULONG_MAX - sentTime + currentTime + 1);
                               
            if (elapsed > REQUEST_TIMEOUT_MS) {
                agedTokens.push_back(token);
            }
        }
        
        // Now check for stuck inFlight flags
        for (auto& range : registerRanges) {
            if (range.inFlight) {
                unsigned long inFlightTime = currentTime - range.lastRequestTime;
                if (inFlightTime > REQUEST_TIMEOUT_MS) {
                    logErrln("[purgeAgedTokens] Resetting stuck inFlight flag for range " + 
                           String(range.startAddress) + " after " + String(inFlightTime) + "ms");
                    range.inFlight = false;
                }
            }
        }
        
        // Force purge the oldest token if we're at max capacity with no timeouts
        if (requestMap.size() >= MAX_PENDING_REQUESTS && agedTokens.empty() && !insertionOrder.empty()) {
            logErrln("[purgeAgedTokens] Queue full, force-purging oldest request");
            agedTokens.push_back(insertionOrder.front());
        }
        
        if (!agedTokens.empty()) {
            logErrln("[purgeAgedTokens] Purging " + String(agedTokens.size()) + " aged tokens");
        }
        
        // Purge tokens while still holding the mutex
        for (uint32_t token : agedTokens) {
            purgeToken(token, true);
        }
        
        xSemaphoreGiveRecursive(mutex);
    } else {
        logErrln("[purgeAgedTokens] Failed to acquire mutex");
    }
}

uint32_t extract32BitValue(const uint8_t* buffer, size_t index) {
    // Combine bytes directly without string operations for debugging
    return static_cast<uint32_t>(buffer[index + 2]) << 24 |
           static_cast<uint32_t>(buffer[index + 3]) << 16 |
           static_cast<uint32_t>(buffer[index]) << 8 |
           static_cast<uint32_t>(buffer[index + 1]);
}

uint16_t extract16BitValue(const uint8_t* buffer, size_t index) {
    // Combine bytes directly without string operations for debugging
    return static_cast<uint16_t>(buffer[index]) << 8 |
           static_cast<uint16_t>(buffer[index + 1]);
}


void ModbusCache::processResponsePayload(ModbusMessage& response, uint16_t startAddress, uint16_t regCount) {
    dbgln("[processResponsePayload] Processing payload...");
    
    // Get payload pointer once
    const uint8_t* payload = response.data() + 3;
    size_t payloadIndex = 0;
    
    // Pre-check if we need to update the register sets
    bool needToCheckStaticCompletion = !staticRegistersFetched;
    bool needToCheckDynamicCompletion = !dynamicRegistersFetched;
    
    // Yield before processing to ensure we don't trigger watchdog
    yield();
    
    // Process registers in a single pass
    for (uint16_t i = 0; i < regCount; ++i) {
        uint16_t currentAddress = startAddress + i;
        
        // Check register type only once
        bool is32Bit = is32BitRegister(currentAddress);
        bool is16Bit = !is32Bit && is16BitRegister(currentAddress);
        
        // Skip processing if register type is unknown
        if (!is32Bit && !is16Bit) {
            logWithCollapsing("[processResponsePayload] Address " + String(currentAddress) + " not defined as 16 or 32 bit. Skipping...");
            continue;
        }
        
        // Process based on register type
        if (is32Bit) {
            uint32_t value = extract32BitValue(payload, payloadIndex);
            setRegisterValue(currentAddress, value, true); // true indicates 32-bit operation
            payloadIndex += 4; // Move past the 32-bit value in the payload
            i++; // Skip the next address, as it's part of the 32-bit value
        } else { // is16Bit
            uint16_t value = extract16BitValue(payload, payloadIndex);
            setRegisterValue(currentAddress, value); // Default is 16-bit operation
            payloadIndex += 2; // Move past the 16-bit value in the payload
        }
        
        // Update processed registers sets - only if needed
        if (needToCheckStaticCompletion && isStaticRegister(currentAddress)) {
            fetchedStaticRegisters.insert(currentAddress);
        } else if (needToCheckDynamicCompletion && isDynamicRegister(currentAddress)) {
            fetchedDynamicRegisters.insert(currentAddress);
        }
        
        // Yield after processing every 5 registers to prevent watchdog timeout
        if (i % 5 == 0) {
            yield();
        }
    }
    
    // Yield before completing the method
    yield();

    // Efficiently set completion booleans - only check if needed
    String statusLogs = "";
    
    if (needToCheckStaticCompletion) {
        if (staticRegisterAddresses.size() == fetchedStaticRegisters.size()) {
            staticRegistersFetched = true;
        }
        statusLogs += "[processResponsePayload] staticRegistersFetched: " + String(staticRegistersFetched) + 
              ", staticRegisterAddresses.size(): " + String(staticRegisterAddresses.size()) + 
              ", fetchedStaticRegisters.size(): " + String(fetchedStaticRegisters.size()) + "\n";              
    }
    
    if (needToCheckDynamicCompletion) {
        statusLogs += "[processResponsePayload] dynamicRegistersFetched: " + String(dynamicRegistersFetched) + 
              ", dynamicRegisterAddresses.size(): " + String(dynamicRegisterAddresses.size()) + 
              ", fetchedDynamicRegisters.size(): " + String(fetchedDynamicRegisters.size());
              
        if (dynamicRegisterAddresses.size() == fetchedDynamicRegisters.size()) {
            dynamicRegistersFetched = true;
        } 
    }
    
    if (statusLogs.length() > 0) {
        logWithCollapsing(statusLogs);
    }
    
    // Final yield before exiting
    yield();
    
    dbgln("[processResponsePayload] Done processing payload");
}

#include <optional>
#include <cstring> // For memcpy

Uint16Pair ModbusCache::convertValue(const ModbusRegister& source, const ModbusRegister& destination, uint32_t value) {
    //dbgln("Converting value from " + typeString(source.type) + " to " + typeString(destination.type) + ": " + String(value));
    
    // The combined scaling factor is the source's scaling factor,
    // as the destination's scaling factor is effectively 1 in this scenario.
    double combinedScalingFactor = 1.0;

    if (source.scalingFactor.has_value()) {
        combinedScalingFactor = source.scalingFactor.value();
    }
    // log the scaling factor
    //dbgln("Combined scaling factor: " + String(combinedScalingFactor,4));
    
    float trueValue = 0;
    uint32_t tempValue = 0;

    if (source.type == RegisterType::FLOAT) {
        // If source is FLOAT, interpret the input value directly as float
        memcpy(&trueValue, &value, sizeof(float));
        trueValue *= combinedScalingFactor; // Apply scaling factor
    } else {
        // If source is INT32, apply the scaling factor before conversion
        int32_t intValue;
        memcpy(&intValue, &value, sizeof(int32_t)); // Treat the value as int32_t for scaling
        trueValue = static_cast<float>(intValue) * combinedScalingFactor;
    }

    // if destination.transformFunction is present, apply it to the trueValue
    if (destination.transformFunction.has_value()) {
        std::function<double(ModbusCache*, double)> transformFunction = destination.transformFunction.value();
        //dbgln("Applying transform function to trueValue with value: " + String(trueValue,3));
        trueValue = static_cast<float>(transformFunction(this, static_cast<double>(trueValue)));
        //dbgln("Transformed value: " + String(trueValue,3));
    }

    // Convert trueValue to the destination type
    if (destination.type == RegisterType::FLOAT) {
        // If destination is FLOAT, no further conversion needed
        memcpy(&tempValue, &trueValue, sizeof(float)); // Store the float value as uint32_t for return
    } else {
        // Handle conversion to other types if needed, though not required for this specific scenario
    }

    return split32BitRegister(tempValue);
}


void ModbusCache::createEmulatedServer(const std::vector<ModbusRegister>& registers) {
    // EXPERIMENTAL - Never used
    // make a pointer to a hardware serial device, and make it a null pointer
    //HardwareSerial Serial(0);
    HardwareSerial* mySerial = nullptr;
    int RX;
    int TX;
    #ifdef REROUTE_DEBUG
        mySerial = &Serial0;
        RX=emulator_RX;
        TX=emulator_TX;
    #else
        // Change the pointer to the hardware serial device to modbusServerSerial
        mySerial = &modbusClientSerial;
        RX=16;
        TX=17;
    #endif
    dbgln("[emulator] Prepare hardware serial");
    RTUutils::prepareHardwareSerial(*mySerial);
    //Logi pin numbers
    uint32_t baudRate = config.getModbusBaudRate2();
    dbgln("[emulator] Calling begin on hardware serial - RX: " + String(RX) + ", TX: " + String(TX) + ", Baud: " + String(baudRate) + ", Config: " + String(config.getModbusConfig2()));
    mySerial->begin(baudRate, config.getModbusConfig2(), RX, TX);
    dbgln("[emulator] Calling begin on emulated RTU server");
    modbusRTUEmulator.begin(*mySerial, RTU_emulator_core);

    auto onData = [this, registers](ModbusMessage request) {
        dbgln("[emulator] Received request to emulated server:");
        printHex(request.data(), request.size());

        uint8_t slaveID = request[0];
        uint8_t functionCode = request[1];
        uint16_t address = (request[2] << 8) | request[3];
        uint16_t valueOrWords = (request[4] << 8) | request[5];

        if (!instance->isOperational) {
            dbgln("[emulator] Server is not operational, returning no response");
            return ModbusMessage();
        }

        if (functionCode == 3 || functionCode == 4) {
            ModbusMessage response;
            response.add(slaveID); // Add slave ID
            response.add(functionCode); // Add function code
            response.add(static_cast<uint8_t>(valueOrWords * 2)); // Byte count
            dbgln("[emulator] Function code: " + String(functionCode) + ", Address: " + String(address) + ", Value or Words: " + String(valueOrWords));

            // for (auto it = values.rbegin(); it != values.rend(); ++it) { // reverse this to match the order of the request
            int wordCount = 0;

            // Now we loop through the addresses and determin which registers
            // if any they match. A 32 bit register occupies two addresses
            // Once we know whoch emaulated register is requested, we fetch the value
            // from our backend, and add it to our response. Request addresses that
            // do not exist will result in a 0 value in the response
            //std::vector<uint16_t> values;
            for (uint16_t i = 0; i < valueOrWords; ++i) {
                uint16_t currentAddress = address + i;
                // Log what we're doing
                dbgln("[emulator] Fetching value for address: " + String(currentAddress));
                auto it = std::find_if(registers.begin(), registers.end(), [currentAddress](const ModbusRegister& reg) {
                    return reg.address == currentAddress;
                });
                if (it != registers.end()) {
                    // We found a register definition
                    ModbusRegister destReg = *it;
                    // Log what we know about the register, including the description and backend address
                    

                    // backendAddress is optional. If not present, then we need skip
                    // If it is present, then assign it to a int32_t variable
                    uint16_t backendAddress = UINT16_MAX; // Marker value indicating "undefined";
                    if(destReg.backendAddress.has_value()) {
                        backendAddress = destReg.backendAddress.value();
                    }
                    // dbgln("[emulator] Register: " + String(destReg.address) + ", Description: " + destReg.description +
                    //  ", Backend Address: " + String(backendAddress));
                    if (backendAddress == UINT16_MAX) {
                        dbgln("[emulator] No backend address found for address: " + String(currentAddress));
                        if (this->is32BitRegisterType(destReg)) {
                            response.add(0);
                            response.add(0);
                            i++;
                            wordCount += 2;
                        } else {
                            // values.push_back(0);
                            response.add(0);
                            wordCount++;
                        }
                        continue;
                    }
                    auto iReg = registerDefinitions.find(backendAddress);
                    if (iReg == registerDefinitions.end()) {
                        dbgln("[emulator] No register definition found for backend address: " + String(backendAddress));
                    }
                    // Get a ModbusRegister reference from iReg->second
                    const ModbusRegister& sourceReg = iReg->second;
                    float scalingFactor = 1.0;
                    if (sourceReg.scalingFactor.has_value()) {
                        scalingFactor = sourceReg.scalingFactor.value();
                    }

                    uint32_t sourceValue;
                    if(this->is32BitRegister(backendAddress)) {
                        sourceValue = this->read32BitRegister(backendAddress);
                    } else {
                        sourceValue = static_cast<uint32_t>(this->read16BitRegister(backendAddress));
                    }
                    Uint16Pair pair = this->convertValue(sourceReg, destReg, sourceValue);
                    if (this->is32BitRegisterType(destReg)) {
                        // dbgln("[emulator] 32-bit destination register: ");
                        // dbgln("[emulator] Source register: " + sourceReg.description + ", Scaling factor: " + String(scalingFactor,4) + ", Value: " + String(sourceValue));   
                        response.add(pair.highWord);
                        wordCount++;
                        if (wordCount == valueOrWords) { // We might want word one of a 2 word register
                              break;
                        }
                        response.add(pair.lowWord);
                        i++;
                        wordCount++;
                    } else {
                        // Log the name, value and scalingFactor of the source register
                        // dbgln("[emulator] 16-bit Source register: " + sourceReg.description + ", Value: " + String(sourceValue) + ", Scaling factor: " + String(scalingFactor,4) +
                        //     ", sourveValue: " + String(sourceValue));

                        response.add(pair.lowWord);
                        wordCount++;
                    }
                } else {
                    // We did not find a register definition
                    // We add a 0 value to our response
                    dbgln("[emulator] No register definition found for address: " + String(currentAddress));
                    // values.push_back(0);
                    response.add(0);
                    wordCount++;
                }

                if (wordCount == valueOrWords) { // We might want word one of a 2 word register
                   break;
                }
            }
            // dbgln("[emulator] Sending response from emulator:");
            printHex(response.data(), response.size());
            return response;
        }

        return ModbusMessage();
    };
    dbgln("Registering worker function for emulated server");
    modbusRTUEmulator.registerWorker(1, ANY_FUNCTION_CODE, onData);
}


ModbusMessage ModbusCache::respondFromCache(ModbusMessage request) {
    // Extract request parameters once
    const uint8_t slaveID = request[0];
    const uint8_t functionCode = request[1];
    const uint16_t address = extract16BitValue(request.data(), 2);
    const uint16_t valueOrWords = extract16BitValue(request.data(), 4);

    // Early exit for non-operational state (atomic check, no mutex needed)
    if (!instance->isOperational.load()) {
        return ModbusMessage();
    }

    // Pre-allocate response buffer
    ModbusMessage response;
    
    // Record start time for monitoring
    unsigned long startTime = millis();
    
    // Take mutex with a shorter timeout to prevent blocking too long
    if (xSemaphoreTakeRecursive(instance->mutex, pdMS_TO_TICKS(50))) {
        // Double-check operational state after acquiring mutex
        if (!instance->isOperational.load()) {
            xSemaphoreGiveRecursive(instance->mutex);
            return ModbusMessage();
        }

        // Handle write single register (function code 6)
        if (functionCode == 6) {
            ModbusMessage forwardRequest;
            forwardRequest.add(slaveID, functionCode, address, valueOrWords);
            uint32_t currentToken = globalToken++;
            
            // Update cache first
            instance->setRegisterValue(address, valueOrWords, false);
            
            // Release mutex before forwarding request
            xSemaphoreGiveRecursive(instance->mutex);
            
            instance->modbusTCPClient->addRequest(forwardRequest, currentToken);
            return forwardRequest;
        }

        // Handle read holding registers or read input registers (function codes 3 or 4)
        if (functionCode == 3 || functionCode == 4) {
            // Pre-allocate vector to collect values within critical section
            std::vector<uint16_t> values;
            values.reserve(valueOrWords * 2); // Reserve space for worst case
            
            uint16_t i = 0;
            uint16_t currentAddress = address;
            
            // Collect all values quickly without yields
            while (i < valueOrWords) {
                // Check processing time to prevent holding mutex too long
                if (millis() - startTime > 30) { // Reduced timeout
                    xSemaphoreGiveRecursive(instance->mutex);
                    return ModbusMessage(); // Return empty response if taking too long
                }
                
                if (instance->is32BitRegister(currentAddress)) {
                    uint32_t value32 = instance->read32BitRegister(currentAddress);
                    Uint16Pair pair = instance->split32BitRegister(value32);
                    
                    values.push_back(pair.lowWord);
                    if (i + 1 < valueOrWords) {
                        values.push_back(pair.highWord);
                        i++;
                    }
                    currentAddress += 2;
                } else {
                    values.push_back(instance->read16BitRegister(currentAddress));
                    currentAddress++;
                }
                i++;
            }
            
            // Release mutex before building response
            xSemaphoreGiveRecursive(instance->mutex);
            
            // Build response outside critical section
            response.add(slaveID);
            response.add(functionCode);
            response.add(static_cast<uint8_t>(values.size() * 2));
            
            for (uint16_t val : values) {
                response.add(val);
            }
            
            // Log if operation took unusually long
            unsigned long duration = millis() - startTime;
            if (duration > 50) {
                logErrln("[respondFromCache] Long operation: " + String(duration) + "ms for " + String(valueOrWords) + " registers");
            }
            
            return response;
        }
        
        // For any other function codes, release mutex and return empty
        xSemaphoreGiveRecursive(instance->mutex);
    } else {
        // If we couldn't acquire the mutex quickly, return empty response
        return ModbusMessage();
    }
    
    return response;
}


ModbusServerRTU &ModbusCache::getModbusRTUServer() {
    return modbusRTUServer;
}

ModbusClientTCPasync* ModbusCache::getModbusTCPClient() {
    return modbusTCPClient;
}

ModbusClientRTU* ModbusCache::getModbusRTUClient() {
    return modbusRTUClient;
}

float ModbusCache::getScaledValueFromRegister(const ModbusRegister& reg, uint32_t rawValue) {
    float value = 0.0;

    // Use RegisterType to determine the appropriate conversion
    switch (reg.type) {
        case RegisterType::UINT32:
            value = static_cast<float>(rawValue);
            break;
        case RegisterType::INT32:
            value = static_cast<float>(static_cast<int32_t>(rawValue));
            break;
        case RegisterType::UINT16:
            value = static_cast<float>(static_cast<uint16_t>(rawValue));
            break;
        case RegisterType::INT16:
            value = static_cast<float>(static_cast<int16_t>(rawValue));
            break;
        case RegisterType::FLOAT:
            value = static_cast<float>(rawValue);
        // Add cases for other RegisterType enums if necessary
        default:
            // Handle unexpected register type or log an error
            break;
    }

    // Apply scaling factor if present
    if (reg.scalingFactor.has_value()) {
        value *= reg.scalingFactor.value();
    }

    return value;
}


float ModbusCache::getRegisterScaledValue(uint16_t address) {
    // Take mutex with timeout
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(100))) {
        auto it = registerDefinitions.find(address);
        if (it == registerDefinitions.end()) {
            xSemaphoreGiveRecursive(mutex);
            return 0.0; // Register not found
        }
        const ModbusRegister& reg = it->second;
        uint32_t rawValue = 0;

        // Read the raw value based on the register's bit width
        if (is32BitRegister(address)) {
            rawValue = read32BitRegister(address);
        } else if (is16BitRegister(address)) {
            rawValue = static_cast<uint32_t>(read16BitRegister(address));
        }

        // Get the scaled value
        float result = getScaledValueFromRegister(reg, rawValue);
        
        xSemaphoreGiveRecursive(mutex);
        return result;
    }
    
    // If we failed to get the mutex, return 0
    logErrln("[getRegisterScaledValue] Failed to acquire mutex within timeout");
    return 0.0;
}

String ModbusCache::formatRegisterValue(const ModbusRegister& reg, float value) {
    char buffer[50];
    if (reg.unit.has_value()) {
        switch (reg.unit.value()) {
            case UnitType::V: snprintf(buffer, sizeof(buffer), "%.1f V", value); break;
            case UnitType::A: snprintf(buffer, sizeof(buffer), "%.3f A", value); break;
            case UnitType::W: snprintf(buffer, sizeof(buffer), "%.1f W", value); break;
            case UnitType::PF: snprintf(buffer, sizeof(buffer), "%.3f", value); break;
            case UnitType::Hz: snprintf(buffer, sizeof(buffer), "%.1f Hz", value); break;
            case UnitType::KWh: snprintf(buffer, sizeof(buffer), "%.1f kWh", value); break;
            case UnitType::KVarh: snprintf(buffer, sizeof(buffer), "%.1f kVARh", value); break;
            case UnitType::VA: snprintf(buffer, sizeof(buffer), "%.1f VA", value); break;
            case UnitType::var: snprintf(buffer, sizeof(buffer), "%.1f var", value); break;
            // Add more units as needed
            default: snprintf(buffer, sizeof(buffer), "%f", value);
        }
    } else {
        snprintf(buffer, sizeof(buffer), "%f", value); // Default formatting
    }
    return String(buffer);
}

String ModbusCache::formatRegisterValue(uint16_t address, float value) {
    auto it = registerDefinitions.find(address);
    if (it == registerDefinitions.end()) {
        return "N/A"; // Register not found
    }
    const ModbusRegister& reg = it->second;
    return formatRegisterValue(reg, value); // Use the new function
}

// Now combine the two functions, and provide a formatted string for a given register address
String ModbusCache::getFormattedRegisterValue(uint16_t address) {
    float value = getRegisterScaledValue(address);
    return formatRegisterValue(address, value);
}

String ModbusCache::getCGBaudRate() {
    // Get the scaled value from the Modbus register
    float value = getRegisterScaledValue(8193);

    // Convert the register value to the corresponding baud rate
    String baudRate;
    switch (static_cast<int>(value)) {
        case 1:
            baudRate = "9.6 kbps";
            break;
        case 2:
            baudRate = "19.2 kbps";
            break;
        case 3:
            baudRate = "38.4 kbps";
            break;
        case 4:
            baudRate = "57.6 kbps";
            break;
        case 5:
            baudRate = "115.2 kbps";
            break;
        default:
            baudRate = "9.6 kbps"; // Default value for any other case
            break;
    }

    return baudRate;
}

void ModbusCache::setCGBaudRate(uint16_t baudRateValue) {
    // Validate that the value is between 1 and 5
    if (baudRateValue < 1 || baudRateValue > 5) {
        dbgln("Invalid baud rate value. Must be between 1 and 5.");
        return;
    }

    // Only proceed if the client is RTU
    if (!config.getClientIsRTU()) {
        dbgln("Cannot set baud rate. Client is not configured for RTU.");
        return;
    }

    // Create the Modbus request to write to the baud rate register (8193 / 0x2001)
    uint16_t startAddress = 0x2001; // Physical address for baud rate
    uint16_t regValue = baudRateValue;

    ModbusMessage request = ModbusMessage(1, 6, startAddress, regValue); // Function code 6 = Write Single Register
    uint32_t currentToken = globalToken++;

    // Mutex protection for request map and queue
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(100))) {
        unsigned long timestamp = millis();
        requestMap[currentToken] = std::make_tuple(startAddress, 1, timestamp); // Register count is 1
        insertionOrder.push_back(currentToken);

        // Check if the map is at its maximum size
        if (requestMap.size() >= 200) {
            uint32_t oldestToken = insertionOrder.front();
            requestMap.erase(oldestToken);
            insertionOrder.erase(insertionOrder.begin()); // Remove the first element
        }
        xSemaphoreGiveRecursive(mutex);
    }

    // Send the request
    Error err = modbusRTUClient->addRequest(request, currentToken);

    // Check for errors
    if (err != SUCCESS) {
        dbgln("Error adding baud rate set request: " + String((int)err));
    } else {
        dbgln("Baud rate set request sent successfully.");
    }
}


ScaledWaterMarks ModbusCache::getRegisterWaterMarks(uint16_t address) {
    ScaledWaterMarks waterMarks{0.0f, 0.0f}; // Initialize to default values

    auto regIt = registerDefinitions.find(address);
    if (regIt == registerDefinitions.end()) {
        // Handle error: Register not found
        return waterMarks;
    }
    const ModbusRegister& reg = regIt->second;

    // Assuming highWaterMarks and lowWaterMarks store raw values
    uint32_t rawHighMark = highWaterMarks.find(address) != highWaterMarks.end() ? highWaterMarks[address] : 0;
    uint32_t rawLowMark = lowWaterMarks.find(address) != lowWaterMarks.end() ? lowWaterMarks[address] : 0;

    // Scale the high and low water marks using the new function
    waterMarks.highWaterMark = getScaledValueFromRegister(reg, rawHighMark);
    waterMarks.lowWaterMark = getScaledValueFromRegister(reg, rawLowMark);

    return waterMarks;
}

std::pair<String, String> ModbusCache::getFormattedWaterMarks(uint16_t address) {
    // First, fetch the scaled water marks
    ScaledWaterMarks waterMarks = getRegisterWaterMarks(address);

    // Fetch the register definition to use in formatting
    auto it = registerDefinitions.find(address);
    if (it == registerDefinitions.end()) {
        // If the register isn't found, return a default pair of empty strings
        return std::make_pair(String(""), String(""));
    }
    const ModbusRegister& reg = it->second;

    // Format the high and low water marks
    String formattedHigh = formatRegisterValue(reg, waterMarks.highWaterMark);
    String formattedLow = formatRegisterValue(reg, waterMarks.lowWaterMark);

    // Return the formatted water marks as a pair of strings
    return std::make_pair(formattedHigh, formattedLow);
}


// Comprehensive method to fetch all system data in a single atomic operation
ModbusCache::SystemSnapshot ModbusCache::fetchSystemSnapshot(const std::set<uint16_t>& addresses) {
    SystemSnapshot snapshot;
    
    // Structure to hold raw data collected under mutex
    struct RawRegisterData {
        ModbusRegister definition;
        uint32_t rawValue;
        uint32_t rawHighMark;
        uint32_t rawLowMark;
        bool is32Bit;
        
        // Constructor to initialize properly
        RawRegisterData(const ModbusRegister& def) 
            : definition(def), rawValue(0), rawHighMark(0), rawLowMark(0), is32Bit(false) {}
    };
    
    std::map<uint16_t, RawRegisterData> rawData;
    float baudRateValue = 0;
    
    // Acquire mutex for minimum time - collect only raw data
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(50))) { // Reduced timeout to 50ms
        // Fetch the insane counter
        snapshot.insaneCounter = insaneCounter;
        
        // Copy the unexpected registers set
        snapshot.unexpectedRegisters = unexpectedRegisters;
        
        // Get CG Baud Rate (register 8193) - get raw value only
        auto baudIt = registerDefinitions.find(8193);
        if (baudIt != registerDefinitions.end()) {
            uint32_t rawBaudValue = 0;
            if (is32BitRegister(8193)) {
                rawBaudValue = read32BitRegister(8193);
            } else if (is16BitRegister(8193)) {
                rawBaudValue = static_cast<uint32_t>(read16BitRegister(8193));
            }
            baudRateValue = getScaledValueFromRegister(baudIt->second, rawBaudValue);
        }
        
        // Process each register address - collect raw data only
        for (const auto& address : addresses) {
            auto defIt = registerDefinitions.find(address);
            if (defIt != registerDefinitions.end()) {
                RawRegisterData regData(defIt->second);
                regData.is32Bit = is32BitRegister(address);
                
                // Get raw value based on register type
                if (regData.is32Bit) {
                    regData.rawValue = read32BitRegister(address);
                } else if (is16BitRegister(address)) {
                    regData.rawValue = static_cast<uint32_t>(read16BitRegister(address));
                } else {
                    regData.rawValue = 0;
                }
                
                // Get raw water marks
                regData.rawHighMark = highWaterMarks.find(address) != highWaterMarks.end() ? highWaterMarks[address] : 0;
                regData.rawLowMark = lowWaterMarks.find(address) != lowWaterMarks.end() ? lowWaterMarks[address] : 0;
                
                rawData.emplace(address, std::move(regData));
            }
        }
        
        xSemaphoreGiveRecursive(mutex);
    } else {
        logErrln("[fetchSystemSnapshot] Failed to acquire mutex within timeout");
        return snapshot; // Return early with empty snapshot
    }
    
    // Process baud rate outside mutex
    switch (static_cast<int>(baudRateValue)) {
        case 1: snapshot.cgBaudRate = "9.6 kbps"; break;
        case 2: snapshot.cgBaudRate = "19.2 kbps"; break;
        case 3: snapshot.cgBaudRate = "38.4 kbps"; break;
        case 4: snapshot.cgBaudRate = "57.6 kbps"; break;
        case 5: snapshot.cgBaudRate = "115.2 kbps"; break;
        default: snapshot.cgBaudRate = "Unknown"; break;
    }
    
    // Process all formatting outside mutex to avoid deadlock
    for (const auto& [address, regData] : rawData) {
        RegisterSnapshot regSnapshot;
        regSnapshot.definition = regData.definition;
        
        // Format value outside mutex
        float scaledValue = getScaledValueFromRegister(regData.definition, regData.rawValue);
        regSnapshot.formattedValue = formatRegisterValue(regData.definition, scaledValue);
        
        // Format water marks outside mutex
        float highMarkScaled = getScaledValueFromRegister(regData.definition, regData.rawHighMark);
        float lowMarkScaled = getScaledValueFromRegister(regData.definition, regData.rawLowMark);
        
        String formattedHigh = formatRegisterValue(regData.definition, highMarkScaled);
        String formattedLow = formatRegisterValue(regData.definition, lowMarkScaled);
        
        regSnapshot.waterMarks = std::make_pair(formattedHigh, formattedLow);
        
        // Add to result map
        snapshot.registers[address] = regSnapshot;
    }
    
    return snapshot;
}

// Add a new method to reset all pending requests
void ModbusCache::resetAllPendingRequests() {
    // Use a timeout for mutex acquisition to avoid blocking indefinitely
    const TickType_t xTicksToWait = pdMS_TO_TICKS(100); // 100ms timeout
    
    // Variables to store information for deferred logging
    size_t requestCount = 0;
    std::vector<uint32_t> tokens;
    
    if (xSemaphoreTakeRecursive(mutex, xTicksToWait)) {
        // Store count for logging after mutex release
        requestCount = requestMap.size();
        
        // Collect all tokens for diagnostic logging
        tokens.reserve(requestCount);
        for (const auto& entry : requestMap) {
            tokens.push_back(entry.first);
        }
        
        // Clear all pending requests
        requestMap.clear();
        insertionOrder.clear();
        
        // Reset the lastRequestTimeout to avoid backoff period
        lastRequestTimeout = 0;
        
        xSemaphoreGiveRecursive(mutex); // Release mutex before logging
    } else {
        // If we couldn't acquire the mutex, log an error
        logErrln("[resetAllPendingRequests] Failed to acquire mutex within timeout");
        return;
    }
    
    // Now log outside the mutex
    if (requestCount > 0) {
        String tokenList = "";
        for (size_t i = 0; i < std::min(tokens.size(), size_t(5)); i++) {
            if (!tokenList.isEmpty()) tokenList += ", ";
            tokenList += String(tokens[i]);
        }
        if (tokens.size() > 5) {
            tokenList += ", ... (" + String(tokens.size() - 5) + " more)";
        }
        
        logErrln("[resetAllPendingRequests] Cleared " + String(requestCount) + 
               " pending requests. Tokens: " + tokenList);
        
        // Force a small delay to let the system stabilize
        delay(20);
    } else {
        logWithCollapsing("[resetAllPendingRequests] No pending requests to clear.");
    }
}

// New method to initialize poll groups
void ModbusCache::initializePollGroups() {
    pollGroups.clear();
    
    // Create optimal batches for static registers
    if (!staticRegisterAddresses.empty()) {
        createOptimalRegisterBatches(std::vector<uint16_t>(
            staticRegisterAddresses.begin(), staticRegisterAddresses.end()), true);
    }
    
    // Create optimal batches for dynamic registers
    if (!dynamicRegisterAddresses.empty()) {
        createOptimalRegisterBatches(std::vector<uint16_t>(
            dynamicRegisterAddresses.begin(), dynamicRegisterAddresses.end()), false);
    }
    
    // Optimize the final poll groups
    optimizePollGroups(pollGroups);
    
    // Reset round-robin state
    currentGroupIndex = 0;
    staticGroupsCompleted = false;
    
    // Log the created poll groups
    String pollGroupsLog = "Created " + String(pollGroups.size()) + " poll groups:";
    
    for (size_t i = 0; i < pollGroups.size(); i++) {
        const PollGroup& group = pollGroups[i];
        String addressList = "";
        for (uint16_t addr : group.addresses) {
            if (!addressList.isEmpty()) addressList += ", ";
            addressList += String(addr);
        }
        pollGroupsLog += "\nGroup " + String(i) + ": " + (group.isStatic ? "Static" : "Dynamic") + 
              " - Addresses: " + addressList;
    }
    
    dbgln(pollGroupsLog);
}

// Create optimal register batches based on address continuity and register type
void ModbusCache::createOptimalRegisterBatches(const std::vector<uint16_t>& addresses, bool isStatic) {
    if (addresses.empty()) return;
    
    const uint16_t MAX_BATCH_SIZE = 50; // Maximum registers per batch
    
    // Sort addresses for optimal batching
    std::vector<uint16_t> sortedAddresses = addresses;
    std::sort(sortedAddresses.begin(), sortedAddresses.end());
    
    PollGroup currentGroup;
    currentGroup.isStatic = isStatic;
    currentGroup.pollInterval = isStatic ? 0 : update_interval; // Static groups don't need regular polling
    
    // Start with the first address
    uint16_t startAddress = sortedAddresses[0];
    uint16_t lastAddress = startAddress;
    bool lastWas32Bit = is32BitRegister(startAddress);
    uint16_t currentGroupSize = lastWas32Bit ? 2 : 1;
    currentGroup.addresses.push_back(startAddress);
    
    for (size_t i = 1; i < sortedAddresses.size(); i++) {
        uint16_t currentAddress = sortedAddresses[i];
        bool isCurrent32Bit = is32BitRegister(currentAddress);
        uint16_t expectedNextAddress = lastAddress + (lastWas32Bit ? 2 : 1);
        
        // Check if this register would fit in the current batch
        bool isContiguous = (currentAddress == expectedNextAddress);
        bool wouldExceedMaxSize = (currentGroupSize + (isCurrent32Bit ? 2 : 1)) > MAX_BATCH_SIZE;
        
        if (isContiguous && !wouldExceedMaxSize) {
            // Add to current group
            currentGroup.addresses.push_back(currentAddress);
            currentGroupSize += (isCurrent32Bit ? 2 : 1);
        } else {
            // Save current group and start a new one
            if (!currentGroup.addresses.empty()) {
                pollGroups.push_back(currentGroup);
                currentGroup.addresses.clear();
            }
            
            // Start new group
            currentGroup.addresses.push_back(currentAddress);
            currentGroupSize = isCurrent32Bit ? 2 : 1;
        }
        
        // Update for next iteration
        lastAddress = currentAddress;
        lastWas32Bit = isCurrent32Bit;
    }
    
    // Add the last group if not empty
    if (!currentGroup.addresses.empty()) {
        pollGroups.push_back(currentGroup);
    }
}

// Optimize poll groups for more efficient polling
void ModbusCache::optimizePollGroups(std::vector<PollGroup>& groups) {
    // Nothing to optimize if we have 0 or 1 groups
    if (groups.size() <= 1) return;
    
    // This is where additional optimizations could be implemented
    // For example, balancing group sizes, adjusting poll intervals, etc.
    
    // For now, we'll just ensure static groups come first in the round-robin
    std::stable_sort(groups.begin(), groups.end(), 
        [](const PollGroup& a, const PollGroup& b) {
            return a.isStatic && !b.isStatic;
        });
}

// Process the next poll group in the round-robin
void ModbusCache::processNextPollGroup() {
    // If no poll groups, initialize them
    if (pollGroups.empty()) {
        initializePollGroups();
        if (pollGroups.empty()) {
            return;
        }
    }
    
    // Find the next group that doesn't have an in-flight request
    size_t groupsChecked = 0;
    bool foundEligibleGroup = false;
    
    while (groupsChecked < pollGroups.size() && !foundEligibleGroup) {
        PollGroup& group = pollGroups[currentGroupIndex];
        
        // Only skip if the group has an in-flight request
        bool hasInFlightRequest = false;
        for (const auto& range : registerRanges) {
            if (range.startAddress == group.addresses[0] && range.inFlight) {
                hasInFlightRequest = true;
                break;
            }
        }
        
        if (!hasInFlightRequest) {
            foundEligibleGroup = true;
        } else {
            currentGroupIndex = (currentGroupIndex + 1) % pollGroups.size();
            groupsChecked++;
        }
    }
    
    if (!foundEligibleGroup) {
        // Log if we checked all groups and found none eligible
        logErrln("[processNextPollGroup] All groups have in-flight requests");
        return;
    }
    
    // Get the current group
    PollGroup& group = pollGroups[currentGroupIndex];
    
    // Handle empty groups (shouldn't happen, but just in case)
    if (group.addresses.empty()) {
        group.completed = true;
        currentGroupIndex = (currentGroupIndex + 1) % pollGroups.size();
        return;
    }
    
    // Find contiguous ranges within the group to minimize Modbus requests
    std::vector<std::pair<uint16_t, uint16_t>> ranges; // pairs of (startAddress, regCount)
    
    // Sort addresses to optimize for finding contiguous ranges
    std::sort(group.addresses.begin(), group.addresses.end());
    
    // Start with the first address
    uint16_t startAddress = group.addresses[0];
    uint16_t lastAddress = startAddress;
    bool lastWas32Bit = is32BitRegister(startAddress);
    uint16_t regCount = lastWas32Bit ? 2 : 1;
    
    // Optimize batch sizes - don't make batches too large to avoid timeouts
    const uint16_t MAX_REGISTERS_PER_BATCH = 24; // Limit batch size
    
    for (size_t i = 1; i < group.addresses.size(); i++) {
        uint16_t currentAddress = group.addresses[i];
        bool isCurrent32Bit = is32BitRegister(currentAddress);
        uint16_t expectedNextAddress = lastAddress + (lastWas32Bit ? 2 : 1);
        
        // Check if this would exceed our max batch size
        uint16_t potentialRegCount = regCount + (isCurrent32Bit ? 2 : 1);
        
        if (currentAddress == expectedNextAddress && potentialRegCount <= MAX_REGISTERS_PER_BATCH) {
            // Contiguous and within size limit, extend current range
            regCount = potentialRegCount;
        } else {
            // Not contiguous or would exceed size limit, finish current range and start new one
            ranges.push_back(std::make_pair(startAddress, regCount));
            startAddress = currentAddress;
            regCount = isCurrent32Bit ? 2 : 1;
        }
        
        // Update for next iteration
        lastAddress = currentAddress;
        lastWas32Bit = isCurrent32Bit;
    }
    
    // Add the last range
    ranges.push_back(std::make_pair(startAddress, regCount));
    
    // Check if we have capacity to send all the requests
    size_t maxConcurrentRequests = config.getClientIsRTU() ? 1 : MAX_PENDING_REQUESTS;
    
    // Get current pending requests count
    size_t currentPendingRequests = 0;
    if (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(50))) {
        currentPendingRequests = requestMap.size();
        xSemaphoreGiveRecursive(mutex);
    } else {
        logWithCollapsing("[processNextPollGroup] Failed to acquire mutex, skipping poll cycle");
        return;
    }
    
    // Calculate how many requests we can send
    int requestsAvailable = maxConcurrentRequests - currentPendingRequests;
    
    // For RTU, we only send one at a time
    // For TCP, leave a small safety buffer to prevent edge cases
    if (!config.getClientIsRTU()) {
        requestsAvailable = std::min(requestsAvailable, static_cast<int>(MAX_PENDING_REQUESTS - 1));
    }
    
    if (requestsAvailable <= 0) {
        // No capacity to send requests, will try again later
        logWithCollapsing("[processNextPollGroup] No capacity to send requests, will try again later");
        return;
    }
    
    // Send as many requests as we can, limited only by available capacity and number of ranges
    size_t requestsToSend = std::min(static_cast<size_t>(requestsAvailable), ranges.size());
    
    // Send Modbus requests for each range (up to our limit)
    for (size_t i = 0; i < requestsToSend; i++) {
        sendModbusRequest(ranges[i].first, ranges[i].second);
        yield(); // Allow other tasks to run
    }
    
    // Update group status
    if (group.isStatic) {
        // Only mark static groups complete if we sent all ranges
        if (requestsToSend >= ranges.size()) {
            group.completed = true;
        }
    }
    
    // Move to next group for round-robin
    currentGroupIndex = (currentGroupIndex + 1) % pollGroups.size();
}

String ModbusCache::getRequestMapStatus() {
    // This function assumes the mutex is already held by the caller
    String status;
    status.reserve(200); // Pre-allocate space for the string
    
    // Count in-flight requests
    size_t inFlightCount = 0;
    unsigned long currentTime = millis();
    unsigned long minAge = ULONG_MAX;
    unsigned long maxAge = 0;
    
    for (const auto& range : registerRanges) {
        if (range.inFlight) {
            inFlightCount++;
        }
    }
    
    // Calculate ages of requests
    for (const auto& entry : requestMap) {
        unsigned long age = currentTime - std::get<2>(entry.second);
        minAge = std::min(minAge, age);
        maxAge = std::max(maxAge, age);
    }
    
    // If no requests, set minAge to 0
    if (requestMap.empty()) {
        minAge = 0;
    }
    
    status = "[RequestMap Status] Total: " + String(requestMap.size()) + 
             ", In-Flight: " + String(inFlightCount) + 
             ", Age Range: " + String(minAge) + "ms to " + String(maxAge) + "ms";
             
    return status;
}