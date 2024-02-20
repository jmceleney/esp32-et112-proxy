#include "ModbusCache.h"
#include <unordered_set>
#include <functional>

extern Config config;

// Global variables for token and response handling
uint32_t globalToken = 0;

void printHex(const uint8_t *buffer, size_t length) {
    for (size_t i = 0; i < length; ++i)
    {
        char hexString[3];                     // Two characters for the byte and one for the null terminator
        sprintf(hexString, "%02X", buffer[i]); // Format the byte as a two-digit hexadecimal
        dbg(hexString);                        // Send to debug
        if (i < length - 1)
        {
            dbg(" "); // Space between bytes for readability
        }
    }
    dbgln(); // New line after printing the array
}

// Initialize static instance pointer
ModbusCache *ModbusCache::instance = nullptr;

ModbusCache::ModbusCache(const std::vector<ModbusRegister>& dynamicRegisters, 
                         const std::vector<ModbusRegister>& staticRegisters, 
                         const String& serverIPStr, 
                         uint16_t port) :
      serverIPString(serverIPStr),
      serverPort(port),
      modbusRTUServer(2000, config.getModbusRtsPin2()),
      modbusRTUEmulator(2000, -1),
      modbusRTUClient(nullptr),
      modbusTCPClient(nullptr) //,10) // queueLimit 10
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

        
        modbusRTUClient->begin(modbusClientSerial, 1);
    }
    
    if (serverIPString == "127.0.0.1") {
        IPAddress newIP = WiFi.localIP();
        Serial.println("Server IP address is the loopback address (127.0.0.1).");
        dbgln("Using Local IP address: " + newIP.toString());
        serverIP = newIP;
    } else if (!serverIP.fromString(serverIPString)) {
        Serial.println("Error: Invalid IP address. Aborting operation.");
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
    
    mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) {
        dbgln("Failed to create mutex");
    }
    
    RTUutils::prepareHardwareSerial(modbusServerSerial);
    modbusServerSerial.begin(config.getModbusBaudRate2(), config.getModbusConfig2(), 25, 26); // Start the Serial communication

    // Register worker function
    modbusRTUServer.registerWorker(1, ANY_FUNCTION_CODE, &ModbusCache::respondFromCache);
    MBserver.registerWorker(1, ANY_FUNCTION_CODE, &ModbusCache::respondFromCache);

    // Start the Modbus RTU server
    modbusRTUServer.begin(modbusServerSerial); // or modbusRTUServer.begin(modbusServerSerial, -1) to specify coreID
    MBserver.start(config.getTcpPort3(), 20, config.getTcpTimeout());

    if(config.getClientIsRTU()) {
        modbusRTUClient->onDataHandler(&ModbusCache::handleData);
        modbusRTUClient->onErrorHandler(&ModbusCache::handleError);
    } else {
        modbusTCPClient->setMaxInflightRequests(6);
        modbusTCPClient->connect(serverIP, serverPort);
        modbusTCPClient->onDataHandler(&ModbusCache::handleData);
        modbusTCPClient->onErrorHandler(&ModbusCache::handleError);
    }
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
    dbgln("16-bit registers: ");
    for (auto addr : register16BitValues) {
        dbgln(String(addr.first));
    }
    dbgln("32-bit registers: ");
    for (auto addr : register32BitValues) {
        dbgln(String(addr.first));
    }

}

std::vector<uint16_t> ModbusCache::getRegisterValues(uint16_t startAddress, uint16_t count) {
    std::vector<uint16_t> values;
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t address = startAddress + i;
        if (is32BitRegister(address)) {
            // Handle 32-bit register
            uint32_t value32 = read32BitRegister(address);
            Uint16Pair pair = split32BitRegister(value32);
            values.push_back(pair.lowWord);
            values.push_back(pair.highWord);
            ++i; // Skip next address since it's part of the 32-bit value
        } else {
            // Handle 16-bit register
            values.push_back(read16BitRegister(address));
        }
    }
    return values;
}

