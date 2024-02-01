#include "ModbusCache.h"

extern Config config; // Declare config as extern

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

ModbusCache::ModbusCache(const uint16_t *addressList, size_t addressCount, const uint16_t *addressListStatic, size_t addressStaticCount, String &serverIPStr, uint16_t port)
    : addressList(addressList), // Fast changing registers
      addressCount(addressCount),
      addressListStatic(addressListStatic), // Only have to fetch these once
      addressStaticCount(addressStaticCount),
      serverIPString(serverIPStr),
      serverPort(port),
      modbusRTUServer(2000, config.getModbusRtsPin2()),
      modbusTCPClient(wifiClient,10) // queueLimit 10
{
    if (serverIPString == "127.0.0.1") {
        IPAddress newIP = WiFi.localIP();
        Serial.println("Server IP address is the loopback address (127.0.0.1).");
        dbgln("Using Local IP address: " + newIP.toString());
        serverIP = newIP;
    } else if (!serverIP.fromString(serverIPString)) {
        Serial.println("Error: Invalid IP address. Aborting operation.");
        // Enter a safe state, stop further processing, or reset the system
        while (true)
        {
            delay(1000); // Halt operation
        }
    }

    modbusTCPClient.setTarget(serverIP, serverPort, 1000);
    memset(registerValues, 0, sizeof(registerValues));
    instance = this; // Set the instance pointer to this object
}

void ModbusCache::begin()
{
    // Assuming Serial1 is the HardwareSerial object you want to use
    dbgln("Begin mosbusCache");

    buildUnifiedRegisterList();

    RTUutils::prepareHardwareSerial(Serial1);
    Serial1.begin(config.getModbusBaudRate2(), config.getModbusConfig2(), 25, 26); // Start the Serial communication

    // Start the Modbus RTU server
    modbusRTUServer.begin(Serial1); // or modbusRTUServer.begin(Serial1, -1) to specify coreID

    // Register worker function
    modbusRTUServer.registerWorker(1, ANY_FUNCTION_CODE, &ModbusCache::respondFromCache);

    // Initialize Modbus TCP Client
    modbusTCPClient.begin();
    ensureTCPConnection();

    modbusTCPClient.setTarget(serverIP, serverPort, 1000);
    modbusTCPClient.onDataHandler(&ModbusCache::handleData);
    modbusTCPClient.onErrorHandler(&ModbusCache::handleError);
}

void ModbusCache::buildUnifiedRegisterList() {
    // Add dynamic registers first
    for (size_t i = 0; i < addressCount; ++i) {
        unifiedRegisterList.push_back(addressList[i]);
        registerIndexMap[addressList[i]] = i;
    }

    // Add static registers, avoiding duplicates
    for (size_t i = 0; i < addressStaticCount; ++i) {
        if (registerIndexMap.find(addressListStatic[i]) == registerIndexMap.end()) {
            unifiedRegisterList.push_back(addressListStatic[i]);
            registerIndexMap[addressListStatic[i]] = unifiedRegisterList.size() - 1;
        }
    }
}

void ModbusCache::update()
{
    unsigned long currentMillis = millis();

    // Check if `update_interval` time has passed since last update
    if (currentMillis - lastPollStart >= update_interval)
    {
        lastPollStart = currentMillis; // Update the last poll start time

        // Check the number of pending requests
        if (modbusTCPClient.pendingRequests() > 4) {
            dbgln("Skipping update due to more than 4 pending requests");
            return; // Exit the function if there are more than 4 pending requests
        }

        dbgln("Update modbusCache");
        ensureTCPConnection();

        if (!staticRegistersFetched) {
            fetchFromRemote(addressListStatic, addressStaticCount);
        } else {
            fetchFromRemote(addressList, addressCount);
        }

        updateServerStatusBasedOnCommFailure();
    }
}

