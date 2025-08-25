#ifndef DEBUG_BUFFER_H
#define DEBUG_BUFFER_H

#include <Arduino.h>
#include <mutex>

// Increased from 8KB to 32KB ring buffer
// ESP32 has about 520KB of SRAM, so 32KB is a reasonable size (about 6% of available RAM)
// This will hold approximately 400-800 log messages depending on their size
#define DEBUG_BUFFER_SIZE 32768

class DebugRingBuffer {
private:
    char buffer[DEBUG_BUFFER_SIZE];
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;
    std::mutex bufferMutex;
    bool overflow = false;

public:
    DebugRingBuffer();
    
    // Add a message to the ring buffer
    void add(const String& message);
    
    // Get all messages currently in the buffer
    String getAll();
    
    // Get messages since the last position
    String getNewMessages(size_t& lastPosition);
    
    // Get a specific chunk of the buffer
    String getChunk(size_t startPos, size_t endPos);
    
    // Get a small chunk of data safely (for AJAX endpoint)
    String getSafeChunk(size_t startPos, size_t maxChars, size_t& newPosition);
    
    // Get current position for tracking updates
    size_t getCurrentPosition();
    
    // Clear the buffer
    void clear();
    
    // Check if buffer has overflowed since last clear
    bool hasOverflowed();
};

// Global instance
extern DebugRingBuffer debugBuffer;

#endif // DEBUG_BUFFER_H 