uint16_t ModbusCache::read16BitRegister(uint16_t address) {
    if (register16BitValues.find(address) != register16BitValues.end()) {
        return register16BitValues[address];
    }
    // Add this address to the set of unexpected registers
    unexpectedRegisters.insert(address);
    return 0; // Placeholder for non-existent register
}

uint32_t ModbusCache::read32BitRegister(uint16_t address) {
    if (is32BitRegister(address)) {
        return register32BitValues[address];
    }
    return 0; // Placeholder for non-existent register
}

Uint16Pair ModbusCache::split32BitRegister(uint32_t value) {
    Uint16Pair result;
    result.highWord = static_cast<uint16_t>(value >> 16); // Extract the high word
    result.lowWord = static_cast<uint16_t>(value & 0xFFFF); // Extract the low word
    return result;
}

void ModbusCache::update() {
    unsigned long currentMillis = millis();

    if (currentMillis - lastPollStart >= update_interval) {
        lastPollStart = currentMillis;

        if (requestMap.size() > 2) {
            dbgln("Skipping update due to more than 2 pending requests");
            //yield();
            return;
        }

        dbgln("Update modbusCache");

        if (!staticRegistersFetched) {
            dbgln("Fetching static registers...");
            fetchFromRemote(staticRegisterAddresses);
        } else {
            dbgln("Fetching dynamic registers...");
            fetchFromRemote(dynamicRegisterAddresses);
        }

        updateServerStatus();
    }
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
        dbgln("New value for register " + String(address) + " is not sane. Rejecting...");
        return;
    }
    if (is32Bit) {
        if (is32BitRegister(address)) {
            if (register32BitValues[address] != value) { // Check if value has changed
                register32BitValues[address] = value;
                updateWaterMarks(address, value, is32Bit);
            }
        } else {
            dbgln("Error: Attempt to write 32-bit value to non-32-bit register at address: " + String(address));
        }
    } else {
        if (is16BitRegister(address)) {
            uint16_t newValue = static_cast<uint16_t>(value);
            if (register16BitValues[address] != newValue) { // Check if value has changed
                register16BitValues[address] = newValue;
                updateWaterMarks(address, newValue, is32Bit);
            }
        } else {
            dbgln("Error: Attempt to write 16-bit value to non-16-bit register or 32-bit register at address: " + String(address));
        }
    }
}

void ModbusCache::fetchFromRemote(const std::set<uint16_t>& regAddresses) {
    if (regAddresses.empty()) return; // Early return if there are no addresses to process

    uint16_t startAddress = *regAddresses.begin();
    uint16_t lastAddress = startAddress;
    bool lastWas32Bit = is32BitRegister(startAddress);
    // Initialize regCount based on the first register size.
    uint16_t regCount = lastWas32Bit ? 2 : 1;

    //dbgln("startAddress: " + String(startAddress));

    for (auto it = std::next(regAddresses.begin()); it != regAddresses.end(); ++it) {
        dbg(String(*it) + ", ");
        uint16_t currentAddress = *it;
        // If this is a static register that we have already fetched, but fetching
        // of all static registers is not yet complete, skip it.
        if (!staticRegistersFetched && fetchedStaticRegisters.find(currentAddress) != fetchedStaticRegisters.end()) {
            continue;
        }

        // If this register is already in the queue, skip it.
        bool isCurrent32Bit = is32BitRegister(currentAddress);

        // Calculate expected next address considering the size of the last register.
        uint16_t expectedNextAddress = lastAddress + (lastWas32Bit ? 2 : 1);

        if (currentAddress == expectedNextAddress) {
            // Address is contiguous with the last one considering register size.
            regCount += isCurrent32Bit ? 2 : 1;
        } else {
            // Found a non-contiguous address, send request for the previous block.
            //dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress));
            sendModbusRequest(startAddress, regCount);

            // Start new block from the current address.
            startAddress = currentAddress;
            regCount = isCurrent32Bit ? 2 : 1;
        }

        // Update for the next iteration.
        lastAddress = currentAddress;
        lastWas32Bit = isCurrent32Bit;

        // Always check at the end of each iteration to send due to request size limit or if it's the last item.
        if (regCount >= 100 || std::next(it) == regAddresses.end()) {
            //dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress));
            sendModbusRequest(startAddress, regCount);

            // Prepare for potentially next block, reset regCount only if not at the end.
            if (std::next(it) != regAddresses.end()) {
                startAddress = *std::next(it);
                lastWas32Bit = is32BitRegister(startAddress);
                regCount = lastWas32Bit ? 2 : 1;
                lastAddress = startAddress;
            }
        }
    }
    //dbgln("]");
}


