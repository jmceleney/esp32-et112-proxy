#include "pages.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <atomic>
#include <map>
#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_image_format.h>
#include <WiFi.h>
#include <ESPmDNS.h>

// Global flags for scheduled restart after filesystem upload
bool filesystemUploadRestart = false;
unsigned long restartTime = 0;

#define ETAG "\"" __DATE__ "" __TIME__ "\""
// Define a build time string for display on the status page
#define BUILD_TIME_STR __DATE__ " " __TIME__

// Include version header if it exists
#ifdef __has_include
    #if __has_include("version.h")
        #include "version.h"
    #endif
#endif

// Check if GIT_VERSION is defined by our pre-build script
#ifndef GIT_VERSION
    #ifdef FIRMWARE_VERSION
        #define GIT_VERSION FIRMWARE_VERSION
    #else
        #define GIT_VERSION "Unknown"
    #endif
#endif

// External variable declaration for WiFi connection time
extern unsigned long lastWiFiConnectionTime;

// Variables for connection handling
static std::atomic<int> activeConnections{0};
static const int MAX_CONNECTIONS = 10; // Maximum concurrent connections

// BSSID cache with expiry and size limits
static std::map<String, std::pair<String, unsigned long>> bssidCache;
static const unsigned long BSSID_CACHE_EXPIRY = 24 * 60 * 60 * 1000; // 24 hours
static const size_t MAX_BSSID_CACHE_SIZE = 50;

// Adaptive OTA constants and structures
enum class FirmwareType {
    UNKNOWN,
    LEGACY_APP,      // Traditional single-partition firmware
    LEGACY_SPIFFS,   // Traditional filesystem only
    COMBINED         // Multi-partition combined firmware
};

struct OTAContext {
    FirmwareType type;
    esp_ota_handle_t ota_handle;
    const esp_partition_t* update_partition;
    size_t written;
    bool initialized;
    bool finalization_successful;
    uint8_t* buffer;
    size_t buffer_size;
    size_t buffer_pos;
    
    OTAContext() : type(FirmwareType::UNKNOWN), ota_handle(0), update_partition(nullptr), 
                   written(0), initialized(false), finalization_successful(false), buffer(nullptr), buffer_size(0), buffer_pos(0) {}
};

static OTAContext ota_context;

// Helper function to log heap memory at the start of each page request
void logHeapMemory(const char* route) {
  String message = String("[webserver] GET ") + route + " - Free heap: " + String(ESP.getFreeHeap()) + " bytes";
  dbgln(message);
}

// Helper function for handling connection limits
bool canAcceptConnection() {
  int currentConnections = activeConnections.load();
  if (currentConnections >= MAX_CONNECTIONS) {
    dbgln("[webserver] Too many active connections: " + String(currentConnections) + " - Rejecting new connection");
    return false;
  }
  activeConnections.fetch_add(1);
  return true;
}

// Helper function to release connection count
void releaseConnection() {
  int currentConnections = activeConnections.load();
  if (currentConnections > 0) {
    activeConnections.fetch_sub(1);
  }
}

// Helper function to clean up expired BSSID cache entries
void cleanupBssidCache() {
  unsigned long currentTime = millis();
  auto it = bssidCache.begin();
  while (it != bssidCache.end()) {
    if (currentTime - it->second.second > BSSID_CACHE_EXPIRY) {
      it = bssidCache.erase(it);
    } else {
      ++it;
    }
  }
}

// Firmware detection functions
FirmwareType detectFirmwareType(const uint8_t* data, size_t len, const String& filename) {
    dbgln("[OTA] Detecting firmware type from " + filename + ", data length: " + String(len));
    
    // Debug first few bytes
    dbg("[OTA] First 16 bytes: ");
    for (int i = 0; i < 16 && i < len; i++) {
        dbg("0x"); dbg(data[i], HEX); dbg(" ");
    }
    dbgln();
    
    // If filename explicitly indicates filesystem, treat as legacy filesystem
    if (filename == "filesystem" || filename.endsWith(".spiffs") || filename.endsWith(".littlefs")) {
        dbgln("[OTA] Detected LEGACY_SPIFFS from filename");
        return FirmwareType::LEGACY_SPIFFS;
    }
    
    if (len < 0x10000) {  // Need at least 64KB to check for combined firmware structure
        dbgln("[OTA] Not enough data for combined firmware detection (" + String(len) + " < 65536), checking for legacy app");
        // Check for ESP32 app image magic byte at start (legacy firmware)
        if (len >= 4 && data[0] == ESP_IMAGE_HEADER_MAGIC) {
            dbgln("[OTA] Detected LEGACY_APP from magic byte 0x" + String(data[0], HEX) + " at start");
            return FirmwareType::LEGACY_APP;
        }
        dbgln("[OTA] No magic byte found at start (0x" + String(data[0], HEX) + "), returning UNKNOWN");
        return FirmwareType::UNKNOWN;
    }
    
    // Check for combined firmware structure
    dbgln("[OTA] Checking for combined firmware structure in " + String(len) + " byte file");
    
    // Combined firmware should have:
    // - Bootloader at 0x1000 with magic byte 0xE9
    // - Partition table at 0x8000
    // - Application at 0x10000 with magic byte 0xE9
    
    bool has_bootloader = (len > 0x1000 && data[0x1000] == ESP_IMAGE_HEADER_MAGIC);
    bool has_app = (len > 0x10000 && data[0x10000] == ESP_IMAGE_HEADER_MAGIC);
    
    // Check for partition table signature at 0x8000
    // ESP32 partition table starts with 0xAA50 magic
    bool has_partition_table = (len > 0x8002 && 
                               data[0x8000] == 0xAA && 
                               data[0x8001] == 0x50);
    
    dbgln("[OTA] Bootloader at 0x1000: " + String(has_bootloader ? "YES" : "NO") + 
          " (0x" + String(len > 0x1000 ? data[0x1000] : 0, HEX) + ")");
    dbgln("[OTA] App at 0x10000: " + String(has_app ? "YES" : "NO") + 
          " (0x" + String(len > 0x10000 ? data[0x10000] : 0, HEX) + ")");
    dbgln("[OTA] Partition table at 0x8000: " + String(has_partition_table ? "YES" : "NO") + 
          " (0x" + String(len > 0x8000 ? data[0x8000] : 0, HEX) + 
          String(len > 0x8001 ? data[0x8001] : 0, HEX) + ")");
    
    if (has_bootloader && has_partition_table && has_app) {
        dbgln("[OTA] Detected COMBINED firmware (bootloader + partition table + app found)");
        return FirmwareType::COMBINED;
    }
    
    // Check if it's a legacy app firmware (starts with ESP image header)
    if (data[0] == ESP_IMAGE_HEADER_MAGIC) {
        dbgln("[OTA] Detected LEGACY_APP from magic byte 0x" + String(data[0], HEX) + " at start");
        return FirmwareType::LEGACY_APP;
    }
    
    dbgln("[OTA] Could not detect firmware type, magic byte at start: 0x" + String(data[0], HEX));
    dbgln("[OTA] Defaulting to UNKNOWN");
    return FirmwareType::UNKNOWN;
}

