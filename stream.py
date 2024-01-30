import socket
import sys
from pymodbus.constants import Endian
from pymodbus.payload import BinaryPayloadDecoder
from pymodbus.client.sync import ModbusTcpClient
from pymodbus.framer.rtu_framer import ModbusRtuFramer
from pymodbus.factory import ClientDecoder

# Check if the correct number of arguments are provided
if len(sys.argv) != 3:
    print("Usage: script.py <IP> <PORT>")
    sys.exit(1)

# Configuration
TCP_IP = sys.argv[1]  # Get IP from command line argument
TCP_PORT = int(sys.argv[2])  # Get port from command line argument and convert to int

# Configuration
#TCP_IP = '192.168.29.156'
#TCP_IP = '192.168.28.42'
#TCP_PORT = 8899  # Replace with your port number

BUFFER_SIZE = 1024  # Adjust based on your expected message sizes

def crc16(data):
    crc = 0xFFFF
    for pos in data:
        crc ^= pos
        for _ in range(8):
            if (crc & 1) != 0:
                crc >>= 1
                crc ^= 0xA001
            else:
                crc >>= 1
    return crc

def split_frames(data):
    frames = []
    i = 0
    while i < len(data):
        # Minimum length for a Modbus RTU frame is 4 bytes (excluding CRC)
        if i + 4 > len(data):
            break  # Not enough data for a valid frame

        slave_id, function_code = data[i], data[i+1]
        if function_code == 0x03:  # Function code for reading registers
            if i + 5 <= len(data) and (data[i+2] << 8) + data[i+3] <= 0x007D:  # Max 125 registers can be requested
                # Assuming it's a request frame (fixed length of 8 bytes)
                frame_len = 8
            elif i + 2 < len(data):
                # Assuming it's a response frame (variable length)
                byte_count = data[i+2]
                frame_len = 3 + byte_count + 2  # Slave ID + Function Code + Byte Count + Data + CRC
            else:
                break  # Not enough data for a valid frame

            if i + frame_len <= len(data):
                frames.append(data[i:i+frame_len])
                i += frame_len
            else:
                break  # Not enough data for the complete frame
        else:
            i += 1  # Move to the next byte if function code doesn't match

    return frames

def decode_frames(frames):
    for frame in frames:
        decode_message(frame)
def decode_message(message):
    if len(message) < 5:  # Minimum length check
        print("Incomplete or corrupt message received")
        return

    # Extract the CRC from the message
    received_crc = message[-2] + (message[-1] << 8)

    # Calculate the CRC of the message (excluding the received CRC part)
    calculated_crc = crc16(message[:-2])

    # Compare the received CRC with the calculated CRC
    if received_crc != calculated_crc:
        print("CRC check failed")
        return

    slave_id, function_code = message[0], message[1]

    # Handling Read Holding Registers
    if function_code == 0x03:
        # Distinguish between request and response frames
        if len(message) == 8:  # Likely a request frame
            start_address = (message[2] << 8) + message[3]
            num_registers = (message[4] << 8) + message[5]
            print(f"Request Frame - Slave ID: {slave_id}, Function Code: {function_code}, Start Address: {start_address}, Number of Registers: {num_registers}")
        else:  # Likely a response frame
            byte_count = message[2]
            register_data = message[3:3+byte_count]
            registers = [((register_data[i] << 8) + register_data[i+1]) for i in range(0, len(register_data), 2)]
            print(f"Response Frame - Slave ID: {slave_id}, Function Code: {function_code}, Byte Count: {byte_count}, Register Values: {registers}")
    else:
        print(f"Unsupported Function Code: {function_code}")

def main():
    # TCP connection setup
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((TCP_IP, TCP_PORT))

    try:
        while True:
            data = sock.recv(BUFFER_SIZE)
            if data:
                #print("Raw data received:", data.hex())
                frames = split_frames(data)
                decode_frames(frames)
            else:
                break
    finally:
        sock.close()

if __name__ == '__main__':
    main()
