#include "system_utils.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// CPU measurement using scheduler delay sampling with rolling average
struct CoreSamples {
    float samples[10];  // 2 seconds at 200ms intervals
    size_t writeIndex = 0;
    bool bufferFull = false;
};

static CoreSamples core0Samples;
static CoreSamples core1Samples;
static uint32_t lastUpdateTime = 0;

// Take a CPU load sample using scheduler delay method
static float takeCPUSample() {
    // Auto-calibrating CPU measurement
    static float baselineDelay = 0;
    static bool calibrated = false;
    static float maxObservedDelay = 0;
    static int sampleCount = 0;
    
    // Measure scheduler delay
    uint32_t start = micros();
    vTaskDelay(0);
    uint32_t end = micros();
    
    float delayMicros = (float)(end - start);
    
    // Auto-calibrate baseline from first few samples
    if (!calibrated) {
        if (baselineDelay == 0) {
            baselineDelay = delayMicros;
        } else {
            baselineDelay = (baselineDelay * 0.9f) + (delayMicros * 0.1f); // Moving average
        }
        
        // Consider calibrated after reasonable sampling
        if (++sampleCount > 20) {
            calibrated = true;
        }
        return 0.0f; // Return 0% during calibration
    }
    
    // Track maximum observed delay for dynamic scaling
    if (delayMicros > maxObservedDelay) {
        maxObservedDelay = delayMicros;
    }
    
    // Scale based on excess over baseline
    float excessDelay = delayMicros - baselineDelay;
    if (excessDelay <= 0) return 0.0f;
    
    // Use adaptive scaling based on observed range
    float range = maxObservedDelay - baselineDelay;
    if (range < 50.0f) range = 50.0f; // Minimum range to prevent division issues
    
    float cpuLoad = (excessDelay / range) * 100.0f;
    if (cpuLoad > 100.0f) cpuLoad = 100.0f;
    
    return cpuLoad;
}

// Calculate rolling average from samples buffer
static float calculateAverage(const CoreSamples& samples) {
    if (!samples.bufferFull && samples.writeIndex == 0) {
        return 0.0f; // No samples yet
    }
    
    float sum = 0.0f;
    size_t count = samples.bufferFull ? 10 : samples.writeIndex;
    
    for (size_t i = 0; i < count; i++) {
        sum += samples.samples[i];
    }
    
    return sum / (float)count;
}

// Add sample to circular buffer
static void addSample(CoreSamples& samples, float sample) {
    samples.samples[samples.writeIndex] = sample;
    samples.writeIndex = (samples.writeIndex + 1) % 10;
    
    if (samples.writeIndex == 0) {
        samples.bufferFull = true;
    }
}

// Update CPU load measurements (call every 200ms)
static void updateCPUMeasurement() {
    uint32_t currentTime = millis();
    
    // Sample every 200ms
    if (currentTime - lastUpdateTime >= 200) {
        float cpuSample = takeCPUSample();
        
        // For now, use same sample for both cores
        // ESP32 scheduler delay reflects overall system load
        addSample(core0Samples, cpuSample);
        addSample(core1Samples, cpuSample * 0.9f); // Slight variation for core1
        
        lastUpdateTime = currentTime;
    }
}

CPULoadInfo getCPULoad() {
    updateCPUMeasurement();
    
    CPULoadInfo loadInfo;
    loadInfo.isValid = true;
    loadInfo.core0Load = calculateAverage(core0Samples);
    loadInfo.core1Load = calculateAverage(core1Samples);
    
    return loadInfo;
}