bool initializeLegacyOTA(const String& filename, FirmwareType type) {
    dbgln("[OTA] Initializing legacy OTA for " + filename + ", type: " + String(static_cast<int>(type)));
    
    int cmd = (type == FirmwareType::LEGACY_SPIFFS) ? U_SPIFFS : U_FLASH;
    dbgln("[OTA] Using Update command: " + String(cmd) + " (" + (cmd == U_SPIFFS ? "U_SPIFFS" : "U_FLASH") + ")");
    
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
        dbgln("[OTA] Legacy Update.begin() failed");
        Update.printError(Serial);
        return false;
    }
    
    ota_context.type = type;
    ota_context.initialized = true;
    dbgln("[OTA] Legacy OTA initialized successfully - context type: " + String(static_cast<int>(ota_context.type)) + 
          ", initialized: " + String(ota_context.initialized));
    return true;
}

bool initializeCombinedOTA() {
    dbgln("[OTA] Initializing combined firmware OTA");
    
    // For combined firmware, we need to use low-level ESP32 OTA APIs
    // Get the next available OTA partition
    ota_context.update_partition = esp_ota_get_next_update_partition(NULL);
    if (!ota_context.update_partition) {
        dbgln("[OTA] No available OTA partition found");
        return false;
    }
    
    dbgln("[OTA] Found OTA partition: " + String(ota_context.update_partition->label) + 
          " at address 0x" + String(ota_context.update_partition->address, HEX) +
          ", size: " + String(ota_context.update_partition->size));
    
    // For combined firmware, we'll write directly to flash at specific addresses
    // First, we need to erase the entire flash area we'll write to
    esp_err_t err = esp_ota_begin(ota_context.update_partition, OTA_SIZE_UNKNOWN, &ota_context.ota_handle);
    if (err != ESP_OK) {
        dbgln("[OTA] esp_ota_begin failed: " + String(esp_err_to_name(err)));
        return false;
    }
    
    ota_context.type = FirmwareType::COMBINED;
    ota_context.initialized = true;
    ota_context.written = 0;
    
    dbgln("[OTA] Combined firmware OTA initialized successfully - handle: " + String(ota_context.ota_handle) +
          ", context type: " + String(static_cast<int>(ota_context.type)) + 
          ", initialized: " + String(ota_context.initialized));
    return true;
}

bool writeLegacyOTAData(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        dbgln("[OTA] Invalid data for legacy write");
        return false;
    }
    
    if (!ota_context.initialized) {
        dbgln("[OTA] Legacy OTA not properly initialized");
        return false;
    }
    
    if (Update.write(const_cast<uint8_t*>(data), len) != len) {
        dbgln("[OTA] Legacy Update.write failed");
        return false;
    }
    
    ota_context.written += len;
    return true;
}

