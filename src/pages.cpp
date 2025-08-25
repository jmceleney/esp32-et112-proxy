#include "pages.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>


#define ETAG "\"" __DATE__ "" __TIME__ "\""
// Define a build time string for display on the status page
#define BUILD_TIME_STR __DATE__ " " __TIME__

// Check if GIT_VERSION is defined by our pre-build script
#ifndef GIT_VERSION
#define GIT_VERSION "Unknown"
#endif

// External variable declaration for WiFi connection time
extern unsigned long lastWiFiConnectionTime;

// Variables for connection handling
static int activeConnections = 0;
static const int MAX_CONNECTIONS = 10; // Maximum concurrent connections

// Static cache variables for BSSID lookup
static String cachedBSSID;
static String cachedPayload;

// Helper function to log heap memory at the start of each page request
void logHeapMemory(const char* route) {
  String message = String("[webserver] GET ") + route + " - Free heap: " + String(ESP.getFreeHeap()) + " bytes";
  dbgln(message);
}

// Helper function for handling connection limits
bool canAcceptConnection() {
  if (activeConnections >= MAX_CONNECTIONS) {
    dbgln("[webserver] Too many active connections: " + String(activeConnections) + " - Rejecting new connection");
    return false;
  }
  activeConnections++;
  return true;
}

// Helper function to release connection count
void releaseConnection() {
  if (activeConnections > 0) {
    activeConnections--;
  }
}