void ModbusCache::updateServerStatus() {
    if (millis() - lastSuccessfulUpdate > (update_interval + 6000)) {
        isOperational = false;
    } else if(staticRegistersFetched && dynamicRegistersFetched) {
        isOperational = true;
    }
}

void ModbusCache::sendModbusRequest(uint16_t startAddress, uint16_t regCount) {
    if (regCount > 0) {
        ModbusMessage request = ModbusMessage(1, 3, startAddress, regCount);
        uint32_t currentToken = globalToken++;

        // Log start address, register count, and token
        //dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress) + " with token: " + String(currentToken));
        printHex(request.data(), request.size());
        if (xSemaphoreTake(mutex, portMAX_DELAY)) { // Wait indefinitely until the mutex is available
            unsigned long timestamp = millis();
            requestMap[currentToken] = std::make_tuple(startAddress, regCount, timestamp); // Store the request info along with timestamp
            insertionOrder.push(currentToken); // Add the token to the queue
            // Check if the map is at its maximum size
            if (requestMap.size() >= 200) {
                // Remove the oldest entry
                uint32_t oldestToken = insertionOrder.front();
                //dbgln("Removing oldest entry with token: " + String(oldestToken));
                requestMap.erase(oldestToken);
                //dbgln("Erasing token done");
                insertionOrder.pop();
            }
            xSemaphoreGive(mutex); // Release the mutex
        }
        if(config.getClientIsRTU()) {
            modbusRTUClient->addRequest(request, currentToken);
        } else {
            modbusTCPClient->addRequest(request, currentToken);
        }
        
    }
}

// This function handles responses from the Modbus TCP client
void ModbusCache::handleData(ModbusMessage response, uint32_t token) {
    dbgln("Received response for token: " + String(token));
    printHex(response.data(), response.size());
    auto it = instance->requestMap.find(token);
    if (it != instance->requestMap.end()) {
        uint16_t startAddress = std::get<0>(it->second);
        uint16_t regCount = std::get<1>(it->second);
        unsigned long sentTimestamp = std::get<2>(it->second);
        
        unsigned long responseTime = millis() - sentTimestamp;
        dbgln("Response time for token " + String(token) + ": " + String(responseTime) + " ms");

        dbgln("[handleData] Start address: " + String(startAddress) + ", register count: " + String(regCount));
        //printHex(response.data(), response.size());

        instance->processResponsePayload(response, startAddress, regCount);

        instance->lastSuccessfulUpdate = millis();
        instance->purgeToken(token); // Cleanup the token now that the response has been processed

        dbgln("[rcpt:" + String(token) + "] Queue size: " + String(instance->insertionOrder.size()) + ", map size: " + String(instance->requestMap.size()));
    } else {
        dbgln("Token " + String(token) + " not found in map");
    }
    yield();
}

void ModbusCache::handleError(Error error, uint32_t token) {
    // ModbusError wraps the error code and provides a readable error message for it
    ModbusError me(error);
    Serial.printf("Error response: %02X - %s token: %d\n", (int)me, (const char *)me, token);
    instance->purgeToken(token);
}

