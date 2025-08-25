#ifndef WIFI_UTILS_H
#define WIFI_UTILS_H

/**
 * Attempts to reset the WiFi connection by disconnecting and reconnecting
 * to the configured SSID.
 * 
 * @return true if successfully reconnected, false otherwise
 */
bool forceWiFiReset();

/**
 * Checks if the WiFi is actually disconnected by verifying multiple times.
 * 
 * @return true if WiFi is disconnected, false otherwise
 */
bool isWiFiActuallyDisconnected();

#endif // WIFI_UTILS_H 