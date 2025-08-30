#ifndef PAGES_H
    #define PAGES_H

    #include <ESPAsyncWiFiManager.h>
    #include <Logging.h>
    #include <ESPAsyncWebServer.h>
    #include <ModbusClientRTU.h>
    #include <ModbusCache.h>
    #include <Update.h>
    #include "config.h"
    #include "debug.h"
    #include "debug_buffer.h"

    void setupPages(AsyncWebServer* server, ModbusCache *modbusCache, Config *config, AsyncWiFiManager *wm);
    void sendResponseHeader(AsyncResponseStream *response, const char *title, bool inlineStyle = false, const String &hostname = "");
    void sendResponseTrailer(AsyncResponseStream *response);
    void sendButton(AsyncResponseStream *response, const char *title, const char *action, const char *css = "");
    void sendTableRow(AsyncResponseStream *response, const char *name, uint32_t value);
    void sendTableRow(AsyncResponseStream *response, const char *name, String value);
    void sendDebugForm(AsyncResponseStream *response, String slaveId, String reg, String function, String count);
    void sendMinCss(AsyncResponseStream *response);
    void sendLogPage(AsyncResponseStream *response, const String &hostname);
    const String ErrorName(Modbus::Error code);
    const String WiFiQuality(int rssiValue);
    
    // External flag to track config portal mode
    extern bool inConfigPortal;
#endif /* PAGES_H */