void ModbusCache::purgeToken(uint32_t token) {
    if (xSemaphoreTake(mutex, portMAX_DELAY)) { // Wait indefinitely until the mutex is available
        std::vector<uint32_t> tokensToPurge; // List of tokens to be purged
        unsigned long currentTime = millis();

        // Add incoming token to the list
        tokensToPurge.push_back(token);

        // Find other tokens that are older than 6000ms
        for (const auto& entry : requestMap) {
            uint32_t currentToken = entry.first;
            unsigned long sentTimestamp = std::get<2>(entry.second);
            if (currentTime - sentTimestamp > 20000) {
                tokensToPurge.push_back(currentToken);
            }
        }

        // Remove tokens from the map
        for (uint32_t purgeToken : tokensToPurge) {
            //dbgln("Erasing token " + String(purgeToken) + " from map");
            requestMap.erase(purgeToken);
            //dbgln("Erasing token done");
        }

        // Efficiently process the queue
        std::queue<uint32_t> tempQueue;
        while (!insertionOrder.empty()) {
            uint32_t currentToken = insertionOrder.front();
            insertionOrder.pop();
            if (std::find(tokensToPurge.begin(), tokensToPurge.end(), currentToken) == tokensToPurge.end()) {
                tempQueue.push(currentToken);
            }
        }
        dbgln("Swapping queues");
        std::swap(insertionOrder, tempQueue);
        dbgln("Swapping done");
        xSemaphoreGive(mutex); // Release the mutex
    }
    dbgln("Erasing tokens done");
}

uint32_t extract32BitValue(const uint8_t* buffer, size_t index) {
    uint32_t value = static_cast<uint32_t>(buffer[index + 2]) << 24 |
                     static_cast<uint32_t>(buffer[index + 3]) << 16 |
                     static_cast<uint32_t>(buffer[index]) << 8 |
                     static_cast<uint32_t>(buffer[index + 1]);

    String debugMsg = "32-bit Value: " + String(buffer[index]) + ", " +
                      String(buffer[index + 1]) + ", " + String(buffer[index + 2]) + ", " +
                      String(buffer[index + 3]);
    //dbgln(debugMsg);
    //dbgln("Resulting 32-bit Value: " + String(value));

    return value;
}

uint16_t extract16BitValue(const uint8_t* buffer, size_t index) {
    // log the 2 bytes in order
    //dbgln("16-bit Value: " + String(buffer[index]) + ", " + String(buffer[index + 1]));
    return static_cast<uint16_t>(buffer[index]) << 8 |
           static_cast<uint16_t>(buffer[index + 1]);
}