void ModbusCache::ensureTCPConnection()
{
    static unsigned long lastConnectionAttemptTime = 0;
    const unsigned long connectionDelay = 2000;

    if (!wifiClient.connected())
    {
        IPAddress newIP = WiFi.localIP();
        if(serverIPString == "127.0.0.1" && newIP != serverIP) {
            dbgln("Local IP address changed from " + serverIP.toString() + " to " + newIP.toString());
            serverIP = newIP;
            modbusTCPClient.setTarget(serverIP, serverPort, 1000);
        }

        unsigned long currentTime = millis();
        dbg("Client not connected");

        // Check if sufficient time has elapsed since the last connection attempt
        if (currentTime - lastConnectionAttemptTime >= connectionDelay)
        {
            dbgln(" - Reconnecting to IP: " + serverIP.toString() + ", port: " + String(config.getTcpPort2()));

            // Reconnect logic
            modbusTCPClient.setTarget(serverIP, config.getTcpPort2());

            // Update the last connection attempt time
            lastConnectionAttemptTime = currentTime;
        }
        else
        {
            dbgln(" - Waiting to reconnect...");
        }
    }
}

uint16_t ModbusCache::getRegisterValue(uint16_t address) {
    auto it = registerIndexMap.find(address);
    if (it != registerIndexMap.end()) {
        return registerValues[it->second];
    }
    return 0; // Address not found
}

void ModbusCache::setRegisterValue(uint16_t address, uint16_t value) {
    auto it = registerIndexMap.find(address);
    if (it != registerIndexMap.end()) {
        registerValues[it->second] = value;
    }
    // If address not found, value setting is skipped
}

void ModbusCache::fetchFromRemote(const uint16_t* regList, size_t regListSize) {
    uint16_t startAddress = regList[0]; // Initialize to the first address

    // log startAddress and the entire regList, in the format [1,2,3...]
    dbg("startAddress: " + String(startAddress) + ", ");
    dbg("regList: [");
    for (size_t i = 0; i < regListSize; ++i)
    {
        dbg(String(regList[i]));
        if (i < regListSize - 1)
        {
            dbg(", ");
        }
    }
    dbgln("]");

    uint16_t regCount = 1; // Start with a count of 1 for the first address

    for (size_t i = 1; i < regListSize; ++i)
    { // Start from the second element
        if (regList[i] == (regList[i - 1] + 1))
        {
            // Address is contiguous with the previous one
            regCount++;
        }
        else
        {
            // Found a non-contiguous address, send request for the previous block
            dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress));
            sendModbusRequest(startAddress, regCount);

            // Start a new block
            startAddress = regList[i];
            regCount = 1;
        }

        // Check if it's time to send a request or if it's the end of the list
        if (regCount >= 100 || i == regListSize - 1)
        {
            dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress));
            sendModbusRequest(startAddress, regCount);
            startAddress = regList[i] + 1; // Prepare for the next block
            regCount = 0;
        }
    }
    wifiClient.flush();
    yield();
}

void ModbusCache::updateServerStatusBasedOnCommFailure()
{
    if (millis() - lastSuccessfulUpdate > (update_interval + 2000))
    {
        isOperational = false;
    }
}

void ModbusCache::sendModbusRequest(uint16_t startAddress, uint16_t regCount)
{
    if (regCount > 0)
    {
        ModbusMessage request = ModbusMessage(1, 3, startAddress, regCount);
        uint32_t currentToken = globalToken++;

        // Log start address, register count, and token
        dbgln("Sending request for " + String(regCount) + " registers starting at " + String(startAddress) + " with token: " + String(currentToken));
        printHex(request.data(), request.size());
        unsigned long timestamp = millis();
        requestMap[currentToken] = std::make_tuple(startAddress, regCount, timestamp); // Store the request info along with timestamp
        insertionOrder.push(currentToken); // Add the token to the queue
        // Check if the map is at its maximum size
        if (requestMap.size() >= 200) {
            // Remove the oldest entry
            uint32_t oldestToken = insertionOrder.front();
            dbgln("Removing oldest entry with token: " + String(oldestToken));
            requestMap.erase(oldestToken);
            dbgln("Erasing token done");
            insertionOrder.pop();
        }
        dbgln("[send:" + String(currentToken) + "] Queue size: " + String(instance->insertionOrder.size()) + ", map size: " + String(instance->requestMap.size()));
        modbusTCPClient.addRequest(request, currentToken);
    }
}

