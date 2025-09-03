#include "system_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Static variables for tracking CPU usage over time
static uint32_t lastTotalRunTime = 0;
static uint32_t lastIdleRunTime[2] = {0, 0};
static uint64_t lastMeasurementTime = 0;

CPULoadInfo getCPULoad() {
    CPULoadInfo loadInfo;
    loadInfo.isValid = true;
    
    // Measure CPU load using zero-delay timing method
    // This works by measuring time away from task using vTaskDelay(0)
    
    // Sample core 0 load (if running on core 0, measures this core)
    int64_t start = esp_timer_get_time();
    vTaskDelay(0 / portTICK_RATE_MS);  // Yield to scheduler
    int64_t end = esp_timer_get_time();
    
    // Calculate load as percentage - higher delay = higher CPU usage by other tasks
    // We sample over a short period and estimate load based on scheduler delay
    int64_t delayMicros = end - start;
    
    // Normalize delay to percentage (typical idle delay is very small ~10-50us)
    // Under load, delays can be much higher (hundreds to thousands of microseconds)
    // Scale appropriately: delays >1000us indicate high load
    float load0 = (float)delayMicros / 10.0f; // Scale factor for percentage
    if (load0 > 100.0f) load0 = 100.0f;
    if (load0 < 0.0f) load0 = 0.0f;
    
    loadInfo.core0Load = load0;
    
    // For dual core estimation, we approximate core 1 load
    // This is a simplified approach - more accurate would require core-pinned tasks
    // For now, provide a reasonable estimate based on overall system load
    loadInfo.core1Load = load0 * 0.8f; // Approximate as 80% of measured load
    
    return loadInfo;
}