void setupPages(AsyncWebServer *server, ModbusCache *modbusCache, Config *config, WiFiManager *wm){
    server->on("/metrics", HTTP_GET, [modbusCache](AsyncWebServerRequest *request) {
    logHeapMemory("/metrics");

    String response;

    // ESP specific metrics
    unsigned long uptime = millis() / 1000;
    response += String("esp_uptime_seconds ") + String(uptime) + "\n";
    response += String("esp_rssi ") + String(WiFi.RSSI()) + "\n";
    response += String("esp_heap_free_bytes ") + String(ESP.getFreeHeap()) + "\n";

    // Modbus metrics
    ModbusClientRTU* rtu = modbusCache->getModbusRTUClient();
    response += String("modbus_primary_rtu_messages ") + String(rtu->getMessageCount()) + "\n";
    response += String("modbus_primary_rtu_pending_messages ") + String(rtu->pendingRequests()) + "\n";
    response += String("modbus_primary_rtu_errors ") + String(rtu->getErrorCount()) + "\n";

    ModbusClientTCPasync* modbusTCPClient = modbusCache->getModbusTCPClient();
    response += String("modbus_secondary_tcp_messages ") + String(modbusTCPClient->getMessageCount()) + "\n";
    response += String("modbus_secondary_tcp_errors ") + String(modbusTCPClient->getErrorCount()) + "\n";

    ModbusServerRTU& modbusRTUServer = modbusCache->getModbusRTUServer();
    response += String("modbus_server_messages ") + String(modbusRTUServer.getMessageCount()) + "\n";
    response += String("modbus_server_errors ") + String(modbusRTUServer.getErrorCount()) + "\n";
    response += String("modbus_static_registers_fetched ") + String(modbusCache->getStaticRegistersFetched() ? 1 : 0) + "\n";
    response += String("modbus_dynamic_registers_fetched ") + String(modbusCache->getDynamicRegistersFetched() ? 1 : 0) + "\n";
    response += String("modbus_operational ") + String(modbusCache->getIsOperational() ? 1 : 0) + "\n";
    response += String("modbus_bogus_register_count ") + String(modbusCache->getInsaneCounter()) + "\n";
    response += String("min_latency_ms ") + String(modbusCache->getMinLatency()) + "\n";
    response += String("max_latency_ms ") + String(modbusCache->getMaxLatency()) + "\n";
    response += String("average_latency_ms ") + String(modbusCache->getAverageLatency()) + "\n";
    response += String("std_deviation_latency_ms ") + String(modbusCache->getStdDeviation()) + "\n";

    // Add dynamic registers
    for (auto& address : modbusCache->getDynamicRegisterAddresses()) {
        String formattedValue = modbusCache->getFormattedRegisterValue(address);
        auto regDef = modbusCache->getRegisterDefinition(address);
        if (regDef.has_value()) {
            String metricName = regDef->description;
            metricName.replace("(", ""); // Remove parentheses
            metricName.replace(")", ""); // Remove parentheses
            metricName.replace("-", ""); // Remove hyphens
            metricName.replace("+", ""); // Remove plus signs
            metricName.trim();
            metricName.replace(" ", "_"); // Replace spaces with underscores for Prometheus compliance
            metricName.toLowerCase();

            // Remove units from values
            formattedValue.replace(" V", "");
            formattedValue.replace(" A", "");

            formattedValue.replace(" W", "");
            formattedValue.replace(" VA", "");
            formattedValue.replace(" var", "");
            formattedValue.replace(" kWh", "");
            formattedValue.replace(" kVARh", "");
            formattedValue.replace(" Hz", "");
            formattedValue.replace("A", "");

            response += metricName + " " + formattedValue + "\n";
        }
    }

    // Send the response to Prometheus
    request->send(200, "text/plain", response);
  });


  server->on("/lookup", HTTP_GET, [](AsyncWebServerRequest *request) {
    logHeapMemory("/lookup");
      if (!request->hasParam("bssid")) {
          request->send(400, "application/json", "{\"error\":\"Missing BSSID parameter\"}");
          return;
      }

      String bssid = request->getParam("bssid")->value();
      
      // Check if we have a cached result for this BSSID
      if (bssid == cachedBSSID && cachedPayload.length() > 0) {
          dbgln("[BSSID] Cache hit for " + bssid);
          request->send(200, "application/json", cachedPayload);
          return;
      }

      dbgln("[BSSID] Cache miss for " + bssid + ", fetching from API");
      WiFiClient client;
      HTTPClient http;
      String url = "http://api.maclookup.app/v2/macs/" + bssid;

      if (http.begin(client, url)) {
          int httpCode = http.GET();
          if (httpCode == HTTP_CODE_OK) {
              String payload = http.getString();
              // Update cache
              cachedBSSID = bssid;
              cachedPayload = payload;
              request->send(200, "application/json", payload);
          } else {
              // On error, try to use cached data if available
              if (cachedPayload.length() > 0) {
                  dbgln("[BSSID] API request failed, using cached data");
                  request->send(200, "application/json", cachedPayload);
              } else {
                  request->send(500, "application/json", "{\"error\":\"API request failed\"}");
              }
          }
          http.end();
      } else {
          // On connection failure, try to use cached data if available
          if (cachedPayload.length() > 0) {
              dbgln("[BSSID] HTTP connection failed, using cached data");
              request->send(200, "application/json", cachedPayload);
          } else {
              request->send(500, "application/json", "{\"error\":\"HTTP connection failed\"}");
          }
      }
  });
  server->on("/", HTTP_GET, [config](AsyncWebServerRequest *request){
    logHeapMemory("/");

    // Prepare to send an HTML response stream
    AsyncResponseStream *response = request->beginResponseStream("text/html");

    // Send the header with the hostname and title
    String hostname = WiFi.getHostname();  // Fetch the hostname
    sendResponseHeader(response, "Main Menu", false, hostname);

    sendButton(response, "Status", "status");
    sendButton(response, "Config", "config");
    sendButton(response, "Debug", "debug");
    sendButton(response, "Log", "log");
    sendButton(response, "Firmware update", "update");
    sendButton(response, "WiFi reset", "wifi", "r");
    sendButton(response, "Reboot", "reboot", "r");
    sendResponseTrailer(response);
    request->send(response);
  });

  server->on("/status", HTTP_GET, [modbusCache](AsyncWebServerRequest *request) {
    logHeapMemory("/status");
    
    // Yield to prevent watchdog timeout
    yield();
    
    // Check if we can accept more connections
    if (!canAcceptConnection()) {
      request->send(503, "text/plain", "Server busy, try again later");
      return;
    }

    // Prepare to send an HTML response stream
    AsyncResponseStream *response = request->beginResponseStream("text/html");

    // Send the header with the hostname and title
    String hostname = WiFi.getHostname();  // Fetch the hostname
    sendResponseHeader(response, "Status", false, hostname);

    // Yield to prevent watchdog timeout
    yield();
    
    // Send the content of the status page
    response->print(
      R"rawliteral(
      <script>
        function fetchData() {
          fetch('/status.json')
            .then(response => response.json())
            .then(data => {
                // Create two-column layout
                const contentDiv = document.getElementById('content');
                
                // Clear existing content
                const oldTable = document.getElementById('statusTable');
                const oldLeftPanel = document.getElementById('leftPanel');
                const oldRightPanel = document.getElementById('rightPanel');
                
                if (oldTable) oldTable.remove();
                if (oldLeftPanel) oldLeftPanel.remove();
                if (oldRightPanel) oldRightPanel.remove();
                
                // Create the two-column container
                const container = document.createElement('div');
                container.style.display = 'flex';
                container.style.flexWrap = 'wrap';
                container.style.gap = '20px';
                container.style.width = '100%';
                
                // Create left panel for single-value metrics
                const leftPanel = document.createElement('div');
                leftPanel.id = 'leftPanel';
                leftPanel.style.flex = '1';
                leftPanel.style.minWidth = '300px';
                
                // Create table for left panel
                const leftTable = document.createElement('table');
                leftTable.style.width = '100%';
                leftTable.style.borderCollapse = 'collapse';
                
                // Create right panel for metrics with Value/Low/High
                const rightPanel = document.createElement('div');
                rightPanel.id = 'rightPanel';
                rightPanel.style.flex = '1';
                rightPanel.style.minWidth = '300px';
                
                // Create table for right panel
                const rightTable = document.createElement('table');
                rightTable.style.width = '100%';
                rightTable.style.borderCollapse = 'collapse';
                
                // Add header row to right table
                const rightHeader = rightTable.createTHead();
                const rightHeaderRow = rightHeader.insertRow(0);
                const rightHeaders = ["Name", "Value", "Low", "High"];
                rightHeaders.forEach(headerText => {
                    const cell = rightHeaderRow.insertCell(-1);
                    cell.textContent = headerText;
                    cell.style.fontWeight = 'bold';
                    cell.style.padding = '5px';
                    cell.style.textAlign = 'left';
                    cell.style.borderBottom = '1px solid #ddd';
                });
                
                // Create table bodies
                const leftTbody = leftTable.createTBody();
                const rightTbody = rightTable.createTBody();
                
                // Process data and add to appropriate tables
                data.data.forEach(item => {
                    // Check if item has low and high values that are not empty
                    const hasLowHigh = item.low !== undefined && item.high !== undefined && 
                                       item.low !== "" && item.high !== "";
                    
                    if (hasLowHigh) {
                        // Add to right panel (Value/Low/High metrics)
                        const row = rightTbody.insertRow(-1);
                        const cellName = row.insertCell(0);
                        const cellValue = row.insertCell(1);
                        const cellLow = row.insertCell(2);
                        const cellHigh = row.insertCell(3);
                        
                        cellName.textContent = item.name;
                        cellValue.textContent = item.value;
                        cellLow.textContent = item.low;
                        cellHigh.textContent = item.high;
                        
                        // Apply styling
                        [cellName, cellValue, cellLow, cellHigh].forEach(cell => {
                            cell.style.padding = '5px';
                            cell.style.borderBottom = '1px solid #eee';
                        });
                        
                        // Check if the current row is for the BSSID
                        if (item.name === "ESP BSSID") {
                            appendBssidCompany(cellValue, item.value);
                        }
                    } else {
                        // Add to left panel (single-value metrics)
                        const row = leftTbody.insertRow(-1);
                        const cellName = row.insertCell(0);
                        const cellValue = row.insertCell(1);
                        
                        cellName.textContent = item.name + ":";
                        cellValue.textContent = item.value;
                        
                        // Apply styling
                        cellName.style.padding = '5px';
                        cellName.style.fontWeight = 'bold';
                        cellValue.style.padding = '5px';
                        row.style.borderBottom = '1px solid #eee';
                        
                        // Check if the current row is for the BSSID
                        if (item.name === "ESP BSSID") {
                            appendBssidCompany(cellValue, item.value);
                        }
                    }
                });
                
                // Add tables to panels
                leftPanel.appendChild(leftTable);
                rightPanel.appendChild(rightTable);
                
                // Add panels to container
                container.appendChild(leftPanel);
                container.appendChild(rightPanel);
                
                // Add container to content div
                contentDiv.insertBefore(container, contentDiv.firstChild);
            })
            .catch(error => console.error('Error:', error));
        }

        function appendBssidCompany(cell, bssid) {
          // Check if the BSSID is already cached
          const cachedData = localStorage.getItem(`bssid-${bssid}`);
          if (cachedData) {
              const company = JSON.parse(cachedData).company;
              cell.textContent += ` (${company})`;
              return;
          }

          // Fetch the MAC lookup asynchronously
          fetch(`/lookup?bssid=${bssid}`)
              .then(response => response.json())
              .then(data => {
                  if (data.success && data.found) {
                      const company = data.company;
                      // Cache the result in localStorage
                      localStorage.setItem(`bssid-${bssid}`, JSON.stringify({ company }));

                      // Update the cell with the company name
                      cell.textContent += ` (${company})`;
                  } else {
                      console.warn(`No data found for BSSID: ${bssid}`);
                  }
              })
              .catch(error => console.error('MAC Lookup Error:', error));
        }

        setInterval(fetchData, 3000); // Refresh every 3 seconds
        document.addEventListener('DOMContentLoaded', fetchData); // Initial fetch
      </script>
      <div id="content">
        <p></p>
        <form method="get" action="/">
          <button class="">Back</button>
        </form>
        <p></p>
      )rawliteral"
    );

    // Send the response trailer (closing tags)
    sendResponseTrailer(response);

    // Yield before sending the response
    yield();
    
    // Send the final response
    request->send(response);
    
    // Release the connection count
    releaseConnection();
  });

  // Endpoint to serve JSON data
  server->on("/status.json", HTTP_GET, [modbusCache](AsyncWebServerRequest *request) {
    logHeapMemory("/status.json");
    
    // Yield to prevent watchdog timeout
    yield();
    
    // Check if we can accept more connections
    if (!canAcceptConnection()) {
      request->send(503, "application/json", "{\"error\":\"Server busy\"}");
      return;
    }
    
    DynamicJsonDocument doc(4096);
    JsonArray data = doc.createNestedArray("data");

    // Yield periodically during JSON generation
    yield();
    
    // Add system information as objects to the array
    auto addSystemInfo = [&data](const char* name, const String& value) {
        JsonObject obj = data.createNestedObject();
        obj["name"] = name;
        obj["value"] = value;
    };

    // Add firmware version and build information at the top
    addSystemInfo("Firmware Version", GIT_VERSION);
    addSystemInfo("Firmware Build Time", BUILD_TIME_STR);

    unsigned long uptime = millis() / 1000;
    unsigned long days = uptime / 86400;
    uptime %= 86400;
    unsigned long hours = uptime / 3600;
    uptime %= 3600;
    unsigned long minutes = uptime / 60;
    unsigned long seconds = uptime % 60;
    char uptimeStr[50];
    sprintf(uptimeStr, "%lu days, %02lu:%02lu:%02lu", days, hours, minutes, seconds);

    addSystemInfo("ESP Uptime", uptimeStr);
    
    // Calculate WiFi uptime (time since last connection)
    if (lastWiFiConnectionTime > 0 && WiFi.status() == WL_CONNECTED) {
        unsigned long wifiUptime = (millis() - lastWiFiConnectionTime) / 1000;
        unsigned long wifiDays = wifiUptime / 86400;
        wifiUptime %= 86400;
        unsigned long wifiHours = wifiUptime / 3600;
        wifiUptime %= 3600;
        unsigned long wifiMinutes = wifiUptime / 60;
        unsigned long wifiSeconds = wifiUptime % 60;
        char wifiUptimeStr[50];
        sprintf(wifiUptimeStr, "%lu days, %02lu:%02lu:%02lu", wifiDays, wifiHours, wifiMinutes, wifiSeconds);
        addSystemInfo("WiFi Uptime", wifiUptimeStr);
    } else {
        addSystemInfo("WiFi Uptime", "Not connected");
    }
    
    addSystemInfo("ESP SSID", WiFi.SSID());
    addSystemInfo("ESP RSSI", String(WiFi.RSSI()));
    addSystemInfo("ESP WiFi Quality", String(WiFiQuality(WiFi.RSSI())));
    addSystemInfo("ESP MAC", WiFi.macAddress());
    addSystemInfo("ESP IP", WiFi.localIP().toString());
    addSystemInfo("ESP Subnet Mask", WiFi.subnetMask().toString());
    addSystemInfo("ESP Gateway", WiFi.gatewayIP().toString());
    addSystemInfo("ESP BSSID", WiFi.BSSIDstr());
    ModbusClientRTU* rtu = modbusCache->getModbusRTUClient();
    addSystemInfo("Primary RTU Messages", String(rtu->getMessageCount()));
    addSystemInfo("Primary RTU Pending Messages", String(rtu->pendingRequests()));
    
    // Add Modbus information as objects to the array
    ModbusClientTCPasync* modbusTCPClient = modbusCache->getModbusTCPClient();
    
    addSystemInfo("Secondary TCP Messages", String(modbusTCPClient->getMessageCount()));
    addSystemInfo("Secondary TCP Errors", String(modbusTCPClient->getErrorCount()));

    ModbusServerRTU& modbusRTUServer = modbusCache->getModbusRTUServer();
    addSystemInfo("Server Message", String(modbusRTUServer.getMessageCount()));
    addSystemInfo("Server Errors", String(modbusRTUServer.getErrorCount()));
    addSystemInfo("Server - Static Registers Fetched", modbusCache->getStaticRegistersFetched() ? "Yes" : "No");
    addSystemInfo("Server - Dynamic Registers Fetched", modbusCache->getDynamicRegistersFetched() ? "Yes" : "No");
    addSystemInfo("Server - Operational", modbusCache->getIsOperational() ? "Yes" : "No");
    
    // Get dynamic register addresses
    std::set<uint16_t> dynamicAddresses = modbusCache->getDynamicRegisterAddresses();
    
    // Fetch all system data in a single atomic operation including registers, unexpected registers, and insane counter
    auto systemSnapshot = modbusCache->fetchSystemSnapshot(dynamicAddresses);
    
    // Use the baud rate from the system snapshot
    addSystemInfo("ET112 BAUD Rate", systemSnapshot.cgBaudRate);

    // Add dynamic registers with low and high watermarks
    for (const auto& [address, snapshot] : systemSnapshot.registers) {
        if (snapshot.definition.has_value()) {
            JsonObject obj = data.createNestedObject();
            obj["name"] = snapshot.definition->description;
            obj["value"] = snapshot.formattedValue;
            obj["low"] = snapshot.waterMarks.second;  // Low watermark
            obj["high"] = snapshot.waterMarks.first;  // High watermark
        }
    }
    
    // Show insaneCounter from snapshot
    addSystemInfo("Bogus Register Count", String(systemSnapshot.insaneCounter));

    // Add unexpected registers as a single entry from snapshot
    String unexpectedRegisters;
    for (auto& address : systemSnapshot.unexpectedRegisters) {
        unexpectedRegisters += String(address) + ", ";
    }
    if (!unexpectedRegisters.isEmpty()) {
        unexpectedRegisters.remove(unexpectedRegisters.length() - 2); // Remove the trailing comma and space
        addSystemInfo("Unexpected Registers", unexpectedRegisters);
    }

    // Add Modbus statistics
    addSystemInfo("Modbus Min Latency", String(modbusCache->getMinLatency()) + " ms");
    addSystemInfo("Modbus Max Latency", String(modbusCache->getMaxLatency()) + " ms");
    addSystemInfo("Modbus Avg Latency", String(modbusCache->getAverageLatency(), 2) + " ms");
    addSystemInfo("Modbus Latency StdDev", String(modbusCache->getStdDeviation(), 2) + " ms");
    
    // Add mutex statistics
    addSystemInfo("Mutex Acquisition Attempts", String(modbusCache->getMutexAcquisitionAttempts()));
    addSystemInfo("Mutex Acquisition Failures", String(modbusCache->getMutexAcquisitionFailures()));
    addSystemInfo("Mutex Avg Wait Time", String(modbusCache->getAverageMutexWaitTime(), 2) + " ms");
    addSystemInfo("Mutex Avg Hold Time", String(modbusCache->getAverageMutexHoldTime(), 2) + " ms");
    addSystemInfo("Mutex Max Hold Time", String(modbusCache->getMaxMutexHoldTime()) + " ms");

    // Yield again before serializing JSON
    yield();

    // Serialize the JSON document and send the response
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
    
    // Release the connection count
    releaseConnection();
  });

  server->on("/baudrate", HTTP_GET, [config,modbusCache](AsyncWebServerRequest *request) {
    logHeapMemory("/baudrate");
    auto *response = request->beginResponseStream("text/html");
    const String &hostname = config->getHostname();

    response->print("<p class=\"w\" style=\"color: red; font-weight: bold;\">"
                    "WARNING: Changing the baud rate from 9.6 kbps can make it impossible to directly address the ET112 from a CerboGX. "
                    "The CerboGX requires 9.6 kbps for direct Modbus RTU communication.<br>However, if you are using the ESP32 caching proxy, 38.4 kbps is recommended. "
                    "<br />Please proceed at your own risk.<br /> You will need to manually change the RTU Client bps rate in the \"Config\" section "
                    "after making this change.</p>");

    sendResponseHeader(response, "Set Baud Rate", true, hostname);

    response->print("<p class=\"e\">Select a new baud rate:</p>");
    response->print("<form method=\"post\">"
                    "<label><input type=\"radio\" name=\"baudrate\" value=\"1\"> 9.6 kbps</label><br>"
                    "<label><input type=\"radio\" name=\"baudrate\" value=\"2\"> 19.2 kbps</label><br>"
                    "<label><input type=\"radio\" name=\"baudrate\" value=\"3\"> 38.4 kbps</label><br>"
                    "<label><input type=\"radio\" name=\"baudrate\" value=\"4\"> 57.6 kbps</label><br>"
                    "<label><input type=\"radio\" name=\"baudrate\" value=\"5\"> 115.2 kbps</label><br><br>"
                    "<button type=\"submit\" class=\"g\">Set Baud Rate</button>"
                    "</form>"
                    "<hr/>");
    sendButton(response, "Back", "/");
    sendResponseTrailer(response);
    request->send(response);
  });

  server->on("/baudrate", HTTP_POST, [modbusCache](AsyncWebServerRequest *request) {
    dbgln("[webserver] POST /baudrate");

    if (!request->hasParam("baudrate", true)) {
        dbgln("[webserver] Missing baudrate parameter");
        request->send(400, "text/plain", "Missing baudrate parameter");
        return;
    }

    // Retrieve the selected baud rate
    String baudRateParam = request->getParam("baudrate", true)->value();
    uint16_t baudRateValue = baudRateParam.toInt();

    if (baudRateValue < 1 || baudRateValue > 5) {
        dbgln("[webserver] Invalid baudrate value");
        request->send(400, "text/plain", "Invalid baudrate value");
        return;
    }

    // Set the baud rate using the ModbusCache function
    modbusCache->setCGBaudRate(baudRateValue);
    dbgln("[webserver] Baud rate set to " + String(baudRateValue));

    // Redirect back to the GET page
    request->redirect("/baudrate");
  });

  server->on("/reboot", HTTP_GET, [config](AsyncWebServerRequest *request){
    logHeapMemory("/reboot");
    const String &hostname = config->getHostname();
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Really?", false, hostname);
    sendButton(response, "Back", "/");
    response->print("<form method=\"post\">"
        "<button class=\"r\">Yes, do it!</button>"
      "</form>");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
    dbgln("[webserver] POST /reboot");
    request->redirect("/");
    dbgln("[webserver] rebooting...")
    ESP.restart();
    dbgln("[webserver] rebooted...")
  });
  server->on("/config", HTTP_GET, [config](AsyncWebServerRequest *request){
    logHeapMemory("/config");
    const String &hostname = config->getHostname();
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Configuration", false, hostname);
    response->print("<form method=\"post\">");
    response->printf("<table>"
                      "<tr>"
                        "<td>"
                          "<label for=\"hostname\">Hostname</label>"
                        "</td>"
                        "<td>"
                          "<input type=\"text\" id=\"hostname\" name=\"hostname\" value=\"%s\"><br/>", config->getHostname().c_str());
    response->print("</td></tr>");
    response->printf("<tr>"
                      "<td>"
                        "<label for=\"pi\">Modbus Client Polling Interval (ms)&nbsp;</label>"
                      "</td>"
                      "<td>"
                        "<input type=\"number\" min=\"0\" id=\"pi\" name=\"pi\" value=\"%lu\">", config->getPollingInterval());
    response->print("</td></tr>");
    // Checkbox for Modbus Client is RTU
    response->printf("<tr>"
                      "<td>"
                        "<label for=\"clientIsRTU\">Modbus Client is RTU</label>"
                      "</td>"
                      "<td>"
                        "<input type=\"checkbox\" id=\"clientIsRTU\" name=\"clientIsRTU\" %s><br/>", config->getClientIsRTU() ? "checked" : "");
    response->print("</td></tr></table>");
    // Modbus Primary RTU section
    response->print("<div id=\"rtuSettings\" style=\"display:none;\">"
      "<h3>Modbus RTU Client</h3>"
      "<table>");
    response->print(
        "<tr>"
          "<td>"
            "<label for=\"mb\">Baud rate</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"0\" id=\"mb\" name=\"mb\" value=\"%lu\">", config->getModbusBaudRate());
    response->print("</td>"
        "</tr>");
    response->printf("<tr>"
          "<td>"
            "<label for=\"md\">Data bits</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"5\" max=\"8\" id=\"md\" name=\"md\" value=\"%d\">", config->getModbusDataBits());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"mp\">Parity</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"mp\" name=\"mp\" data-value=\"%d\">", config->getModbusParity());
    response->print("<option value=\"0\">None</option>"
              "<option value=\"2\">Even</option>"
              "<option value=\"3\">Odd</option>"
            "</select>"
          "</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"ms\">Stop bits</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"ms\" name=\"ms\" data-value=\"%d\">", config->getModbusStopBits());
    response->print("<option value=\"1\">1 bit</option>"
              "<option value=\"2\">1.5 bits</option>"
              "<option value=\"3\">2 bits</option>"
            "</select>"
          "</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"mr\">RTS Pin</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"mr\" name=\"mr\" data-value=\"%d\">", config->getModbusRtsPin());
    response->print("<option value=\"-1\">Auto</option>"
              "<option value=\"4\">D4</option>"
              "<option value=\"13\">D13</option>"
              "<option value=\"14\">D14</option>"
              "<option value=\"18\">D18</option>"
              "<option value=\"19\">D19</option>"
              "<option value=\"21\">D21</option>"
              "<option value=\"22\">D22</option>"
              "<option value=\"23\">D23</option>"
              "<option value=\"25\">D25</option>"
              "<option value=\"26\">D26</option>"
              "<option value=\"27\">D27</option>"
              "<option value=\"32\">D32</option>"
              "<option value=\"33\">D33</option>"
            "</select>"
          "</td>"
        "</tr>"
      "</table>"
    "</div>");
    // TCP Settings section
    response->print("<div id=\"tcpSettings\" style=\"display:none;\">"
        "<h3>Modbus TCP Client Settings</h3>"
        "<table>"
        "<tr>"
          "<td>"
            "<label for=\"sip\">Server IP</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"text\" id=\"sip\" name=\"sip\" value=\"%s\">", config->getTargetIP().c_str());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"tp2\">Server Port</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"1\" max=\"65535\" id=\"tp2\" name=\"tp2\" value=\"%d\">", config->getTcpPort2());

    response->print("</td>"
        "</tr>"
        "</table>"
        "</div>"
        "<h3>Modbus Secondary RTU (server/slave)</h3>"
        "<table>"
        "<tr>"
          "<td>"
            "<label for=\"mb2\">Baud rate</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"0\" id=\"mb2\" name=\"mb2\" value=\"%lu\">", config->getModbusBaudRate2());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"md2\">Data bits</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"5\" max=\"8\" id=\"md2\" name=\"md2\" value=\"%d\">", config->getModbusDataBits2());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"mp2\">Parity</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"mp2\" name=\"mp2\" data-value=\"%d\">", config->getModbusParity2());
    response->print("<option value=\"0\">None</option>"
              "<option value=\"2\">Even</option>"
              "<option value=\"3\">Odd</option>"
            "</select>"
          "</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"ms2\">Stop bits</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"ms2\" name=\"ms2\" data-value=\"%d\">", config->getModbusStopBits2());
    response->print("<option value=\"1\">1 bit</option>"
              "<option value=\"2\">1.5 bits</option>"
              "<option value=\"3\">2 bits</option>"
            "</select>"
          "</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"mr2\">RTS Pin</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"mr2\" name=\"mr2\" data-value=\"%d\">", config->getModbusRtsPin2());
    response->print("<option value=\"-1\">Auto</option>"
              "<option value=\"4\">D4</option>"
              "<option value=\"13\">D13</option>"
              "<option value=\"14\">D14</option>"
              "<option value=\"18\">D18</option>"
              "<option value=\"19\">D19</option>"
              "<option value=\"21\">D21</option>"
              "<option value=\"22\">D22</option>"
              "<option value=\"23\">D23</option>"
              "<option value=\"25\">D25</option>"
              "<option value=\"26\">D26</option>"
              "<option value=\"27\">D27</option>"
              "<option value=\"32\">D32</option>"
              "<option value=\"33\">D33</option>"
            "</select>"
          "</td>"
        "</tr>"
        "</table>"
        "<h3>Modbus Secondary TCP Server</h3>"
        "<table>"
        "<tr>"
          "<td>"
            "<label for=\"mb2\">Port Number</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"1\" max=\"65535\" id=\"tp3\" name=\"tp3\" value=\"%d\">", config->getTcpPort3());
    response->print("</td>"
        "</tr>"
        "</table>"
        "<hr>"
        "<h3>Serial (Debug)</h3>"
        "<table>"
        "<tr>"
          "<td>"
            "<label for=\"sb\">Baud rate</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"0\" id=\"sb\" name=\"sb\" value=\"%lu\">", config->getSerialBaudRate());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"sd\">Data bits</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"5\" max=\"8\" id=\"sd\" name=\"sd\" value=\"%d\">", config->getSerialDataBits());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"sp\">Parity</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"sp\" name=\"sp\" data-value=\"%d\">", config->getSerialParity());
    response->print("<option value=\"0\">None</option>"
              "<option value=\"2\">Even</option>"
              "<option value=\"3\">Odd</option>"
            "</select>"
          "</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"ss\">Stop bits</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"ss\" name=\"ss\" data-value=\"%d\">", config->getSerialStopBits());
    response->print("<option value=\"1\">1 bit</option>"
              "<option value=\"2\">1.5 bits</option>"
              "<option value=\"3\">2 bits</option>"
            "</select>"
          "</td>"
          "</tr>");
    response->printf("</table>");
    response->print("<h3>Network Settings</h3>"
      "<table>"
      "<tr>"
        "<td>"
          "<label for=\"useStaticIP\">Use Static IP</label>"
        "</td>"
        "<td>");
    response->printf("<input type=\"checkbox\" id=\"useStaticIP\" name=\"useStaticIP\" %s>", config->getUseStaticIP() ? "checked" : "");
    response->print("</td>"
      "</tr>"
      "<tr>"
        "<td>"
          "<label for=\"staticIP\">Static IP Address</label>"
        "</td>"
        "<td>");
    response->printf("<input type=\"text\" id=\"staticIP\" name=\"staticIP\" value=\"%s\" pattern=\"^(?:[0-9]{1,3}\\.){3}[0-9]{1,3}$\">", config->getStaticIP().c_str());
    response->print("</td>"
      "</tr>"
      "<tr>"
        "<td>"
          "<label for=\"staticGateway\">Gateway IP</label>"
        "</td>"
        "<td>");
    response->printf("<input type=\"text\" id=\"staticGateway\" name=\"staticGateway\" value=\"%s\" pattern=\"^(?:[0-9]{1,3}\\.){3}[0-9]{1,3}$\">", config->getStaticGateway().c_str());
    response->print("</td>"
      "</tr>"
      "<tr>"
        "<td>"
          "<label for=\"staticSubnet\">Subnet Mask</label>"
        "</td>"
        "<td>");
    response->printf("<input type=\"text\" id=\"staticSubnet\" name=\"staticSubnet\" value=\"%s\" pattern=\"^(?:[0-9]{1,3}\\.){3}[0-9]{1,3}$\">", config->getStaticSubnet().c_str());
    response->print("</td>"
      "</tr>"
      "</table>");
    response->print("<button class=\"r\">Save</button>"
      "</form>"
      "<p></p>");
    sendButton(response, "Back", "/");
    response->print("<script>"
      "(function(){"
        "var s = document.querySelectorAll('select[data-value]');"
        "for(d of s){"
          "d.querySelector(`option[value='${d.dataset.value}']`).selected=true"
      "}})();"
      "document.addEventListener('DOMContentLoaded', function() {"
      "  var checkbox = document.getElementById('clientIsRTU');"
      "  var showRTU = function() {"
      "    document.getElementById('rtuSettings').style.display = checkbox.checked ? 'block' : 'none';"
      "    document.getElementById('tcpSettings').style.display = checkbox.checked ? 'none' : 'block';"
      "  };"
      "  checkbox.addEventListener('change', showRTU);"
      "  showRTU();"
      "  var staticIPCheckbox = document.getElementById('useStaticIP');"
      "  var staticIPFields = ['staticIP', 'staticGateway', 'staticSubnet'];"
      "  var toggleStaticFields = function() {"
      "    staticIPFields.forEach(function(field) {"
      "      document.getElementById(field).disabled = !staticIPCheckbox.checked;"
      "    });"
      "  };"
      "  staticIPCheckbox.addEventListener('change', toggleStaticFields);"
      "  toggleStaticFields();"
      "});"
      "</script>");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/config", HTTP_POST, [config](AsyncWebServerRequest *request){
    dbgln("[webserver] POST /config");
    bool validIP = true;
    if (request->hasParam("hostname", true)) {
        String hostname = request->getParam("hostname", true)->value();
        config->setHostname(hostname);  // Save the hostname in preferences
        dbgln("[webserver] saved hostname");
    }
    if (request->hasParam("tp", true)){
      auto port = request->getParam("tp", true)->value().toInt();
      config->setTcpPort(port);
      dbgln("[webserver] saved port");
    }
    if (request->hasParam("tp2", true)){
      auto port = request->getParam("tp2", true)->value().toInt();
      config->setTcpPort2(port);
      dbgln("[webserver] saved port2");
    }
    if (request->hasParam("tp3", true)){
      auto port = request->getParam("tp3", true)->value().toInt();
      config->setTcpPort3(port);
      dbgln("[webserver] saved port3");
    }
    if (request->hasParam("sip", true)){
      String targetIP = request->getParam("sip", true)->value();
        IPAddress ip;
        if (ip.fromString(targetIP)) {
            config->setTargetIP(targetIP);
            dbgln("[webserver] saved target IP");
        } else {
            dbgln("[webserver] invalid target IP");
            validIP = false;
            // Optionally: Add code to handle invalid IP address
        }
    }
    if (request->hasParam("tt", true)){
      auto timeout = request->getParam("tt", true)->value().toInt();
      config->setTcpTimeout(timeout);
      dbgln("[webserver] saved timeout");
    }
    if (request->hasParam("mb", true)){
      auto baud = request->getParam("mb", true)->value().toInt();
      config->setModbusBaudRate(baud);
      dbgln("[webserver] saved modbus baud rate");
    }
    if (request->hasParam("md", true)){
      auto data = request->getParam("md", true)->value().toInt();
      config->setModbusDataBits(data);
      dbgln("[webserver] saved modbus data bits");
    }
    if (request->hasParam("mp", true)){
      auto parity = request->getParam("mp", true)->value().toInt();
      config->setModbusParity(parity);
      dbgln("[webserver] saved modbus parity");
    }
    if (request->hasParam("ms", true)){
      auto stop = request->getParam("ms", true)->value().toInt();
      config->setModbusStopBits(stop);
      dbgln("[webserver] saved modbus stop bits");
    }
    if (request->hasParam("mr", true)){
      auto rts = request->getParam("mr", true)->value().toInt();
      config->setModbusRtsPin(rts);
      dbgln("[webserver] saved modbus rts pin");
    }

    if (request->hasParam("mb2", true)){
      auto baud = request->getParam("mb2", true)->value().toInt();
      config->setModbusBaudRate2(baud);
      dbgln("[webserver] saved modbus baud rate 2");
    }
    if (request->hasParam("md2", true)){
      auto data = request->getParam("md2", true)->value().toInt();
      config->setModbusDataBits2(data);
      dbgln("[webserver] saved modbus data bits 2");
    }
    if (request->hasParam("mp2", true)){
      auto parity = request->getParam("mp2", true)->value().toInt();
      config->setModbusParity2(parity);
      dbgln("[webserver] saved modbus parity 2");
    }
    if (request->hasParam("ms2", true)){
      auto stop = request->getParam("ms2", true)->value().toInt();
      config->setModbusStopBits2(stop);
      dbgln("[webserver] saved modbus stop bits 2");
    }
    if (request->hasParam("mr2", true)){
      auto rts = request->getParam("mr2", true)->value().toInt();
      config->setModbusRtsPin2(rts);
      dbgln("[webserver] saved modbus rts pin 2");
    }

    if (request->hasParam("sb", true)){
      auto baud = request->getParam("sb", true)->value().toInt();
      config->setSerialBaudRate(baud);
      dbgln("[webserver] saved serial baud rate");
    }
    if (request->hasParam("sd", true)){
      auto data = request->getParam("sd", true)->value().toInt();
      config->setSerialDataBits(data);
      dbgln("[webserver] saved serial data bits");
    }
    if (request->hasParam("sp", true)){
      auto parity = request->getParam("sp", true)->value().toInt();
      config->setSerialParity(parity);
      dbgln("[webserver] saved serial parity");
    }
    if (request->hasParam("ss", true)){
      auto stop = request->getParam("ss", true)->value().toInt();
      config->setSerialStopBits(stop);
      dbgln("[webserver] saved serial stop bits");
    }
    if (request->hasParam("pi", true)){
      auto pollingInterval = request->getParam("pi", true)->value().toInt();
      config->setPollingInterval(pollingInterval);
      dbgln("[webserver] saved polling interval");
    }
    // Redirect logic with error handling
    if (validIP) {
        // Redirect to the config page or a success page
        request->redirect("/");
    } else {
        // Redirect back to the form with an error message
        // This can be implemented in different ways, for example:
        request->redirect("/config?error=invalidIP");
        // Then, on the GET handler for "/config", check for this error parameter and display a message if present
    }
    // Handling new checkbox input for Modbus Client is RTU
    if (request->hasParam("clientIsRTU", true)){
      // If the parameter exists, the checkbox was checked
      config->setClientIsRTU(true);
      dbgln("[webserver] Modbus Client is RTU: true");
    } else {
      // Otherwise, it was not checked
      config->setClientIsRTU(false);
      dbgln("[webserver] Modbus Client is RTU: false");
    }
    if (request->hasParam("useStaticIP", true)) {
        config->setUseStaticIP(true);
        dbgln("[webserver] saved useStaticIP");
    } else {
        config->setUseStaticIP(false);
        dbgln("[webserver] cleared useStaticIP");
    }
    if (request->hasParam("staticIP", true)) {
        String staticIP = request->getParam("staticIP", true)->value();
        IPAddress ip;
        if (ip.fromString(staticIP)) {
            config->setStaticIP(staticIP);
            dbgln("[webserver] saved static IP");
        } else {
            dbgln("[webserver] invalid static IP");
            validIP = false;
        }
    }
    if (request->hasParam("staticGateway", true)) {
        String gateway = request->getParam("staticGateway", true)->value();
        IPAddress ip;
        if (ip.fromString(gateway)) {
            config->setStaticGateway(gateway);
            dbgln("[webserver] saved gateway IP");
        } else {
            dbgln("[webserver] invalid gateway IP");
            validIP = false;
        }
    }
    if (request->hasParam("staticSubnet", true)) {
        String subnet = request->getParam("staticSubnet", true)->value();
        IPAddress ip;
        if (ip.fromString(subnet)) {
            config->setStaticSubnet(subnet);
            dbgln("[webserver] saved subnet mask");
        } else {
            dbgln("[webserver] invalid subnet mask");
            validIP = false;
        }
    }
  });
  server->on("/debug", HTTP_GET, [config](AsyncWebServerRequest *request){
    logHeapMemory("/debug");
    const String &hostname = config->getHostname();
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Debug RTU Client", false, hostname);
    sendDebugForm(response, "1", "1", "3", "1");
    sendButton(response, "Back", "/");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/debug", HTTP_POST, [modbusCache, config](AsyncWebServerRequest *request){
    dbgln("[webserver] POST /debug");
    const String &hostname = config->getHostname();

    ModbusClientRTU* rtu = modbusCache->getModbusRTUClient();
    String slaveId = "1";
    if (request->hasParam("slave", true)){
      slaveId = request->getParam("slave", true)->value();
    }
    String reg = "1";
    if (request->hasParam("reg", true)){
      reg = request->getParam("reg", true)->value();
    }
    String func = "3";
    if (request->hasParam("func", true)){
      func = request->getParam("func", true)->value();
    }
    String count = "1";
    if (request->hasParam("count", true)){
      count = request->getParam("count", true)->value();
    }
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Debug", false, hostname);
    response->print("<pre>");
    auto previous = LOGDEVICE;
    auto previousLevel = MBUlogLvl;
    auto debug = WebPrint(previous, response);
    LOGDEVICE = &debug;
    MBUlogLvl = LOG_LEVEL_DEBUG;
    ModbusMessage answer = rtu->syncRequest(0xdeadbeef, slaveId.toInt(), func.toInt(), reg.toInt(), count.toInt());
    MBUlogLvl = previousLevel;
    LOGDEVICE = previous;
    response->print("</pre>");
    auto error = answer.getError();
    if (error == SUCCESS){
      auto count = answer[2];
      response->print("<span >Answer: 0x");
      for (size_t i = 0; i < count; i++)
      {
        response->printf("%02x", answer[i + 3]);
      }      
      response->print("</span>");
    }
    else{
      response->printf("<span class=\"e\">Error: %#02x (%s)</span>", error, ErrorName(error).c_str());
    }
    sendDebugForm(response, slaveId, reg, func, count);
    sendButton(response, "Back", "/");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/update", HTTP_GET, [config](AsyncWebServerRequest *request){
    logHeapMemory("/update");
    const String &hostname = config->getHostname();
    auto *response = request->beginResponseStream("text/html");
    sendResponseHeader(response, "Firmware Update", false, hostname);
    response->print("<form method=\"post\" enctype=\"multipart/form-data\">"
      "<input type=\"file\" name=\"file\" id=\"file\" required/>"
      "<p></p>"
      "<button class=\"r\">Upload</button>"
      "</form>"
      "<p></p>");
    sendButton(response, "Back", "/");
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/update", HTTP_POST, [config](AsyncWebServerRequest *request){
    String hostname = config->getHostname();
    request->onDisconnect([](){
      ESP.restart();
    });
    dbgln("[webserver] OTA finished");
    if (Update.hasError()){
      auto *response = request->beginResponse(500, "text/plain", "Ota failed");
      response->addHeader("Connection", "close");
      request->send(response);
    }
    else{
      auto *response = request->beginResponseStream("text/html");
      response->addHeader("Connection", "close");
      sendResponseHeader(response, "Firmware Update", true, hostname);
      response->print("<p>Update successful.</p>");
      sendButton(response, "Back", "/");
      sendResponseTrailer(response);
      request->send(response);
    }
  }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    dbg("[webserver] OTA progress ");dbgln(index);
    if (!index) {
      //TODO add MD5 Checksum and Update.setMD5
      int cmd = (filename == "filesystem") ? U_SPIFFS : U_FLASH;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) { // Start with max available size
        Update.printError(Serial);
        return request->send(400, "text/plain", "OTA could not begin");
      }
    }
    // Write chunked data to the free sketch space
    if(len){
      if (Update.write(data, len) != len) {
        return request->send(400, "text/plain", "OTA could not write data");
      }
    }
    if (final) { // if the final flag is set then this is the last frame of data
      if (!Update.end(true)) { //true to set the size to the current progress
        Update.printError(Serial);
        return request->send(400, "text/plain", "Could not end OTA");
      }
    }else{
      return;
    }
  });
  server->on("/wifi", HTTP_GET, [config](AsyncWebServerRequest *request){
    logHeapMemory("/wifi");
    auto *response = request->beginResponseStream("text/html");
    const String &hostname = config->getHostname();

    sendResponseHeader(response, "WiFi reset", true, hostname);
    response->print("<p class=\"e\">"
        "This will delete the stored WiFi config<br/>"
        "and restart the ESP in AP mode.<br/> Are you sure?"
      "</p>");
    sendButton(response, "Back", "/");
    response->print("<p></p>"
      "<form method=\"post\">"
        "<button class=\"r\">Yes, do it!</button>"
      "</form>");    
    sendResponseTrailer(response);
    request->send(response);
  });
  server->on("/wifi", HTTP_POST, [wm](AsyncWebServerRequest *request){
    dbgln("[webserver] POST /wifi");
    request->redirect("/");
    wm->erase();
    dbgln("[webserver] erased wifi config");
    dbgln("[webserver] rebooting...");
    ESP.restart();
    dbgln("[webserver] rebooted...");
  });
  server->on("/favicon.ico", [](AsyncWebServerRequest *request){
    logHeapMemory("/favicon.ico");
    request->send(204);//TODO add favicon
  });
  server->on("/style.css", [](AsyncWebServerRequest *request){
    logHeapMemory("/style.css");
    if (request->hasHeader("If-None-Match")){
      auto header = request->getHeader("If-None-Match");
      if (header->value() == String(ETAG)){
        request->send(304);
        return;
      }
    }
    dbgln("[webserver] GET /style.css");
    auto *response = request->beginResponseStream("text/css");
    sendMinCss(response);
    response->print(
    "button.r{"
	    "background: #d43535;"
    "}"
    "button.r:hover{"
	    "background: #931f1f;"
    "}"
    "table{"
      "text-align:left;"
      "width:100%;"
    "}"
    "input{"
      "width:100%;"
    "}"
    ".e{"
      "color:red;"
    "}"
    "pre{"
      "text-align:left;"
    "}"
    );
    response->addHeader("ETag", ETAG);
    request->send(response);
  });
  server->on("/log", HTTP_GET, [config](AsyncWebServerRequest *request) {
    logHeapMemory("/log");
    
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    sendLogPage(response, config->getHostname());
    request->send(response);
  });
  
  // Add log data endpoint for AJAX fallback - ultra lightweight version
  server->on("/logdata", HTTP_GET, [](AsyncWebServerRequest *request) {
    logHeapMemory("/logdata");
    // Don't even log this request to avoid recursive logging
    // dbgln("[webserver] GET /logdata");
    
    // Send a simple response immediately
    AsyncResponseStream *response = request->beginResponseStream("text/plain");
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    
    // Get position parameter if it exists
    size_t position = 0;
    if (request->hasParam("position")) {
        position = request->getParam("position")->value().toInt();
    }
    
    // Get chunk_size parameter if it exists
    size_t maxChars = 8192; // Default to 8KB
    if (request->hasParam("chunk_size")) {
        maxChars = request->getParam("chunk_size")->value().toInt();
        // Limit to reasonable size to prevent memory issues
        if (maxChars > 32768) {
            maxChars = 32768; // Cap at 32KB (buffer size)
        }
    }
    
    // Immediately yield to allow other tasks to run
    yield();
    
    // Get a chunk of data with the requested size
    String messages = "";
    bool hasOverflow = false;
    
    // Use a critical section to minimize mutex lock time
    {
        // Manually get data from buffer with minimal processing
        size_t newPosition = position;
        
        // Get overflow flag
        hasOverflow = debugBuffer.hasOverflowed();
        
        // Get a chunk of messages with the requested size
        messages = debugBuffer.getSafeChunk(position, maxChars, newPosition);
        
        // Update position
        position = newPosition;
    }
    
    // Write position first
    response->print(position);
    response->print("\n");
    
    // Write overflow flag
    response->print(hasOverflow ? "1" : "0");
    response->print("\n");
    
    // Write messages
    response->print(messages);
    
    // Send response
    request->send(response);
  });
  
  // Add log clear endpoint
  server->on("/logclear", HTTP_POST, [](AsyncWebServerRequest *request) {
    dbgln("[webserver] POST /logclear");
    
    // Allow other tasks to run immediately
    yield();
    
    // Clear the log buffer
    debugBuffer.clear();
    
    // Send a simple text response
    request->send(200, "text/plain", "OK");
  });

  server->onNotFound([](AsyncWebServerRequest *request){
    dbg("[webserver] request to ");dbg(request->url());dbgln(" not found");
    request->send(404, "text/plain", "404");
  });
}

