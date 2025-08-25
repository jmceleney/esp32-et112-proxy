#include "debug_buffer.h"

// Global instance
DebugRingBuffer debugBuffer;

DebugRingBuffer::DebugRingBuffer() {
    memset(buffer, 0, DEBUG_BUFFER_SIZE);
}

void DebugRingBuffer::add(const String& message) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    
    // Ensure the message ends with a newline
    String formattedMessage = message;
    if (!formattedMessage.endsWith("\n")) {
        formattedMessage += "\n";
    }
    
    // Add timestamp to the message
    String timestampedMessage = "[" + String(millis() / 1000) + "s] " + formattedMessage;
    
    size_t msgLen = timestampedMessage.length();
    
    // Check if message is too large for the buffer
    if (msgLen >= DEBUG_BUFFER_SIZE) {
        // Just store the last part of the message that fits
        const char* msgStr = timestampedMessage.c_str();
        memcpy(buffer, msgStr + (msgLen - DEBUG_BUFFER_SIZE + 1), DEBUG_BUFFER_SIZE - 1);
        buffer[DEBUG_BUFFER_SIZE - 1] = '\0';
        head = 0;
        tail = DEBUG_BUFFER_SIZE - 1;
        size = DEBUG_BUFFER_SIZE - 1;
        overflow = true;
        return;
    }
    
    // Check if we need to overwrite old data
    if (size + msgLen >= DEBUG_BUFFER_SIZE) {
        // Mark that we've overflowed
        overflow = true;
        
        // For larger buffers, we can be more efficient by removing larger chunks at once
        // Calculate how much space we need to free up
        size_t spaceNeeded = (size + msgLen) - DEBUG_BUFFER_SIZE + 1024; // Add some extra margin
        
        // Remove old data to make room
        while (size > 0 && spaceNeeded > 0) {
            // Find the next newline to remove a complete line
            size_t i = tail;
            size_t lineLength = 0;
            
            while (i != head && buffer[i] != '\n') {
                i = (i + 1) % DEBUG_BUFFER_SIZE;
                lineLength++;
            }
            
            // Include the newline character
            if (i != head) {
                lineLength++; // Count the newline
                i = (i + 1) % DEBUG_BUFFER_SIZE;
            }
            
            // Update tail and size
            tail = i;
            size -= lineLength;
            spaceNeeded -= lineLength;
        }
    }
    
    // Add the new message - use memcpy for efficiency with larger messages
    if (msgLen > 20) {
        // If the message wraps around the buffer, we need to do it in two parts
        const char* msgStr = timestampedMessage.c_str();
        size_t firstPart = DEBUG_BUFFER_SIZE - head;
        if (msgLen <= firstPart) {
            // Message fits before the end of the buffer
            memcpy(buffer + head, msgStr, msgLen);
            head = (head + msgLen) % DEBUG_BUFFER_SIZE;
            size += msgLen;
        } else {
            // Message wraps around the buffer
            memcpy(buffer + head, msgStr, firstPart);
            memcpy(buffer, msgStr + firstPart, msgLen - firstPart);
            head = msgLen - firstPart;
            size += msgLen;
        }
    } else {
        // For small messages, use the original approach
        for (size_t i = 0; i < msgLen; i++) {
            buffer[head] = timestampedMessage[i];
            head = (head + 1) % DEBUG_BUFFER_SIZE;
            size++;
        }
    }
}

String DebugRingBuffer::getAll() {
    std::lock_guard<std::mutex> lock(bufferMutex);
    
    // Allow other tasks to run
    yield();
    
    String result;
    result.reserve(size + 1);
    
    size_t current = tail;
    for (size_t i = 0; i < size; i++) {
        result += buffer[current];
        current = (current + 1) % DEBUG_BUFFER_SIZE;
        
        // Allow other tasks to run periodically during large transfers
        if (i > 0 && i % 500 == 0) {
            yield();
        }
    }
    
    return result;
}

String DebugRingBuffer::getNewMessages(size_t& lastPosition) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    
    // Allow other tasks to run
    yield();
    
    // If lastPosition is out of bounds or we've overflowed, return all messages
    if (lastPosition >= DEBUG_BUFFER_SIZE || overflow) {
        lastPosition = (head == 0) ? DEBUG_BUFFER_SIZE - 1 : head - 1;
        overflow = false;
        return getAll();
    }
    
    // Calculate how many characters to read
    size_t current = (lastPosition + 1) % DEBUG_BUFFER_SIZE;
    size_t count = 0;
    
    if (current <= head) {
        count = head - current;
    } else {
        count = (DEBUG_BUFFER_SIZE - current) + head;
    }
    
    if (count == 0) {
        return "";
    }
    
    // Allow other tasks to run again if we have a lot of data
    if (count > 1000) {
        yield();
    }
    
    String result;
    result.reserve(count + 1);
    
    for (size_t i = 0; i < count; i++) {
        result += buffer[current];
        current = (current + 1) % DEBUG_BUFFER_SIZE;
        
        // Allow other tasks to run periodically during large transfers
        if (i > 0 && i % 500 == 0) {
            yield();
        }
    }
    
    lastPosition = (head == 0) ? DEBUG_BUFFER_SIZE - 1 : head - 1;
    return result;
}

size_t DebugRingBuffer::getCurrentPosition() {
    std::lock_guard<std::mutex> lock(bufferMutex);
    return (head == 0) ? DEBUG_BUFFER_SIZE - 1 : head - 1;
}

void DebugRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(bufferMutex);
    head = 0;
    tail = 0;
    size = 0;
    overflow = false;
    memset(buffer, 0, DEBUG_BUFFER_SIZE);
}

bool DebugRingBuffer::hasOverflowed() {
    std::lock_guard<std::mutex> lock(bufferMutex);
    return overflow;
}

String DebugRingBuffer::getChunk(size_t startPos, size_t endPos) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    
    // Allow other tasks to run
    yield();
    
    // Validate positions
    if (startPos >= DEBUG_BUFFER_SIZE || endPos >= DEBUG_BUFFER_SIZE || startPos >= endPos) {
        return "";
    }
    
    // Calculate how many characters to read
    size_t count = endPos - startPos;
    
    // Limit to buffer size
    if (count > size) {
        count = size;
    }
    
    // Allow other tasks to run again if we have a lot of data
    if (count > 1000) {
        yield();
    }
    
    String result;
    result.reserve(count + 1);
    
    size_t current = startPos;
    for (size_t i = 0; i < count; i++) {
        result += buffer[current];
        current = (current + 1) % DEBUG_BUFFER_SIZE;
        
        // Stop if we reach the head
        if (current == head) {
            break;
        }
        
        // Allow other tasks to run periodically during large transfers
        if (i > 0 && i % 500 == 0) {
            yield();
        }
    }
    
    return result;
}

// Ultra lightweight method to get a small chunk of data safely
String DebugRingBuffer::getSafeChunk(size_t startPos, size_t maxChars, size_t& newPosition) {
    // Use a very short lock to minimize blocking
    std::lock_guard<std::mutex> lock(bufferMutex);
    
    // Validate position
    if (startPos >= DEBUG_BUFFER_SIZE || size == 0) {
        newPosition = (head == 0) ? DEBUG_BUFFER_SIZE - 1 : head - 1;
        return "";
    }
    
    // Calculate the current position
    size_t current = (startPos + 1) % DEBUG_BUFFER_SIZE;
    
    // Check if we're already at the head
    if (current == head) {
        newPosition = (head == 0) ? DEBUG_BUFFER_SIZE - 1 : head - 1;
        return "";
    }
    
    // Calculate how many characters we can read
    size_t availableChars = 0;
    
    if (current < head) {
        availableChars = head - current;
    } else {
        availableChars = (DEBUG_BUFFER_SIZE - current) + head;
    }
    
    // Limit to maxChars
    size_t charsToRead = (availableChars > maxChars) ? maxChars : availableChars;
    
    // Prepare result string with exact capacity
    String result;
    result.reserve(charsToRead + 1);
    
    // Track the original starting position for position calculations
    size_t originalCurrent = current;
    size_t charsRead = 0;
    
    // For larger chunks, use a more efficient approach
    if (charsToRead > 1000) {
        // If the data doesn't wrap around the buffer, we can use a single memcpy
        if (current + charsToRead <= DEBUG_BUFFER_SIZE) {
            // Create a temporary buffer
            char* tempBuffer = new char[charsToRead + 1];
            memcpy(tempBuffer, buffer + current, charsToRead);
            tempBuffer[charsToRead] = '\0';
            
            // Create the string from the buffer
            result = String(tempBuffer);
            delete[] tempBuffer;
        } else {
            // Data wraps around the buffer, need two memcpy operations
            size_t firstPartSize = DEBUG_BUFFER_SIZE - current;
            size_t secondPartSize = charsToRead - firstPartSize;
            
            // Create a temporary buffer
            char* tempBuffer = new char[charsToRead + 1];
            memcpy(tempBuffer, buffer + current, firstPartSize);
            memcpy(tempBuffer + firstPartSize, buffer, secondPartSize);
            tempBuffer[charsToRead] = '\0';
            
            // Create the string from the buffer
            result = String(tempBuffer);
            delete[] tempBuffer;
        }
        
        charsRead = result.length();
        
        // Find the last newline character to ensure we end on a complete line
        int lastNewlinePos = result.lastIndexOf('\n');
        if (lastNewlinePos >= 0 && lastNewlinePos < result.length() - 1) {
            // Truncate to the last complete line
            result = result.substring(0, lastNewlinePos + 1);
            // Adjust chars read
            charsRead = lastNewlinePos + 1;
        }
    } else {
        // For smaller chunks, use the original approach
        size_t i;
        for (i = 0; i < charsToRead; i++) {
            char c = buffer[current];
            result += c;
            current = (current + 1) % DEBUG_BUFFER_SIZE;
            charsRead++;
            
            // Stop if we reach the head
            if (current == head) {
                break;
            }
        }
        
        // If we didn't read the full chunk and didn't end on a newline, find the last newline
        if (i == charsToRead && result.length() > 0 && result[result.length() - 1] != '\n') {
            int lastNewlinePos = result.lastIndexOf('\n');
            if (lastNewlinePos >= 0) {
                // Truncate to the last complete line
                result = result.substring(0, lastNewlinePos + 1);
                // Adjust chars read
                charsRead = lastNewlinePos + 1;
            }
        }
    }
    
    // Calculate the new position based on the original position and chars read
    current = (originalCurrent + charsRead) % DEBUG_BUFFER_SIZE;
    if (current == 0) current = DEBUG_BUFFER_SIZE - 1;
    else current--;
    
    newPosition = current;
    
    // Reset overflow flag after reading
    overflow = false;
    
    return result;
} 