void ModbusCache::handleData(ModbusMessage response, uint32_t token)
{
    dbgln("Received response for token: " + String(token));
    auto it = instance->requestMap.find(token);
    if (it != instance->requestMap.end()) {
        uint16_t startAddress = std::get<0>(it->second);
        uint16_t regCount = std::get<1>(it->second);
        unsigned long sentTimestamp = std::get<2>(it->second);

        unsigned long responseTime = millis() - sentTimestamp;
        dbgln("Response time for token " + String(token) + ": " + String(responseTime) + " ms");

        // Log start address and register count
        dbgln("[handleData] Start address: " + String(startAddress) + ", register count: " + String(regCount));
        printHex(response.data(), response.size());

        // Extract payload from response
        std::vector<uint8_t> payload(response.data() + 3, response.data() + response.size());
        dbg("Received values: [");

        // Process payload
        for (size_t i = 0; i < payload.size(); i += 2)
        {
            uint16_t value = (uint16_t)payload[i] << 8 | payload[i + 1];
            dbg(String(value) + " {" + String(startAddress) + "}, ");
            instance->setRegisterValue(startAddress, value);
            startAddress++;
        }
        dbgln("] DONE");

        if (!instance->staticRegistersFetched) {
            instance->staticRegisterFetchStatus[startAddress] = true;

            // Check if all static registers have been fetched
            instance->staticRegistersFetched = std::all_of(instance->staticRegisterFetchStatus.begin(), 
                                                 instance->staticRegisterFetchStatus.end(), 
                                                 [](const std::pair<uint16_t, bool>& entry) { return entry.second; });

        }

        instance->lastSuccessfulUpdate = millis();
        instance->isOperational = true;
        instance->purgeToken(token);

        // Output queue and map sizes
        dbgln("[rcpt:" + String(token) + "] Queue size: " + String(instance->insertionOrder.size()) + ", map size: " + String(instance->requestMap.size()));
        
    } else {
        // Log the unfound token
        dbgln("Token " + String(token) + " not found in map");
    }
}

void ModbusCache::handleError(Error err, uint32_t token) {
    instance->purgeToken(token);
    dbgln("Error " + String(err) + " for token " + String(token));
}