void sendMinCss(AsyncResponseStream *response){
  response->print("body{"    
      "font-family:sans-serif;"
	    "text-align: center;"
      "background: #252525;"
	    "color: #faffff;"
    "}"
    "#content{"
	    "display: inline-block;"
	    "min-width: 340px;"
    "}"
    "button{"
	    "width: 100%;"
	    "line-height: 2.4rem;"
	    "background: #1fa3ec;"
	    "border: 0;"
	    "border-radius: 0.3rem;"
	    "font-size: 1.2rem;"
      "-webkit-transition-duration: 0.4s;"
      "transition-duration: 0.4s;"
	    "color: #faffff;"
    "}"
    "button:hover{"
	    "background: #0e70a4;"
    "}");
}

void sendResponseHeader(AsyncResponseStream *response, const char *title, bool inlineStyle, const String &hostname){
    // If hostname is not defined, use "ESP32 Modbus Cache"
    response->print("<!DOCTYPE html>"
      "<html lang=\"en\" class=\"\">"
      "<head>"
      "<meta charset='utf-8'>"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,user-scalable=no\"/>");
    // The title should be "hostname - title"
    response->printf("<title>%s - %s</title>", hostname.isEmpty() ? "ESP32 Modbus Cache" : hostname.c_str(), title);

    if (inlineStyle){
      response->print("<style>");
      sendMinCss(response);
      response->print("</style>");
    }
    else{
      response->print("<link rel=\"stylesheet\" href=\"style.css\">");
    }
    response->printf(
      "</head>"
      "<body>"
      "<h2>%s</h2>", hostname.isEmpty() ? "ESP32 Modbus Cache" : hostname.c_str());
    response->printf("<h3>%s</h3>", title);
    response->print("<div id=\"content\">");
}