void ModbusCache::processResponsePayload(ModbusMessage& response, uint16_t startAddress, uint16_t regCount) {
    dbgln("[processResponsePayload] Processing payload...");
    // Log the payload
    //printHex(response.data(), response.size());
    size_t payloadIndex = 0;
    //const uint8_t* payload = const_cast<ModbusMessage&>(response).data();
    const uint8_t* payload = response.data() + 3;

    for (uint16_t i = 0; i < regCount; ++i) {
        uint16_t currentAddress = startAddress + i;
        bool isStatic = isStaticRegister(currentAddress);
        bool isDynamic = isDynamicRegister(currentAddress);
        if (is32BitRegister(currentAddress)) {
            uint32_t value = extract32BitValue(payload, payloadIndex);
            // dbgln("32-bit Value at address " + String(currentAddress) + ": " + String(value));
            // Log what we know about the register, including its dynamic/static status
            //dbgln("32-bit Value at address " + String(currentAddress) + ": " + String(value) + " (Static: " + String(isStatic) + ", Dynamic: " + String(isDynamic) + ")");

            setRegisterValue(currentAddress, value, true); // true indicates 32-bit operation
            payloadIndex += 4; // Move past the 32-bit value in the payload
            i++; // Skip the next address, as it's part of the 32-bit value
        } else if (is16BitRegister(currentAddress)) {
            uint16_t value = extract16BitValue(payload, payloadIndex);
            //dbgln("16-bit Value at address " + String(currentAddress) + ": " + String(value));
            // Log what we know about the register, including its dynamic/static status
            //dbgln("16-bit Value at address " + String(currentAddress) + ": " + String(value) + " (Static: " + String(isStatic) + ", Dynamic: " + String(isDynamic) + ")");
            setRegisterValue(currentAddress, value); // Default is 16-bit operation
            payloadIndex += 2; // Move past the 16-bit value in the payload
        } else {
            dbgln("Address " + String(currentAddress) + " not defined as 16 or 32 bit. Skipping...");
            continue; // Skip processing this address if it doesn't match known types
        }

        // Update processed registers sets
        if (isStatic) {
            fetchedStaticRegisters.insert(currentAddress);
        } else if (isDynamic) {
            fetchedDynamicRegisters.insert(currentAddress);
        }
    }

    // Efficiently set completion booleans
    if (!staticRegistersFetched) {
        dbgln("staticRegistersFetched: " + String(staticRegistersFetched) + ", staticRegisterAddresses.size(): " + String(staticRegisterAddresses.size()) + ", fetchedStaticRegisters.size(): " + String(fetchedStaticRegisters.size()));
        if (staticRegisterAddresses.size() == fetchedStaticRegisters.size()) {
            staticRegistersFetched = true;
        }
    }
    if (!dynamicRegistersFetched) {
        dbgln("dynamicRegistersFetched: " + String(dynamicRegistersFetched) + ", dynamicRegisterAddresses.size(): " + String(dynamicRegisterAddresses.size()) + ", fetchedDynamicRegisters.size(): " + String(fetchedDynamicRegisters.size()));
        if (dynamicRegisterAddresses.size() == fetchedDynamicRegisters.size()) {
            dynamicRegistersFetched = true;
        } 
    }
}

#include <optional>
#include <cstring> // For memcpy

Uint16Pair ModbusCache::convertValue(const ModbusRegister& source, const ModbusRegister& destination, uint32_t value) {
    dbgln("Converting value from " + typeString(source.type) + " to " + typeString(destination.type) + ": " + String(value));
    
    // The combined scaling factor is the source's scaling factor,
    // as the destination's scaling factor is effectively 1 in this scenario.
    double combinedScalingFactor = 1.0;

    if (source.scalingFactor.has_value()) {
        combinedScalingFactor = source.scalingFactor.value();
    }
    // log the scaling factor
    dbgln("Combined scaling factor: " + String(combinedScalingFactor,4));
    
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
        dbgln("Applying transform function to trueValue with value: " + String(trueValue,3));
        trueValue = static_cast<float>(transformFunction(this, static_cast<double>(trueValue)));
        dbgln("Transformed value: " + String(trueValue,3));
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
    // make a pointer to a hardware serial device, and make it a null pointer
    HardwareSerial* mySerial = nullptr;
    int RX;
    int TX;
    if(config.getClientIsRTU()) {
        // Change the pointer to the hardware serial device to emulatedSerial
        mySerial = &Serial0;
        RX=emulator_RX;
        TX=emulator_TX;
    } else {
        // Change the pointer to the hardware serial device to modbusServerSerial
        mySerial = &modbusClientSerial;
        RX=16;
        TX=17;
    }
    dbgln("Prepare hardware serial");
    RTUutils::prepareHardwareSerial(*mySerial);
    //Logi pin numbers
    dbgln("Calling begin on hardware serial - RX: " + String(RX) + ", TX: " + String(TX) + ", Baud: " + String(config.getModbusBaudRate2()) + ", Config: " + String(config.getModbusConfig2()));
    mySerial->begin(config.getModbusBaudRate2(), config.getModbusConfig2(), RX, TX);
    dbgln("Calling begin on emulated RTU server");
    modbusRTUEmulator.begin(*mySerial);

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
                // Now we need to find the register in "registers" that matches the currentAddress
                // and determine (is32BitRegister) if it is a 16 or 32 bit register
                // is32BitRegister take the register definition and checks if it is a 32 bit register
                // Let's begin by getting the register definition
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
                    dbgln("[emulator] Register: " + String(destReg.address) + ", Description: " + destReg.description +
                     ", Backend Address: " + String(backendAddress));
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
                        dbgln("[emulator] 32-bit destination register: ");
                        dbgln("[emulator] Source register: " + sourceReg.description + ", Scaling factor: " + String(scalingFactor,4) + ", Value: " + String(sourceValue));   
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
                        dbgln("[emulator] 16-bit Source register: " + sourceReg.description + ", Value: " + String(sourceValue) + ", Scaling factor: " + String(scalingFactor,4) +
                            ", sourveValue: " + String(sourceValue));

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
            dbgln("[emulator] Sending response from emulator:");
            printHex(response.data(), response.size());
            return response;
        }

        return ModbusMessage();
    };
    dbgln("Registering worker function for emulated server");
    modbusRTUEmulator.registerWorker(1, ANY_FUNCTION_CODE, onData);
}


