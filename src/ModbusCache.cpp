#include "ModbusCache.h"

extern Config config; // Declare config as extern

// Global variables for token and response handling
uint32_t globalToken = 0;

void printHex(const uint8_t* buffer, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        char hexString[3]; // Two characters for the byte and one for the null terminator
        sprintf(hexString, "%02X", buffer[i]); // Format the byte as a two-digit hexadecimal
        dbg(hexString); // Send to debug
        if (i < length - 1) {
            dbg(" "); // Space between bytes for readability
        }
    }
    dbgln(); // New line after printing the array
}


// Initialize static instance pointer
ModbusCache* ModbusCache::instance = nullptr;

ModbusCache::ModbusCache(const uint16_t* addressList, size_t addressCount, IPAddress ip, uint16_t port)
    : addressList(addressList), 
      addressCount(addressCount),
      serverIP(ip), 
      serverPort(port),
      modbusRTUServer(2000, config.getModbusRtsPin2()), // Example initialization, adjust as needed
      modbusTCPClient(wifiClient, serverIP, serverPort) {

    memset(registerValues, 0, sizeof(registerValues));
    instance = this; // Set the instance pointer to this object
}

void ModbusCache::begin() {
// Assuming Serial1 is the HardwareSerial object you want to use
    dbgln("Begin mosbusCache");
    RTUutils::prepareHardwareSerial(Serial1);
    Serial1.begin(config.getModbusBaudRate2(), config.getModbusConfig2(), 25, 26); // Start the Serial communication

    // Start the Modbus RTU server
    modbusRTUServer.begin(Serial1); // or modbusRTUServer.begin(Serial1, -1) to specify coreID

    // Register worker function
    modbusRTUServer.registerWorker(1, ANY_FUNCTION_CODE, &ModbusCache::respondFromCache);

    // Initialize Modbus TCP Client
    modbusTCPClient.begin();
    
    modbusTCPClient.setTarget(serverIP, serverPort);
    modbusTCPClient.onDataHandler(&ModbusCache::handleData);
    modbusTCPClient.onErrorHandler(&ModbusCache::handleError);
}

void ModbusCache::update() {
    dbgln("Update modbusCache");
    ensureTCPConnection();
    fetchFromRemote();
}

void ModbusCache::ensureTCPConnection() {
    if (!wifiClient.connected()) {
        // Reconnect logic
        IPAddress ip;
        ip.fromString(config.getTargetIP());
        modbusTCPClient.setTarget(ip, config.getTcpPort2());
    }
}

uint16_t ModbusCache::getRegisterValue(uint16_t address) {
    for (size_t i = 0; i < addressCount; ++i) {
        if (addressList[i] == address) {
            return registerValues[i];
        }
    }
    return 0;
}

void ModbusCache::setRegisterValue(uint16_t address, uint16_t value) {
    // log he length of the addressList
    dbgln("addressList length: " + String(addressCount));
    dbgln("Searching for address: " + String(address));
    for (size_t i = 0; i < addressCount; ++i) {
        if (addressList[i] == address) {
            dbgln("Register " + String(address) + " mapped to index " + String(i) + " set to " + String(value));
            registerValues[i] = value;
            break;
        }
    }
}

void ModbusCache::fetchFromRemote() {
    uint16_t startAddress = addressList[0]; // Initialize to the first address
    
    // log startAddress and the entire addressList, in the format [1,2,3...]
    dbg("startAddress: " + String(startAddress) + ", ");
    dbg("addressList: [");
    for (size_t i = 0; i < addressCount; ++i) {
        dbg(String(addressList[i]));
        if (i < addressCount - 1) {
            dbg(", ");
        }
    }
    dbgln("]");

    uint16_t regCount = 1; // Start with a count of 1 for the first address

    for (size_t i = 1; i < addressCount; ++i) { // Start from the second element
        if (addressList[i] == (addressList[i - 1] + 1)) {
            // Address is contiguous with the previous one
            regCount++;
        } else {
            // Found a non-contiguous address, send request for the previous block
            dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress));
            sendModbusRequest(startAddress, regCount);

            // Start a new block
            startAddress = addressList[i];
            regCount = 1;
        }

        // Check if it's time to send a request or if it's the end of the list
        if (regCount >= 100 || i == addressCount - 1) {
            dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress));
            sendModbusRequest(startAddress, regCount);
            startAddress = addressList[i] + 1; // Prepare for the next block
            regCount = 0;
        }
    }
}


void ModbusCache::sendModbusRequest(uint16_t startAddress, uint16_t regCount) {
    if (regCount > 0) {
        ModbusMessage request = ModbusMessage(1, 4, startAddress, regCount);
        uint32_t currentToken = globalToken++;
        // Log start address, register count, and token
        dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress) + " with token: " + String(currentToken));
        printHex(request.data(), request.size());
        requestMap[currentToken] = std::make_pair(startAddress, regCount); // Store the request info
        modbusTCPClient.addRequest(request, currentToken);
    }
}

void ModbusCache::handleData(ModbusMessage response, uint32_t token) {
    dbgln("Received response for token: " + String(token));
    auto it = instance->requestMap.find(token);
    if (it != instance->requestMap.end()) {
        uint16_t startAddress = it->second.first;
        uint16_t regCount = it->second.second;

        // Log start address and register count
        dbgln("[handleData] Start address: " + String(startAddress) + ", register count: " + String(regCount));        
        printHex(response.data(), response.size());

        // Extract payload from response
        std::vector<uint8_t> payload(response.data() + 3, response.data() + response.size());
        dbg("Received values: [");

        // Process payload
        for (size_t i = 0; i < payload.size(); i += 2) {
            uint16_t value = (uint16_t)payload[i] << 8 | payload[i + 1];
            dbg(String(value) + " {" + String(startAddress) + "}, ");
            instance->setRegisterValue(startAddress, value);
            startAddress++;
        }
        dbgln("] DONE");

        instance->requestMap.erase(it); // Remove the entry from the map
    }
}

void ModbusCache::handleError(Error err, uint32_t token) {
    // Handle errors
}

ModbusMessage ModbusCache::respondFromCache(ModbusMessage request) {
    dbgln("Received request to local server:");
    printHex(request.data(), request.size());
    uint8_t slaveID = request[0];
    uint8_t functionCode = request[1];
    uint16_t address = (uint16_t)request[2] << 8 | request[3];
    uint16_t words = (uint16_t)request[4] << 8 | request[5];

    // Log what we know about the request in one line
    dbgln("Slave ID: " + String(slaveID) + ", function code: " + String(functionCode) + ", address: " + String(address) + ", words: " + String(words));
    
    // Check if the function code is 3 or 4
    if (functionCode != 3 && functionCode != 4) {
        // Return an error message or handle as needed
        return ModbusMessage(); // Placeholder for error handling
    }

    // Preparing response
    uint8_t byteCount = words * 2; // Each register is 2 bytes
    ModbusMessage response; // Create an empty ModbusMessage
    std::vector<uint8_t> responseBytes = {byteCount};
    response.add(slaveID, functionCode, (uint8_t)byteCount); // Add slave ID, function code, and byte count to the response

    for (uint16_t i = 0; i < words; ++i) {
        uint16_t currentValue = instance->getRegisterValue(address + i);
        // Now add uint16_t values to the response
        response.add(currentValue);
    }
    
    dbgln("Sending response from cache:");
    printHex(response.data(), response.size());

    return response;
}

ModbusServerRTU& ModbusCache::getModbusRTUServer() {
    return modbusRTUServer;
}

ModbusClientTCP& ModbusCache::getModbusTCPClient() {
    return modbusTCPClient;
}