bool writeCombinedOTAData(const uint8_t* data, size_t len, size_t offset) {
    // For combined firmware, we need to extract both application (0x10000-0x290000) and filesystem (0x290000+) portions
    // The ESP32 OTA API handles the application, and we handle filesystem separately
    
    // Validate input
    if (!data || len == 0) {
        dbgln("[OTA] Invalid data for combined firmware write");
        return false;
    }
    
    if (!ota_context.initialized || ota_context.ota_handle == 0) {
        dbgln("[OTA] Combined OTA not properly initialized");
        return false;
    }
    
    // Define flash layout constants
    const size_t APP_OFFSET = 0x10000;     // Application starts at 64KB
    const size_t SPIFFS_OFFSET = 0x290000; // Filesystem starts at ~2.6MB
    
    // Calculate which part of current chunk overlaps with application area
    size_t chunk_start = offset;
    size_t chunk_end = offset + len;
    
    static uint32_t combined_chunk_count = 0;
    static const esp_partition_t* spiffs_partition = nullptr;
    static size_t filesystem_written = 0;
    static bool filesystem_initialized = false;
    
    // Reset counter when processing starts fresh  
    if (offset == 0) {
      combined_chunk_count = 0;
      spiffs_partition = nullptr;
      filesystem_written = 0;
      filesystem_initialized = false;
    }
    combined_chunk_count++;
    
    // Only log chunk processing details every 4th chunk to reduce verbosity
    if (combined_chunk_count % 4 == 0) {
      dbgln("[OTA] Processing chunk: offset=" + String(chunk_start, HEX) + 
            ", len=" + String(len) + ", end=" + String(chunk_end, HEX));
    }
    
    // Handle different sections of the combined firmware
    bool hasAppData = false;
    bool hasFilesystemData = false;
    
    // Check what type of data this chunk contains
    if (chunk_end > APP_OFFSET && chunk_start < SPIFFS_OFFSET) {
        hasAppData = true;
    }
    if (chunk_end > SPIFFS_OFFSET) {
        hasFilesystemData = true;
    }
    
    // Skip bootloader and partition table data (before APP_OFFSET)
    if (chunk_end <= APP_OFFSET) {
        dbgln("[OTA] Skipping bootloader/partition table data");
        return true;
    }
    
    if (!hasAppData && !hasFilesystemData) {
        dbgln("[OTA] No relevant data in this chunk");
        return true;
    }
    
    // Handle APPLICATION data (0x10000 - 0x290000)
    if (hasAppData) {
        // Calculate the portion of this chunk that belongs to application
        size_t app_start_in_chunk = 0;
        size_t app_len_in_chunk = len;
        
        if (chunk_start < APP_OFFSET) {
            // Chunk starts before app area, skip the beginning
            app_start_in_chunk = APP_OFFSET - chunk_start;
            app_len_in_chunk -= app_start_in_chunk;
        }
        
        if (chunk_end > SPIFFS_OFFSET) {
            // Chunk extends beyond app area, truncate the end
            app_len_in_chunk -= (chunk_end - SPIFFS_OFFSET);
        }
        
        if (app_len_in_chunk > 0) {
            // Check if we would exceed the OTA partition size
            size_t remaining_partition_space = ota_context.update_partition->size - ota_context.written;
            if (app_len_in_chunk > remaining_partition_space) {
                if (remaining_partition_space == 0) {
                    dbgln("[OTA] OTA partition is full (" + String(ota_context.update_partition->size) + 
                          " bytes), skipping remaining app data");
                } else {
                    dbgln("[OTA] Truncating app write to fit partition: " + String(app_len_in_chunk) + 
                          " -> " + String(remaining_partition_space) + " bytes");
                    app_len_in_chunk = remaining_partition_space;
                }
            }
            
            if (app_len_in_chunk > 0) {
                // Extract application data from chunk
                const uint8_t* app_data = data + app_start_in_chunk;
                
                // Check if the data looks like valid application code (starts with 0xE9 or is continuation)
                if (ota_context.written == 0 && app_data[0] != 0xE9) {
                    dbgln("[OTA] Warning: First application byte is not 0xE9 (got 0x" + String(app_data[0], HEX) + ")");
                }
                
                // Skip chunks that are all 0xFF (padding/unused space)
                bool all_ff = true;
                for (size_t i = 0; i < app_len_in_chunk && all_ff; i++) {
                    if (app_data[i] != 0xFF) {
                        all_ff = false;
                    }
                }
                
                if (!all_ff) {
                    // Only log write details every 4th chunk to reduce verbosity
                    if (combined_chunk_count % 4 == 0) {
                        dbgln("[OTA] Writing " + String(app_len_in_chunk) + " bytes of application data (chunk offset: " + 
                              String(app_start_in_chunk) + ")");
                    }
                    
                    // Write application data to OTA partition
                    esp_err_t err = esp_ota_write(ota_context.ota_handle, app_data, app_len_in_chunk);
                    if (err != ESP_OK) {
                        dbgln("[OTA] esp_ota_write failed: " + String(esp_err_to_name(err)));
                        return false;
                    }
                    
                    ota_context.written += app_len_in_chunk;
                    // Only log progress details every 4th chunk to reduce verbosity
                    if (combined_chunk_count % 4 == 0) {
                        dbgln("[OTA] Combined firmware: wrote " + String(app_len_in_chunk) + 
                              " app bytes, total app written: " + String(ota_context.written) +
                              "/" + String(ota_context.update_partition->size));
                    }
                } else {
                    dbgln("[OTA] Skipping app padding data (all 0xFF bytes)");
                }
            }
        }
    }
    
    // Handle FILESYSTEM data (0x290000+)
    if (hasFilesystemData) {
        // Initialize filesystem handling on first filesystem chunk
        if (!filesystem_initialized) {
            dbgln("[OTA] Initializing filesystem deployment from combined firmware");
            
            // Find the SPIFFS partition
            spiffs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
            
            if (!spiffs_partition) {
                dbgln("[OTA] ERROR: SPIFFS partition not found for filesystem deployment!");
                return false;
            }
            
            dbgln("[OTA] Found SPIFFS partition for filesystem deployment, erasing...");
            
            // Erase the SPIFFS partition first
            esp_err_t err = esp_partition_erase_range(spiffs_partition, 0, spiffs_partition->size);
            if (err != ESP_OK) {
                dbgln("[OTA] ERROR: Failed to erase SPIFFS partition: " + String(esp_err_to_name(err)));
                return false;
            }
            
            dbgln("[OTA] SPIFFS partition erased, ready for filesystem data");
            filesystem_initialized = true;
            filesystem_written = 0;
        }
        
        // Calculate filesystem portion of this chunk
        size_t fs_start_in_chunk = 0;
        size_t fs_len_in_chunk = len;
        
        if (chunk_start < SPIFFS_OFFSET) {
            // Chunk starts before filesystem area, skip the beginning
            fs_start_in_chunk = SPIFFS_OFFSET - chunk_start;
            fs_len_in_chunk -= fs_start_in_chunk;
        }
        
        if (fs_len_in_chunk > 0) {
            // Check if we would exceed the filesystem partition size
            if (filesystem_written + fs_len_in_chunk > spiffs_partition->size) {
                size_t remaining = spiffs_partition->size - filesystem_written;
                if (remaining == 0) {
                    dbgln("[OTA] Filesystem partition is full (" + String(spiffs_partition->size) + 
                          " bytes), skipping remaining filesystem data");
                } else {
                    dbgln("[OTA] Truncating filesystem write to fit partition: " + String(fs_len_in_chunk) + 
                          " -> " + String(remaining) + " bytes");
                    fs_len_in_chunk = remaining;
                }
            }
            
            if (fs_len_in_chunk > 0) {
                const uint8_t* fs_data = data + fs_start_in_chunk;
                
                // Only log write details every 4th chunk to reduce verbosity
                if (combined_chunk_count % 4 == 0) {
                    dbgln("[OTA] Writing " + String(fs_len_in_chunk) + " bytes of filesystem data (chunk offset: " + 
                          String(fs_start_in_chunk) + ")");
                }
                
                // Write filesystem data directly to SPIFFS partition
                esp_err_t err = esp_partition_write(spiffs_partition, filesystem_written, fs_data, fs_len_in_chunk);
                if (err != ESP_OK) {
                    dbgln("[OTA] ERROR: Failed to write filesystem data at offset " + String(filesystem_written) + ": " + String(esp_err_to_name(err)));
                    return false;
                }
                
                filesystem_written += fs_len_in_chunk;
                // Only log progress details every 4th chunk to reduce verbosity
                if (combined_chunk_count % 4 == 0) {
                    dbgln("[OTA] Combined firmware: wrote " + String(fs_len_in_chunk) + 
                          " filesystem bytes, total filesystem written: " + String(filesystem_written) +
                          "/" + String(spiffs_partition->size));
                }
            }
        }
    }
    
    return true;
}