void sendResponseTrailer(AsyncResponseStream *response){
    response->print("</div></body></html>");
}

void sendButton(AsyncResponseStream *response, const char *title, const char *action, const char *css){
    response->printf(
      "<form method=\"get\" action=\"%s\">"
        "<button class=\"%s\">%s</button>"
      "</form>"
      "<p></p>", action, css, title);
}

void sendTableRow(AsyncResponseStream *response, const char *name, String value){
    response->printf(
      "<tr>"
        "<td>%s:</td>"
        "<td>%s</td>"
      "</tr>", name, value.c_str());
}

void sendTableRow(AsyncResponseStream *response, const char *name, uint32_t value){
    response->printf(
      "<tr>"
        "<td>%s:</td>"
        "<td>%d</td>"
      "</tr>", name, value);
}

void sendDebugForm(AsyncResponseStream *response, String slaveId, String reg, String function, String count){
    response->print("<form method=\"post\">");
    response->print("<table>"
      "<tr>"
        "<td>"
          "<label for=\"slave\">Slave ID</label>"
        "</td>"
        "<td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"247\" id=\"slave\" name=\"slave\" value=\"%s\">", slaveId.c_str());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"func\">Function</label>"
          "</td>"
          "<td>");
    response->printf("<select id=\"func\" name=\"func\" data-value=\"%s\">", function.c_str());
    response->print("<option value=\"1\">01 Read Coils</option>"
              "<option value=\"2\">02 Read Discrete Inputs</option>"
              "<option value=\"3\">03 Read Holding Register</option>"
              "<option value=\"4\">04 Read Input Register</option>"
            "</select>"
          "</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"reg\">Register</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"65535\" id=\"reg\" name=\"reg\" value=\"%s\">", reg.c_str());
    response->print("</td>"
        "</tr>"
        "<tr>"
          "<td>"
            "<label for=\"count\">Count</label>"
          "</td>"
          "<td>");
    response->printf("<input type=\"number\" min=\"0\" max=\"65535\" id=\"count\" name=\"count\" value=\"%s\">", count.c_str());
    response->print("</td>"
        "</tr>"
      "</table>");
    response->print("<button class=\"r\">Send</button>"
      "</form>"
      "<p></p>");
    response->print("<script>"
      "(function(){"
        "var s = document.querySelectorAll('select[data-value]');"
        "for(d of s){"
          "d.querySelector(`option[value='${d.dataset.value}']`).selected=true"
      "}})();"
      "</script>");
}

