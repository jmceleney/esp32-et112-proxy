#ifndef SYSTEM_UTILS_H
#define SYSTEM_UTILS_H

#include <Arduino.h>

/**
 * Structure to hold CPU load information for both cores
 */
struct CPULoadInfo {
    float core0Load;  // CPU load percentage for core 0 (0-100)
    float core1Load;  // CPU load percentage for core 1 (0-100)
    bool isValid;     // Whether the data is valid
};

/**
 * Gets the current CPU load percentage for both ESP32 cores using idle time measurement
 * with 2-second rolling average. Automatically initializes idle tracking tasks on first call.
 * 
 * @return CPULoadInfo structure containing averaged load percentages for both cores
 */
CPULoadInfo getCPULoad();

#endif // SYSTEM_UTILS_H