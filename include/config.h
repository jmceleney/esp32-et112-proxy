#ifndef CONFIG_H
    #define CONFIG_H
    #include <Arduino.h>
    #include <Preferences.h>
    #define debugSerial Serial
    #define modbusServerSerial Serial1
    #define modbusClientSerial Serial2
    // Include the WiFi library
    #define DEBUG

    class Config{
        private:
            Preferences *_prefs;
            int16_t _tcpPort;
            int16_t _tcpPort2;
            int16_t _tcpPort3;
            String _targetIP;
            uint32_t _tcpTimeout;
            unsigned long _modbusBaudRate;
            uint32_t _modbusConfig;
            int8_t _modbusRtsPin;
            unsigned long _modbusBaudRate2;
            uint32_t _modbusConfig2;
            int8_t _modbusRtsPin2;
            unsigned long _serialBaudRate;
            uint32_t _serialConfig;
            bool _clientIsRTU;
        public:
            Config();
            void begin(Preferences *prefs);
            uint16_t getTcpPort();
            void setTcpPort(uint16_t value);
            uint16_t getTcpPort2();
            void setTcpPort2(uint16_t value);
            uint16_t getTcpPort3();
            void setTcpPort3(uint16_t value);
            uint32_t getTcpTimeout();
            void setTcpTimeout(uint32_t value);
            String getTargetIP() const;
            void setTargetIP(const String& ip);
            uint32_t getModbusConfig();
            unsigned long getModbusBaudRate();
            void setModbusBaudRate(unsigned long value);
            uint8_t getModbusDataBits();
            void setModbusDataBits(uint8_t value);
            uint8_t getModbusParity();
            void setModbusParity(uint8_t value);
            uint8_t getModbusStopBits();
            void setModbusStopBits(uint8_t value);
            int8_t getModbusRtsPin();
            void setModbusRtsPin(int8_t value);
            uint32_t getModbusConfig2();
            unsigned long getModbusBaudRate2();
            void setModbusBaudRate2(unsigned long value);
            uint8_t getModbusDataBits2();
            void setModbusDataBits2(uint8_t value);
            uint8_t getModbusParity2();
            void setModbusParity2(uint8_t value);
            uint8_t getModbusStopBits2();
            void setModbusStopBits2(uint8_t value);
            int8_t getModbusRtsPin2();
            void setModbusRtsPin2(int8_t value);
            uint32_t getSerialConfig();
            unsigned long getSerialBaudRate();
            void setSerialBaudRate(unsigned long value);
            uint8_t getSerialDataBits();
            void setSerialDataBits(uint8_t value);
            uint8_t getSerialParity();
            void setSerialParity(uint8_t value);
            uint8_t getSerialStopBits();
            void setSerialStopBits(uint8_t value);
            bool getClientIsRTU();
            void setClientIsRTU(bool value);
    };
    #ifdef DEBUG
    #define dbg(x...) debugSerial.print(x);
    #define dbgln(x...) debugSerial.println(x);
    #else /* DEBUG */
    #define dbg(x...) ;
    #define dbgln(x...) ;
    #endif /* DEBUG */
#endif /* CONFIG_H */