const String ErrorName(Modbus::Error code)
{
    switch (code)
    {
        case Modbus::Error::SUCCESS: return "Success";
        case Modbus::Error::ILLEGAL_FUNCTION: return "Illegal function";
        case Modbus::Error::ILLEGAL_DATA_ADDRESS: return "Illegal data address";
        case Modbus::Error::ILLEGAL_DATA_VALUE: return "Illegal data value";
        case Modbus::Error::SERVER_DEVICE_FAILURE: return "Server device failure";
        case Modbus::Error::ACKNOWLEDGE: return "Acknowledge";
        case Modbus::Error::SERVER_DEVICE_BUSY: return "Server device busy";
        case Modbus::Error::NEGATIVE_ACKNOWLEDGE: return "Negative acknowledge";
        case Modbus::Error::MEMORY_PARITY_ERROR: return "Memory parity error";
        case Modbus::Error::GATEWAY_PATH_UNAVAIL: return "Gateway path unavailable";
        case Modbus::Error::GATEWAY_TARGET_NO_RESP: return "Gateway target no response";
        case Modbus::Error::TIMEOUT: return "Timeout";
        case Modbus::Error::INVALID_SERVER: return "Invalid server";
        case Modbus::Error::CRC_ERROR: return "CRC error";
        case Modbus::Error::FC_MISMATCH: return "Function code mismatch";
        case Modbus::Error::SERVER_ID_MISMATCH: return "Server id mismatch";
        case Modbus::Error::PACKET_LENGTH_ERROR: return "Packet length error";
        case Modbus::Error::PARAMETER_COUNT_ERROR: return "Parameter count error";
        case Modbus::Error::PARAMETER_LIMIT_ERROR: return "Parameter limit error";
        case Modbus::Error::REQUEST_QUEUE_FULL: return "Request queue full";
        case Modbus::Error::ILLEGAL_IP_OR_PORT: return "Illegal ip or port";
        case Modbus::Error::IP_CONNECTION_FAILED: return "IP connection failed";
        case Modbus::Error::TCP_HEAD_MISMATCH: return "TCP header mismatch";
        case Modbus::Error::EMPTY_MESSAGE: return "Empty message";
        case Modbus::Error::ASCII_FRAME_ERR: return "ASCII frame error";
        case Modbus::Error::ASCII_CRC_ERR: return "ASCII crc error";
        case Modbus::Error::ASCII_INVALID_CHAR: return "ASCII invalid character";
        default: return "undefined error";
    }
}