ModbusMessage ModbusCache::respondFromCache(ModbusMessage request) {
    dbgln("Received request to local server:");
    printHex(request.data(), request.size());

    uint8_t slaveID = request[0];
    uint8_t functionCode = request[1];
    uint16_t address = (request[2] << 8) | request[3];
    uint16_t valueOrWords = (request[4] << 8) | request[5];

    if (functionCode == 6) { // Write Single Register
        dbgln("Write Single Register - Slave ID: " + String(slaveID) + ", Address: " + String(address) + ", Value: " + String(valueOrWords));

        // Forward the request to the actual device/server.
        ModbusMessage forwardRequest;
        forwardRequest.add(slaveID, functionCode, address, valueOrWords);
        uint32_t currentToken = globalToken++;
        instance->modbusTCPClient->addRequest(forwardRequest, currentToken);

        // Update the value in the cache.
        // The implementation depends on how you've structured your cache. 
        // Here's a simplified example, adjust according to your actual implementation.
        instance->setRegisterValue(address, valueOrWords); // This function needs to be designed to handle 16-bit writes.

        // Assuming a successful write is confirmed by echoing back the request or a specific success message.
        return forwardRequest; // Echo back the request for simplicity, adjust based on actual confirmation needed.
    }

    if (!instance->isOperational) {
        dbgln("Server is not operational, returning no response");
        return ModbusMessage(); // Assuming an empty ModbusMessage indicates no response
    }

    if (functionCode == 3 || functionCode == 4) { // Read Holding Registers or Read Input Registers
        dbgln("Read Registers - Slave ID: " + String(slaveID) + ", Address: " + String(address) + ", Quantity: " + String(valueOrWords));
        
        auto values = instance->getRegisterValues(address, valueOrWords); // Fetch requested values
        if (values.empty()) {
            dbgln("No data available for the requested registers.");
            return ModbusMessage(); // Return an empty message or a specific error response
        }

        ModbusMessage response;
        response.add(slaveID); // Add slave ID
        response.add(functionCode); // Add function code
        response.add(static_cast<uint8_t>(valueOrWords * 2)); // Byte count

        // for (auto it = values.rbegin(); it != values.rend(); ++it) { // reverse this to match the order of the request
        int wordCount = 0;
        for (auto it = values.begin(); it != values.end(); ++it) {
            auto value = *it;   
            response.add(value);
            wordCount++;
            // Stop looping if wordcount equals valueOrWords
            if (wordCount == valueOrWords) { // We might want word one of a 2 word register
                break;
            }
            // Log the value and the MSB and LSB
            //dbgln("Value: " + String(value) + ", MSB: " + String((value >> 8) & 0xFF) + ", LSB: " + String(value & 0xFF));
        }

        dbgln("Sending response from cache:");
        printHex(response.data(), response.size());
        return response;
    }

    return ModbusMessage(); // For unsupported function codes, return an empty message
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
    auto it = registerDefinitions.find(address);
    if (it == registerDefinitions.end()) {
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

    // Use the new function to get the scaled value
    return getScaledValueFromRegister(reg, rawValue);
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