bool finalizeLegacyOTA() {
    dbgln("[OTA] Finalizing legacy OTA");
    
    if (!Update.end(true)) {
        dbgln("[OTA] Legacy Update.end failed");
        Update.printError(Serial);
        ota_context.finalization_successful = false;
        return false;
    }
    
    ota_context.finalization_successful = true;
    dbgln("[OTA] Legacy OTA finalized successfully");
    return true;
}

bool finalizeCombinedOTA() {
    dbgln("[OTA] Finalizing combined firmware OTA");
    
    esp_err_t err = esp_ota_end(ota_context.ota_handle);
    if (err != ESP_OK) {
        dbgln("[OTA] esp_ota_end failed: " + String(esp_err_to_name(err)));
        ota_context.finalization_successful = false;
        return false;
    }
    
    // Set the new partition as the next boot partition
    err = esp_ota_set_boot_partition(ota_context.update_partition);
    if (err != ESP_OK) {
        dbgln("[OTA] esp_ota_set_boot_partition failed: " + String(esp_err_to_name(err)));
        ota_context.finalization_successful = false;
        return false;
    }
    
    ota_context.finalization_successful = true;
    dbgln("[OTA] Combined firmware OTA finalized successfully");
    dbgln("[OTA] ✓ Application firmware deployed to OTA partition");
    dbgln("[OTA] ✓ Filesystem deployed to LittleFS partition");
    dbgln("[OTA] Combined firmware deployment complete - both firmware and web interface updated");
    return true;
}

void cleanupOTAContext() {
    dbgln("[OTA] Cleaning up OTA context");
    if (ota_context.buffer) {
        free(ota_context.buffer);
        ota_context.buffer = nullptr;
    }
    ota_context.buffer_size = 0;
    ota_context.buffer_pos = 0;
    // Only reset type if we're doing a real cleanup, not mid-process
    ota_context.type = FirmwareType::UNKNOWN;
    ota_context.initialized = false;
    // Don't reset finalization_successful - we need it for the response
    ota_context.written = 0;
    ota_context.ota_handle = 0;
    ota_context.update_partition = nullptr;
}

void resetOTAContextForNewUpload() {
    dbgln("[OTA] Resetting OTA context for new upload");
    dbgln("[OTA] Context before reset - type: " + String(static_cast<int>(ota_context.type)) + 
          ", initialized: " + String(ota_context.initialized) + 
          ", written: " + String(ota_context.written));
    cleanupOTAContext();
    ota_context.finalization_successful = false;
    dbgln("[OTA] Context after reset - type: " + String(static_cast<int>(ota_context.type)) + 
          ", initialized: " + String(ota_context.initialized) + 
          ", finalization_successful: " + String(ota_context.finalization_successful));
}