// translate RSSI to quality string
const String WiFiQuality(int rssiValue)
{
    switch (rssiValue)
    {
        case -30 ... 0: return "Amazing"; 
        case -67 ... -31: return "Very Good"; 
        case -70 ... -68: return "Okay"; 
        case -80 ... -71: return "Not Good"; 
        default: return "Unusable";
    }
}

void sendLogPage(AsyncResponseStream *response, const String &hostname) {
    // Use inline style to have more control
    sendResponseHeader(response, "Log Viewer", true, hostname);
    
    // Add custom styles for log viewer
    response->print(R"(
    <style>
        /* Override the default center alignment */
        body {
            text-align: left !important;
        }
        
        #content {
            text-align: left !important;
            display: block !important;
            width: 95% !important;
            max-width: 1200px !important;
            margin: 0 auto !important;
        }
        
        h2, h3 {
            text-align: left !important;
        }
        
        #log-container {
            background-color: #1e1e1e;
            color: #f0f0f0;
            font-family: monospace;
            padding: 10px;
            height: 600px; /* Increased height */
            overflow-y: auto;
            white-space: pre-wrap;
            word-wrap: break-word;
            border-radius: 4px;
            margin-bottom: 10px;
            text-align: left !important;
            width: 100% !important;
            font-size: 14px; /* Explicit font size */
        }
        
        .log-controls {
            margin-bottom: 10px;
            text-align: left !important;
            width: 100% !important;
        }
        
        .log-controls button {
            margin-right: 10px;
            text-align: center; /* Keep button text centered */
            width: auto !important;
            padding: 8px 16px; /* More padding for better clickability */
        }
        
        .log-timestamp {
            color: #888;
        }
        
        .log-wifi {
            color: #58a6ff;
        }
        
        .log-webserver {
            color: #7ee787;
        }
        
        .log-error {
            color: #f85149;
        }
        
        .log-modbuscache {
            color: #d2a8ff;
        }
        
        .log-config {
            color: #f0883e;
        }
        
        .log-setup {
            color: #79c0ff;
        }
        
        .autoscroll-enabled {
            background-color: #238636 !important;
        }
        
        #buffer-info {
            font-size: 12px;
            color: #888;
            margin-top: 5px;
        }
    </style>
    )");
    
    // Log container and controls
    response->print(R"(
    <h2>Log Viewer</h2>
    <div class="log-controls">
        <button id="clear-log" class="btn btn-danger">Clear Log</button>
        <button id="toggle-autoscroll" class="btn btn-primary autoscroll-enabled">Autoscroll: ON</button>
        <button id="download-log" class="btn btn-secondary">Download Log</button>
        <span id="connection-status">AJAX: Connecting...</span>
        <div id="buffer-info">Buffer size: 32KB (approx. 400-800 messages)</div>
    </div>
    <div id="log-container"></div>
    )");
    
    // JavaScript for WebSocket and log handling
    response->print(R"(
    <script>
        const logContainer = document.getElementById('log-container');
        const clearLogBtn = document.getElementById('clear-log');
        const toggleAutoscrollBtn = document.getElementById('toggle-autoscroll');
        const downloadLogBtn = document.getElementById('download-log');
        const connectionStatus = document.getElementById('connection-status');
        
        let position = 0;
        let autoscroll = true;
        let isLoadingChunk = false;
        let messageCount = 0;
        let updateCount = 0;
        let lastUpdateTime = Date.now();
        let updatesPerSecond = 0;
        
        // Chunk size for data
        const CHUNK_SIZE = 8192; // Increased from 512 to 8KB to match server-side
        
        // Start AJAX polling
        startAjaxPolling();
        
        // Start AJAX polling
        function startAjaxPolling() {
            connectionStatus.textContent = 'AJAX: Connected (Continuous Polling)';
            connectionStatus.style.color = '#238636';
            
            // Get initial data
            fetchLogUpdates();
            
            // Note: We're now using requestAnimationFrame and setTimeout for continuous polling
        }
        
        // Fetch log updates via AJAX
        function fetchLogUpdates() {
            if (isLoadingChunk) return;
            
            isLoadingChunk = true;
            fetch(`/logdata?position=${position}&chunk_size=8192`)
                .then(response => response.text())
                .then(data => {
                    // Parse the simple text format
                    const lines = data.split('\n');
                    if (lines.length >= 2) {
                        // First line is position
                        position = parseInt(lines[0], 10);
                        
                        // Second line is overflow flag
                        const hasOverflow = lines[1] === '1';
                        
                        // Rest is messages (join remaining lines)
                        const messages = lines.slice(2).join('\n');
                        
                        if (messages) {
                            // Ensure the message ends with a newline
                            const messageWithNewline = messages.endsWith('\n') ? messages : messages + '\n';
                            appendLog(messageWithNewline);
                        }
                        
                        if (hasOverflow) {
                            appendLog('[System] Log buffer overflow detected. Some messages may have been lost.\n');
                        }
                    }
                    
                    // Track update rate
                    updateCount++;
                    const now = Date.now();
                    const elapsed = now - lastUpdateTime;
                    
                    // Update the rate every second
                    if (elapsed >= 1000) {
                        updatesPerSecond = Math.round((updateCount / elapsed) * 1000);
                        document.getElementById('buffer-info').textContent = 
                            `Buffer size: 32KB (approx. 400-800 messages) - Currently showing: ~${messageCount} messages - Updates: ${updatesPerSecond}/sec`;
                        updateCount = 0;
                        lastUpdateTime = now;
                    }
                    
                    isLoadingChunk = false;
                    
                    // Almost continuous polling - use requestAnimationFrame for browser efficiency
                    // This will poll as fast as the browser can render, but will pause when tab is inactive
                    requestAnimationFrame(() => {
                        // Add a longer delay to prevent overwhelming the ESP32
                        setTimeout(fetchLogUpdates, 200);
                    });
                })
                .catch(error => {
                    console.error('Error fetching log updates:', error);
                    isLoadingChunk = false;
                    connectionStatus.textContent = 'AJAX: Error - Retrying...';
                    connectionStatus.style.color = '#f85149';
                    
                    // Retry after a short delay
                    setTimeout(fetchLogUpdates, 1000);
                });
        }
        
        // Append log messages to the container
        function appendLog(messages) {
            if (!messages) return;
            
            // Count approximate number of messages (by counting newlines)
            const newLines = (messages.match(/\n/g) || []).length;
            messageCount += newLines + 1;
            
            // Process and colorize the log messages
            const colorizedMessages = colorizeLog(messages);
            logContainer.innerHTML += colorizedMessages;
            
            // Limit the DOM size to prevent browser slowdown
            if (logContainer.innerHTML.length > 500000) {
                // Keep only the last 400K characters
                logContainer.innerHTML = logContainer.innerHTML.slice(-400000);
                
                // Recalculate message count (approximate)
                const totalLines = (logContainer.innerHTML.match(/\n/g) || []).length;
                messageCount = totalLines + 1;
            }
            
            if (autoscroll) {
                logContainer.scrollTop = logContainer.scrollHeight;
            }
        }
        
        // Colorize log messages based on content
        function colorizeLog(messages) {
            return messages.replace(/\[(\d+s)\]/g, '<span class="log-timestamp">[$1]</span>')
                          .replace(/\[WiFi\]/g, '<span class="log-wifi">[WiFi]</span>')
                          .replace(/\[webserver\]/g, '<span class="log-webserver">[webserver]</span>')
                          .replace(/\[modbusCache\]/g, '<span class="log-modbuscache">[modbusCache]</span>')
                          .replace(/\[config\]/g, '<span class="log-config">[config]</span>')
                          .replace(/\[setup\]/g, '<span class="log-setup">[setup]</span>')
                          .replace(/\[ws\]/g, '<span class="log-wifi">[ws]</span>')
                          .replace(/Error|Failed|failed|error/gi, '<span class="log-error">$&</span>');
        }
        
        // Clear log
        clearLogBtn.addEventListener('click', function() {
            fetch('/logclear', { method: 'POST' })
                .then(() => {
                    logContainer.innerHTML = '';
                    position = 0;
                    messageCount = 0;
                    updateCount = 0;
                    lastUpdateTime = Date.now();
                    updatesPerSecond = 0;
                    document.getElementById('buffer-info').textContent = 
                        'Buffer size: 32KB (approx. 400-800 messages) - Currently showing: ~0 messages - Updates: 0/sec';
                })
                .catch(error => {
                    console.error('Error clearing log:', error);
                });
        });
        
        // Toggle autoscroll
        toggleAutoscrollBtn.addEventListener('click', function() {
            autoscroll = !autoscroll;
            this.textContent = `Autoscroll: ${autoscroll ? 'ON' : 'OFF'}`;
            this.classList.toggle('autoscroll-enabled', autoscroll);
            
            if (autoscroll) {
                logContainer.scrollTop = logContainer.scrollHeight;
            }
        });
        
        // Download log
        downloadLogBtn.addEventListener('click', function() {
            const logText = logContainer.innerText;
            const blob = new Blob([logText], { type: 'text/plain' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `esp32_log_${new Date().toISOString().replace(/[:.]/g, '-')}.txt`;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
        });
    </script>
    )");
    
    sendResponseTrailer(response);
}