void ModbusCache::purgeToken(uint32_t token) {
    std::vector<uint32_t> tokensToPurge; // List of tokens to be purged
    unsigned long currentTime = millis();

    // Add incoming token to the list
    tokensToPurge.push_back(token);

    // Find other tokens that are older than 6000ms
    for (const auto& entry : requestMap) {
        uint32_t currentToken = entry.first;
        unsigned long sentTimestamp = std::get<2>(entry.second);
        if (currentTime - sentTimestamp > 4000) {
            tokensToPurge.push_back(currentToken);
        }
    }

    // Remove tokens from the map
    for (uint32_t purgeToken : tokensToPurge) {
        dbgln("Erasing token " + String(purgeToken) + " from map");
        requestMap.erase(purgeToken);
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
    std::swap(insertionOrder, tempQueue);
    dbgln("Erasing tokens done");
}


ModbusMessage ModbusCache::respondFromCache(ModbusMessage request)
{
    dbgln("Received request to local server:");
    printHex(request.data(), request.size());
    if (!instance->isOperational)
    {
        // Server is not operational, return no response
        dbgln("Server is not operational, returning no response");
        return NIL_RESPONSE;
    }

    uint8_t slaveID = request[0];
    uint8_t functionCode = request[1];

    // Log what we know about the request in one line

    if (functionCode == 6)
    { // Write Single Register
        uint16_t address = (uint16_t)request[2] << 8 | request[3];
        uint16_t valueToWrite = (uint16_t)request[4] << 8 | request[5];

        // Log the request details
        dbgln("Write Single Register - Slave ID: " + String(slaveID) + ", Address: " + String(address) + ", Value: " + String(valueToWrite));
        // Create and send the request
        ModbusMessage forwardRequest;
        forwardRequest.add(slaveID, functionCode, address, valueToWrite);
        uint32_t currentToken = globalToken++;

        instance->modbusTCPClient.addRequest(forwardRequest, currentToken);
        instance->setRegisterValue(address, valueToWrite);
        return ECHO_RESPONSE;
    }
    // Check if the function code is 3 or 4
    if (functionCode != 3 && functionCode != 4)
    {
        return NIL_RESPONSE;
    }

    uint16_t address = (uint16_t)request[2] << 8 | request[3];
    uint16_t words = (uint16_t)request[4] << 8 | request[5];
    dbgln("Slave ID: " + String(slaveID) + ", function code: " + String(functionCode) + ", address: " + String(address) + ", words: " + String(words));

    // Preparing response
    uint8_t byteCount = words * 2; // Each register is 2 bytes
    ModbusMessage response;        // Create an empty ModbusMessage
    std::vector<uint8_t> responseBytes = {byteCount};
    response.add(slaveID, functionCode, (uint8_t)byteCount); // Add slave ID, function code, and byte count to the response

    for (uint16_t i = 0; i < words; ++i)
    {
        uint16_t currentValue = instance->getRegisterValue(address + i);
        // Now add uint16_t values to the response
        response.add(currentValue);
    }

    dbgln("Sending response from cache:");
    printHex(response.data(), response.size());

    return response;
}

ModbusServerRTU &ModbusCache::getModbusRTUServer()
{
    return modbusRTUServer;
}

ModbusClientTCP &ModbusCache::getModbusTCPClient()
{
    return modbusTCPClient;
}

float ModbusCache::getVoltage()
{
    int32_t voltsRaw = read32BitSignedValue(VOLTAGE_ADDR);
    return voltsRaw / 10.0f; // Assuming volts are stored as volts*10
}

float ModbusCache::getAmps()
{
    int32_t ampsRaw = read32BitSignedValue(AMPS_ADDR);
    return ampsRaw / 1000.0f; // Assuming amps are stored as amps*1000
}

float ModbusCache::getWatts()
{
    int32_t wattsRaw = read32BitSignedValue(WATTS_ADDR);
    return wattsRaw / 10.0f;
}

float ModbusCache::getPowerFactor()
{
    int16_t powerFactorRaw = getRegisterValue(PF_ADDR);
    return powerFactorRaw / 1000.0f;
}

float ModbusCache::getFrequency()
{
    int16_t frequencyRaw = getRegisterValue(FREQ_ADDR);
    return frequencyRaw / 10.0f;
}

uint32_t ModbusCache::read32BitValue(uint16_t address)
{
    uint16_t lowWord = getRegisterValue(address);      // LSW (Least Significant Word)
    uint16_t highWord = getRegisterValue(address + 1); // MSW (Most Significant Word)
    return (uint32_t)lowWord | ((uint32_t)highWord << 16);
}

int32_t ModbusCache::read32BitSignedValue(uint16_t address)
{
    uint16_t lowWord = getRegisterValue(address);      // LSW (Least Significant Word)
    uint16_t highWord = getRegisterValue(address + 1); // MSW (Most Significant Word)

    // Combine the words into a 32-bit signed integer
    int32_t combined = (int32_t)lowWord | ((int32_t)highWord << 16);

    // Handle the sign bit correctly
    return combined;
}