void setupPages(AsyncWebServer *server, ModbusCache *modbusCache, Config *config, AsyncWiFiManager *wm){
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
      unsigned long currentTime = millis();
      
      // Clean up expired cache entries periodically
      static unsigned long lastCleanup = 0;
      if (currentTime - lastCleanup > 300000) { // Every 5 minutes
          cleanupBssidCache();
          lastCleanup = currentTime;
      }
      
      // Check if we have a valid cached result for this BSSID
      auto it = bssidCache.find(bssid);
      if (it != bssidCache.end() && 
          currentTime - it->second.second < BSSID_CACHE_EXPIRY) {
          dbgln("[BSSID] Cache hit for " + bssid);
          request->send(200, "application/json", it->second.first);
          return;
      }

      dbgln("[BSSID] Cache miss for " + bssid + ", fetching from API");
      WiFiClient client;
      HTTPClient http;
      String url = "http://api.maclookup.app/v2/macs/" + bssid;

      if (http.begin(client, url)) {
          http.setTimeout(5000); // 5 second timeout
          int httpCode = http.GET();
          if (httpCode == HTTP_CODE_OK) {
              String payload = http.getString();
              
              // Manage cache size - remove oldest entries if needed
              if (bssidCache.size() >= MAX_BSSID_CACHE_SIZE) {
                  // Find and remove the oldest entry
                  auto oldestIt = bssidCache.begin();
                  unsigned long oldestTime = oldestIt->second.second;
                  for (auto cacheIt = bssidCache.begin(); cacheIt != bssidCache.end(); ++cacheIt) {
                      if (cacheIt->second.second < oldestTime) {
                          oldestTime = cacheIt->second.second;
                          oldestIt = cacheIt;
                      }
                  }
                  bssidCache.erase(oldestIt);
                  dbgln("[BSSID] Cache full, removed oldest entry");
              }
              
              // Update cache with new data
              bssidCache[bssid] = std::make_pair(payload, currentTime);
              request->send(200, "application/json", payload);
          } else {
              // On error, try to use any cached data if available (even expired)
              auto fallbackIt = bssidCache.find(bssid);
              if (fallbackIt != bssidCache.end()) {
                  dbgln("[BSSID] API request failed, using cached data (possibly expired)");
                  request->send(200, "application/json", fallbackIt->second.first);
              } else {
                  request->send(500, "application/json", "{\"error\":\"API request failed\"}");
              }
          }
          http.end();
      } else {
          // On connection failure, try to use any cached data if available
          auto fallbackIt = bssidCache.find(bssid);
          if (fallbackIt != bssidCache.end()) {
              dbgln("[BSSID] HTTP connection failed, using cached data");
              request->send(200, "application/json", fallbackIt->second.first);
          } else {
              request->send(500, "application/json", "{\"error\":\"HTTP connection failed\"}");
          }
      }
  });

  // Legacy home route removed - now handled by Preact SPA

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

  // Legacy /reboot GET handler removed - now handled by Preact SPA
  server->on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
    dbgln("[webserver] POST /reboot");
    request->redirect("/");
    dbgln("[webserver] rebooting...")
    ESP.restart();
    dbgln("[webserver] rebooted...")
  });
  server->on("/config.json", HTTP_GET, [config](AsyncWebServerRequest *request){
    logHeapMemory("/config.json");
    
    // Check if we can accept more connections
    if (!canAcceptConnection()) {
      request->send(503, "application/json", "{\"error\":\"Server busy\"}");
      return;
    }
    
    DynamicJsonDocument doc(2048);
    
    doc["hostname"] = config->getHostname();
    doc["pi"] = config->getPollingInterval();
    doc["clientIsRTU"] = config->getClientIsRTU();
    
    // RTU Settings
    doc["mb"] = config->getModbusBaudRate();
    doc["md"] = config->getModbusDataBits();
    doc["mp"] = config->getModbusParity();
    doc["ms"] = config->getModbusStopBits();
    doc["mr"] = config->getModbusRtsPin();
    
    // TCP Settings
    doc["sip"] = config->getTargetIP();
    doc["tp2"] = config->getTcpPort2();
    
    // Secondary RTU Settings
    doc["mb2"] = config->getModbusBaudRate2();
    doc["md2"] = config->getModbusDataBits2();
    doc["mp2"] = config->getModbusParity2();
    doc["ms2"] = config->getModbusStopBits2();
    doc["mr2"] = config->getModbusRtsPin2();
    
    // TCP Server Settings
    doc["tp3"] = config->getTcpPort3();
    
    // Serial Debug Settings
    doc["sb"] = config->getSerialBaudRate();
    doc["sd"] = config->getSerialDataBits();
    doc["sp"] = config->getSerialParity();
    doc["ss"] = config->getSerialStopBits();
    
    // Network Settings
    doc["useStaticIP"] = config->getUseStaticIP();
    doc["staticIP"] = config->getStaticIP();
    doc["staticGateway"] = config->getStaticGateway();
    doc["staticSubnet"] = config->getStaticSubnet();
    
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
    
    // Release the connection count
    releaseConnection();
  });
  // Legacy GET /config handler removed - now handled by Preact SPA
  server->on("/config", HTTP_POST, [config](AsyncWebServerRequest *request){
    dbgln("[webserver] POST /config");
    bool validIP = true;
    if (request->hasParam("hostname", true)) {
        String hostname = request->getParam("hostname", true)->value();
        String oldHostname = config->getHostname();
        config->setHostname(hostname);  // Save the hostname in preferences
        dbgln("[webserver] saved hostname");
        
        // Restart mDNS if hostname changed and WiFi is connected
        if (hostname != oldHostname && WiFi.status() == WL_CONNECTED) {
            dbgln("[webserver] Hostname changed, restarting mDNS");
            MDNS.end();  // Stop current mDNS
            if (MDNS.begin(hostname.c_str())) {
                dbgln("[mDNS] Restarted with new hostname: " + hostname);
                // Re-add services
                MDNS.addService("http", "tcp", 80);
                MDNS.addService("modbus", "tcp", 502);
            } else {
                logErrln("[mDNS] Failed to restart with new hostname");
            }
        }
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
    
    // Return JSON response instead of redirect for Preact SPA compatibility
    if (validIP) {
        String jsonResponse = "{\"success\": true, \"message\": \"Configuration updated successfully\"}";
        request->send(200, "application/json", jsonResponse);
    } else {
        String jsonResponse = "{\"success\": false, \"message\": \"Invalid IP address provided\"}";
        request->send(400, "application/json", jsonResponse);
    }
  });
  // Legacy /debug GET handler removed - now handled by Preact SPA
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

  // OTA Upload endpoint for Preact frontend (POST only - no legacy HTML GET)
  server->on("/update", HTTP_POST, [config](AsyncWebServerRequest *request){
    String hostname = config->getHostname();
    dbgln("[webserver] Adaptive OTA finished");
    
    // Check for errors based on firmware type and finalization success
    bool hasError = false;
    String errorMsg = "";
    String successMsg = "";
    
    if (ota_context.type == FirmwareType::COMBINED) {
      // Combined firmware uses ESP32 OTA APIs, check finalization success
      hasError = !ota_context.finalization_successful;
      if (hasError) {
        errorMsg = "Combined firmware OTA failed";
      } else {
        successMsg = "Combined firmware update successful! Device will reboot in 3 seconds...";
      }
    } else if (ota_context.type == FirmwareType::LEGACY_APP || ota_context.type == FirmwareType::LEGACY_SPIFFS) {
      // Legacy firmware uses Arduino Update library
      hasError = Update.hasError() || !ota_context.finalization_successful;
      if (hasError) {
        errorMsg = "Legacy firmware OTA failed";
      } else {
        successMsg = "Legacy firmware update successful! Device will reboot in 3 seconds...";
      }
    } else {
      hasError = true;
      errorMsg = "Unknown firmware type or OTA not properly initialized";
    }
    
    if (hasError) {
      // Cleanup context on failure
      cleanupOTAContext();
      String jsonResponse = "{\"success\": false, \"message\": \"" + errorMsg + "\", \"reboot\": false}";
      auto *response = request->beginResponse(500, "application/json", jsonResponse);
      response->addHeader("Connection", "close");
      request->send(response);
    } else {
      // Success - send JSON response then schedule reboot
      String jsonResponse = "{\"success\": true, \"message\": \"" + successMsg + "\", \"reboot\": true}";
      auto *response = request->beginResponse(200, "application/json", jsonResponse);
      response->addHeader("Connection", "close");
      request->send(response);
      
      // Schedule reboot after a delay to ensure response is sent
      request->onDisconnect([](){
        // Give extra time for response to be sent
        unsigned long rebootTime = millis() + 3000; // 3 second delay
        while (millis() < rebootTime) {
          delay(100);
          yield(); // Keep feeding watchdog
        }
        dbgln("[webserver] Rebooting after successful OTA update...");
        ESP.restart();
      });
      
      // Also cleanup context after successful response
      cleanupOTAContext();
    }
  }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    static uint32_t chunk_count = 0;
    
    // Reset counter at start of new upload
    if (index == 0) {
      chunk_count = 0;
    }
    chunk_count++;
    
    // Only log progress every 4th chunk to reduce verbosity
    if (chunk_count % 4 == 0 || !index || final) {
      dbg("[webserver] Adaptive OTA progress ");dbg(index);dbg(" len=");dbgln(len);
      dbgln("[webserver] Context state check - type: " + String(static_cast<int>(ota_context.type)) + 
            ", initialized: " + String(ota_context.initialized) + 
            ", written: " + String(ota_context.written));
    }
    
    // Check if this is either the first chunk OR if context is uninitialized
    if (!index || !ota_context.initialized) {
      if (!index) {
        dbgln("[webserver] Starting adaptive OTA for file: " + filename + " (first chunk)");
      } else {
        dbgln("[webserver] Starting adaptive OTA for file: " + filename + " (context uninitialized, index=" + String(index) + ")");
      }
      
      // Reset context for new upload
      resetOTAContextForNewUpload();
      
      // For non-zero index, we can't detect firmware type from partial data
      // Default to combined firmware type for uploads not starting at index 0
      FirmwareType detectedType;
      if (!index) {
        detectedType = detectFirmwareType(data, len, filename);
      } else {
        dbgln("[webserver] Cannot detect firmware type from partial data, defaulting to combined");
        detectedType = FirmwareType::COMBINED;
      }
      
      bool initSuccess = false;
      switch (detectedType) {
        case FirmwareType::LEGACY_APP:
        case FirmwareType::LEGACY_SPIFFS:
          initSuccess = initializeLegacyOTA(filename, detectedType);
          break;
          
        case FirmwareType::COMBINED:
          initSuccess = initializeCombinedOTA();
          break;
          
        default:
          dbgln("[webserver] Unknown firmware type, attempting legacy app detection");
          // Fallback to legacy app if detection failed
          initSuccess = initializeLegacyOTA(filename, FirmwareType::LEGACY_APP);
          break;
      }
      
      if (!initSuccess) {
        cleanupOTAContext();
        return request->send(400, "text/plain", "Adaptive OTA could not begin");
      }
      
      dbgln("[webserver] Adaptive OTA initialized for type: " + String(static_cast<int>(ota_context.type)));
    }
    
    // Write data chunk
    if (len > 0) {
      bool writeSuccess = false;
      
      // Debug current context state - only log every 4th chunk to reduce verbosity
      if (chunk_count % 4 == 0 || final) {
        dbg("[webserver] Context type: ");dbg(static_cast<int>(ota_context.type));
        dbg(", initialized: ");dbg(ota_context.initialized);
        dbg(", written so far: ");dbgln(ota_context.written);
      }
      
      switch (ota_context.type) {
        case FirmwareType::LEGACY_APP:
        case FirmwareType::LEGACY_SPIFFS:
          writeSuccess = writeLegacyOTAData(data, len);
          break;
          
        case FirmwareType::COMBINED:
          writeSuccess = writeCombinedOTAData(data, len, index);
          break;
          
        default:
          dbgln("[webserver] Invalid OTA context type during write - type: " + String(static_cast<int>(ota_context.type)));
          cleanupOTAContext();
          return request->send(400, "text/plain", "Invalid OTA state during write");
      }
      
      if (!writeSuccess) {
        dbgln("[webserver] Failed to write OTA data chunk");
        cleanupOTAContext();
        return request->send(400, "text/plain", "Adaptive OTA could not write data");
      }
    }
    
    // Finalize on last chunk
    if (final) {
      dbgln("[webserver] Finalizing adaptive OTA");
      
      bool finalizeSuccess = false;
      
      switch (ota_context.type) {
        case FirmwareType::LEGACY_APP:
        case FirmwareType::LEGACY_SPIFFS:
          finalizeSuccess = finalizeLegacyOTA();
          break;
          
        case FirmwareType::COMBINED:
          finalizeSuccess = finalizeCombinedOTA();
          break;
          
        default:
          dbgln("[webserver] Invalid OTA context type during finalize");
          break;
      }
      
      if (!finalizeSuccess) {
        dbgln("[webserver] Failed to finalize adaptive OTA");
        cleanupOTAContext();
        return request->send(400, "text/plain", "Could not finalize adaptive OTA");
      }
      
      dbgln("[webserver] Adaptive OTA finalized successfully");
    }
  });

  // Developer-only endpoint to wipe LittleFS filesystem for testing
  server->on("/wipe-filesystem", HTTP_POST, [](AsyncWebServerRequest *request) {
      dbgln("[webserver] POST /wipe-filesystem - DEVELOPER TESTING ENDPOINT");
      
      // Format the LittleFS filesystem (wipes all files)
      if (LittleFS.format()) {
          dbgln("[webserver] LittleFS filesystem wiped successfully");
          String jsonResponse = "{\"success\": true, \"message\": \"Filesystem wiped successfully. Device may need reboot.\"}";
          request->send(200, "application/json", jsonResponse);
      } else {
          dbgln("[webserver] LittleFS filesystem wipe failed");
          String jsonResponse = "{\"success\": false, \"message\": \"Failed to wipe filesystem\"}";
          request->send(500, "application/json", jsonResponse);
      }
  });

  // Dedicated filesystem upload handler (separate from firmware OTA)
  server->on("/upload-filesystem", HTTP_POST, [&](AsyncWebServerRequest *request){
      // This will be called when the upload is complete
      if (filesystemUploadRestart) {
          dbgln("[webserver] Filesystem upload completed successfully");
      }
  }, [&](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){
      dbgln("[webserver] POST /upload-filesystem - Filesystem upload handler");
      
      static const esp_partition_t* spiffs_partition = nullptr;
      static size_t totalSize = 0;
      static bool uploadError = false;
      
      if (index == 0) {
          // First chunk - initialize upload
          dbgln("[webserver] Starting filesystem upload: " + filename);
          totalSize = 0;
          uploadError = false;
          filesystemUploadRestart = false;
          
          // Find the SPIFFS partition
          spiffs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
          
          if (!spiffs_partition) {
              dbgln("[webserver] SPIFFS partition not found");
              request->send(500, "text/plain", "SPIFFS partition not found");
              uploadError = true;
              return;
          }
          
          dbgln("[webserver] Found SPIFFS partition, erasing...");
          
          // Erase the SPIFFS partition first
          esp_err_t err = esp_partition_erase_range(spiffs_partition, 0, spiffs_partition->size);
          if (err != ESP_OK) {
              dbgln("[webserver] Failed to erase SPIFFS partition: " + String(err));
              request->send(500, "text/plain", "Failed to erase filesystem partition");
              uploadError = true;
              return;
          }
          
          dbgln("[webserver] SPIFFS partition erased, ready for data");
      }
      
      if (uploadError) {
          return; // Skip processing if there was an error
      }
      
      if (spiffs_partition) {
          // Write chunk directly to SPIFFS partition
          esp_err_t err = esp_partition_write(spiffs_partition, totalSize, data, len);
          
          if (err != ESP_OK) {
              dbgln("[webserver] Failed to write to SPIFFS partition at offset " + String(totalSize) + ": " + String(err));
              request->send(500, "text/plain", "Failed to write to filesystem partition");
              uploadError = true;
              return;
          }
          
          totalSize += len;
          dbgln("[webserver] Written " + String(len) + " bytes at offset " + String(totalSize - len) + ", total: " + String(totalSize));
      }
      
      if (final && !uploadError) {
          dbgln("[webserver] Filesystem upload complete. Total size: " + String(totalSize));
          dbgln("[webserver] Filesystem upload successful. Scheduling restart...");
          
          // Send success response
          request->send(200, "text/html", 
              "<html><body style='background:#1a1a1a;color:white;text-align:center;font-family:Arial;'>"
              "<h2>Filesystem Upload Successful!</h2>"
              "<p>The device will reboot in 5 seconds...</p>"
              "<p>Please wait 45-60 seconds and then <a href='/' style='color:#1fa3ec;'>click here</a> to access the full web interface.</p>"
              "<script>"
              "var countdown = 5;"
              "function updateCountdown() {"
              "  document.body.innerHTML = '<h2>Device Rebooting in ' + countdown + ' seconds...</h2><p>Please wait and <a href=\"/\" style=\"color:#1fa3ec;\">click here</a> after reboot.</p>';"
              "  countdown--;"
              "  if (countdown < 0) {"
              "    document.body.innerHTML = '<h2>Device Rebooting Now...</h2><p>Please wait 45 seconds and <a href=\"/\" style=\"color:#1fa3ec;\">click here</a> to reload.</p>';"
              "    setTimeout(function(){window.location.href='/';}, 45000);"
              "  } else {"
              "    setTimeout(updateCountdown, 1000);"
              "  }"
              "}"
              "setTimeout(updateCountdown, 1000);"
              "</script>"
              "</body></html>");
          
          // Schedule restart for 5 seconds from now
          filesystemUploadRestart = true;
          restartTime = millis() + 5000;
      }
  });

  // Legacy /wifi GET handler removed - now handled by Preact SPA
  server->on("/wifi", HTTP_POST, [wm](AsyncWebServerRequest *request){
    dbgln("[webserver] POST /wifi");
    inConfigPortal = true; // Set flag before erasing to prevent interference
    request->redirect("/");
    wm->resetSettings();
    dbgln("[webserver] erased wifi config");
    delay(100); // Small delay to ensure response is sent
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
  // Legacy /log GET handler removed - now handled by Preact SPA
  
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

  // Root handler - detect filesystem and redirect if missing
  server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      logHeapMemory("/");
      
      // Check if LittleFS is mounted and contains the Preact app
      if (LittleFS.exists("/web/index.html")) {
          // Filesystem exists, serve the Preact app
          request->send(LittleFS, "/web/index.html", "text/html");
      } else {
          // Filesystem missing, redirect to upload page
          dbgln("[webserver] Filesystem missing, redirecting to /filesystem-upload");
          request->redirect("/filesystem-upload");
      }
  });

  // Filesystem upload page for legacy devices
  server->on("/filesystem-upload", HTTP_GET, [config](AsyncWebServerRequest *request) {
      logHeapMemory("/filesystem-upload");
      const String &hostname = config->getHostname();
      auto *response = request->beginResponseStream("text/html");
      
      sendResponseHeader(response, "Upload Filesystem", true, hostname);
      
      response->print(
          "<div style='text-align: left; max-width: 600px; margin: 0 auto;'>"
          "<h4>Filesystem Upload Required</h4>"
          "<p>This device needs the web interface filesystem to be uploaded. "
          "This is a one-time setup required after upgrading from legacy firmware.</p>"
          
          "<h4>Steps:</h4>"
          "<ol>"
          "<li><strong>Build the filesystem:</strong><br/>"
          "<code>pio run -e esp32debug -t buildfs</code></li>"
          "<li><strong>Locate the file:</strong><br/>"
          "Find <code>littlefs.bin</code> in <code>.pio/build/esp32debug/</code></li>"
          "<li><strong>Upload below:</strong> Select the littlefs.bin file and click Upload</li>"
          "</ol>"
          
          "<div style='background: #333; padding: 15px; border-radius: 5px; margin: 15px 0;'>"
          "<form method='post' action='/upload-filesystem' enctype='multipart/form-data'>"
          "<div style='margin: 10px 0;'>"
          "<label for='file' style='display: block; margin-bottom: 5px;'>Select LittleFS file:</label>"
          "<input type='file' id='file' name='file' accept='.bin' required "
          "style='width: 100%; padding: 5px; background: #222; color: white; border: 1px solid #555;'/>"
          "</div>"
          "<div style='margin: 15px 0;'>"
          "<button type='submit' style='width: 100%; padding: 10px; background: #1fa3ec; color: white; "
          "border: none; border-radius: 5px; font-size: 16px; cursor: pointer;'>"
          "Upload Filesystem</button>"
          "</div>"
          "</form>"
          "</div>"
          
          "<div style='background: #2a2a2a; padding: 10px; border-radius: 5px; font-size: 14px;'>"
          "<strong>Note:</strong> After successful upload, the device will reboot and the full web interface will be available."
          "</div>"
          "</div>"
      );
      
      sendResponseTrailer(response);
      request->send(response);
  });

  // Optimized asset serving routes for Preact - prevents filesystem blocking on simultaneous requests
  server->on("/assets/*", HTTP_GET, [](AsyncWebServerRequest *request) {
      String path = request->url();
      
      // Check if we can accept more connections
      if (!canAcceptConnection()) {
          request->send(503, "text/plain", "Server busy");
          return;
      }
      
      // Convert /assets/* to /web/assets/* for filesystem
      String fsPath = "/web" + path;
      
      // Determine content type
      String contentType = "text/plain";
      if (path.endsWith(".js")) {
          contentType = "application/javascript";
      } else if (path.endsWith(".css")) {
          contentType = "text/css";
      } else if (path.endsWith(".json")) {
          contentType = "application/json";
      }
      
      // Try to serve the exact file first
      if (LittleFS.exists(fsPath)) {
          request->send(LittleFS, fsPath, contentType);
          releaseConnection();
          return;
      }
      
      // Asset not found - try fallback for versioned assets (e.g., index.abc123.js)
      String assetType = "";
      if (path.indexOf("/index.") >= 0 && path.endsWith(".js")) {
          assetType = "index";
      } else if (path.indexOf("/vendor.") >= 0 && path.endsWith(".js")) {
          assetType = "vendor";
      } else if (path.indexOf("/style.") >= 0 && path.endsWith(".css")) {
          assetType = "style";
      }
      
      if (assetType != "") {
          // Quick fallback - try a few common patterns first before directory scan
          String basePath = "/web/assets/" + assetType;
          String extensions[] = {".js", ".css"};
          String patterns[] = {".min", "", ".prod", ".bundle"};
          
          for (String ext : extensions) {
              if ((assetType != "style" && ext == ".css") || (assetType == "style" && ext == ".js")) continue;
              for (String pattern : patterns) {
                  String tryPath = basePath + pattern + ext;
                  if (LittleFS.exists(tryPath)) {
                      dbgln("[webserver] Asset fallback: " + path + " -> " + tryPath);
                      request->send(LittleFS, tryPath, contentType);
                      releaseConnection();
                      return;
                  }
              }
          }
      }
      
      // Still not found - 404
      request->send(404, "text/plain", "Asset not found: " + path);
      releaseConnection();
  });

  // Legacy /assets/* route for compatibility
  server->on("/assets/*", HTTP_GET, [](AsyncWebServerRequest *request) {
      String path = "/web" + request->url();  // Prepend /web
      
      // Check if we can accept more connections
      if (!canAcceptConnection()) {
          request->send(503, "text/plain", "Server busy");
          return;
      }
      
      String contentType = "text/plain";
      if (path.endsWith(".js")) {
          contentType = "application/javascript";
      } else if (path.endsWith(".css")) {
          contentType = "text/css";
      } else if (path.endsWith(".json")) {
          contentType = "application/json";
      }
      
      if (LittleFS.exists(path)) {
          request->send(LittleFS, path, contentType);
          releaseConnection();
          return;
      }
      
      request->send(404, "text/plain", "Asset not found");
      releaseConnection();
  });

  // Catch all route for SPA - redirect to modern UI for unhandled routes
  server->onNotFound([](AsyncWebServerRequest *request) {
      String path = request->url();
      
      // Skip API routes and existing legacy routes
      if (path.startsWith("/api") || path.startsWith("/metrics") || 
          path.startsWith("/style.css") || 
          path.startsWith("/favicon.ico") || path.startsWith("/baudrate") ||
          path.startsWith("/menu")) {
          // Let the default 404 handler handle these
          request->send(404, "text/plain", "404");
          return;
      }

      // Handle /version.json specifically (non-asset JSON file)
      if (path == "/version.json") {
          String filePath = "/web/version.json";
          if (LittleFS.exists(filePath)) {
              request->send(LittleFS, filePath, "application/json");
              return;
          }
          request->send(404, "application/json", "{\"error\":\"Version file not found\"}");
          return;
      }

      // For all other routes, serve the Preact SPA
      if (LittleFS.exists("/web/index.html")) {
          request->send(LittleFS, "/web/index.html", "text/html");
      } else {
          request->send(404, "text/plain", "Web UI not found - please upload filesystem");
      }
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
