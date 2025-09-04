#include "config.h"
#include <WiFi.h>

Config::Config()
    :_prefs(NULL)
    ,_tcpPort(502)
    ,_tcpPort2(502)
    ,_tcpPort3(10502)
    ,_targetIP("127.0.0.1")
    ,_tcpTimeout(10000)
    ,_modbusBaudRate(9600)
    ,_modbusConfig(SERIAL_8N1)
    ,_modbusRtsPin(-1)
    ,_modbusBaudRate2(9600)
    ,_modbusConfig2(SERIAL_8N1)
    ,_modbusRtsPin2(-1)
    ,_serialBaudRate(115200)
    ,_serialConfig(SERIAL_8N1)
    ,_clientIsRTU(true)
    ,_pollingInterval(500)
    ,_staticIP("0.0.0.0")
    ,_staticGateway("0.0.0.0")
    ,_staticSubnet("255.255.255.0")
    ,_useStaticIP(false)
{}

void Config::begin(Preferences *prefs)
{
    _prefs = prefs;
    _tcpPort = _prefs->getUShort("tcpPort", _tcpPort);
    _tcpPort2 = _prefs->getUShort("tcpPort2", _tcpPort2);
    _tcpPort3 = _prefs->getUShort("tcpPort3", _tcpPort3);
    _targetIP = _prefs->getString("targetIP", _targetIP);
    _tcpTimeout = _prefs->getULong("tcpTimeout", _tcpTimeout);
    _modbusBaudRate = _prefs->getULong("modbusBaudRate", _modbusBaudRate);
    _modbusConfig = _prefs->getULong("modbusConfig", _modbusConfig);
    _modbusRtsPin = _prefs->getChar("modbusRtsPin", _modbusRtsPin);
    _modbusBaudRate2 = _prefs->getULong("modbusBaudRate2", _modbusBaudRate2);
    _modbusConfig2 = _prefs->getULong("modbusConfig2", _modbusConfig2);
    _modbusRtsPin2 = _prefs->getChar("modbusRtsPin2", _modbusRtsPin2);
    _serialBaudRate = _prefs->getULong("serialBaudRate", _serialBaudRate);
    _serialConfig = _prefs->getULong("serialConfig", _serialConfig);
    _clientIsRTU = _prefs->getBool("clientIsRTU", _clientIsRTU);
    _pollingInterval = _prefs->getULong("pollingInterval", _pollingInterval);
    _staticIP = _prefs->getString("staticIP", _staticIP);
    _staticGateway = _prefs->getString("staticGateway", _staticGateway);
    _staticSubnet = _prefs->getString("staticSubnet", _staticSubnet);
    _useStaticIP = _prefs->getBool("useStaticIP", _useStaticIP);
    if (_prefs->isKey("hostname")) {
        _hostname = _prefs->getString("hostname", "");  // Use stored hostname if present
    } else {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        _hostname = "esp32-" + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);  // Generate hostname from MAC
    }
}

uint16_t Config::getTcpPort(){
    return _tcpPort;
}

uint16_t Config::getTcpPort2(){
    return _tcpPort2;
}

uint16_t Config::getTcpPort3(){
    return _tcpPort3;
}

void Config::setTcpPort(uint16_t value){
    if (_tcpPort == value) return;
    _tcpPort = value;
    _prefs->putUShort("tcpPort", _tcpPort);
}

void Config::setTcpPort2(uint16_t value){
    if (_tcpPort2 == value) return;
    _tcpPort2 = value;
    _prefs->putUShort("tcpPort2", _tcpPort2);
}

void Config::setTcpPort3(uint16_t value){
    if (_tcpPort3 == value) return;
    _tcpPort3 = value;
    _prefs->putUShort("tcpPort3", _tcpPort3);
}

String Config::getTargetIP() const {
    return _targetIP;
}

void Config::setTargetIP(const String& ip) {
    if (_targetIP == ip) return;
    _targetIP = ip;
    _prefs->putString("targetIP", _targetIP);
}


uint32_t Config::getTcpTimeout(){
    return _tcpTimeout;
}

void Config::setTcpTimeout(uint32_t value){
    if (_tcpTimeout == value) return;
    _tcpTimeout = value;
    _prefs->putULong("tcpTimeout", _tcpTimeout);
}

uint32_t Config::getModbusConfig(){
    return _modbusConfig;
}

unsigned long Config::getModbusBaudRate(){
    return _modbusBaudRate;
}

void Config::setModbusBaudRate(unsigned long value){
    if (_modbusBaudRate == value) return;
    _modbusBaudRate = value;
    _prefs->putULong("modbusBaudRate", _modbusBaudRate);
}

uint8_t Config::getModbusDataBits(){
    return ((_modbusConfig & 0xc) >> 2) + 5;
}

void Config::setModbusDataBits(uint8_t value){
    auto dataBits = getModbusDataBits();
    value -= 5;
    value = (value << 2) & 0xc;
    if (value == dataBits) return;
    _modbusConfig = (_modbusConfig & 0xfffffff3) | value;
    _prefs->putULong("modbusConfig", _modbusConfig);
}

uint8_t Config::getModbusParity(){
    return _modbusConfig & 0x3;
}

void Config::setModbusParity(uint8_t value){
    auto parity = getModbusParity();
    value = value & 0x3;
    if (parity == value) return;
    _modbusConfig = (_modbusConfig & 0xfffffffc) | value;
    _prefs->putULong("modbusConfig", _modbusConfig);
}

uint8_t Config::getModbusStopBits(){
    return (_modbusConfig & 0x30) >> 4;
}

void Config::setModbusStopBits(uint8_t value){
    auto stopbits = getModbusStopBits();
    value = (value << 4) & 0x30;
    if (stopbits == value) return;
    _modbusConfig = (_modbusConfig & 0xffffffcf) | value;
    _prefs->putULong("modbusConfig", _modbusConfig);
}

int8_t Config::getModbusRtsPin(){
    return _modbusRtsPin;
}

void Config::setModbusRtsPin(int8_t value){
    if (_modbusRtsPin == value) return;
    _modbusRtsPin = value;
    _prefs->putChar("modbusRtsPin", _modbusRtsPin);
}

uint32_t Config::getModbusConfig2(){
    return _modbusConfig2;
}

unsigned long Config::getModbusBaudRate2(){
    return _modbusBaudRate2;
}

void Config::setModbusBaudRate2(unsigned long value){
    if (_modbusBaudRate2 == value) return;
    _modbusBaudRate2 = value;
    _prefs->putULong("modbusBaudRate2", _modbusBaudRate2);
}

uint8_t Config::getModbusDataBits2(){
    return ((_modbusConfig2 & 0xc) >> 2) + 5;
}

void Config::setModbusDataBits2(uint8_t value){
    auto dataBits = getModbusDataBits2();
    value -= 5;
    value = (value << 2) & 0xc;
    if (value == dataBits) return;
    _modbusConfig2 = (_modbusConfig2 & 0xfffffff3) | value;
    _prefs->putULong("modbusConfig2", _modbusConfig2);
}

uint8_t Config::getModbusParity2(){
    return _modbusConfig2 & 0x3;
}

void Config::setModbusParity2(uint8_t value){
    auto parity = getModbusParity2();
    value = value & 0x3;
    if (parity == value) return;
    _modbusConfig2 = (_modbusConfig2 & 0xfffffffc) | value;
    _prefs->putULong("modbusConfig2", _modbusConfig2);
}

uint8_t Config::getModbusStopBits2(){
    return (_modbusConfig2 & 0x30) >> 4;
}

void Config::setModbusStopBits2(uint8_t value){
    auto stopbits = getModbusStopBits2();
    value = (value << 4) & 0x30;
    if (stopbits == value) return;
    _modbusConfig2 = (_modbusConfig2 & 0xffffffcf) | value;
    _prefs->putULong("modbusConfig2", _modbusConfig2);
}

int8_t Config::getModbusRtsPin2(){
    return _modbusRtsPin2;
}

void Config::setModbusRtsPin2(int8_t value){
    if (_modbusRtsPin2 == value) return;
    _modbusRtsPin2 = value;
    _prefs->putChar("modbusRtsPin2", _modbusRtsPin2);
}

uint32_t Config::getSerialConfig(){
    return _serialConfig;
}

unsigned long Config::getSerialBaudRate(){
    return _serialBaudRate;
}

void Config::setSerialBaudRate(unsigned long value){
    if (_serialBaudRate == value) return;
    _serialBaudRate = value;
    _prefs->putULong("serialBaudRate", _serialBaudRate);
}

uint8_t Config::getSerialDataBits(){
    return ((_serialConfig & 0xc) >> 2) + 5;
}

void Config::setSerialDataBits(uint8_t value){
    auto dataBits = getSerialDataBits();
    value -= 5;
    value = (value << 2) & 0xc;
    if (value == dataBits) return;
    _serialConfig = (_serialConfig & 0xfffffff3) | value;
    _prefs->putULong("serialConfig", _serialConfig);
}

uint8_t Config::getSerialParity(){
    return _serialConfig & 0x3;
}

void Config::setSerialParity(uint8_t value){
    auto parity = getSerialParity();
    value = value & 0x3;
    if (parity == value) return;
    _serialConfig = (_serialConfig & 0xfffffffc) | value;
    _prefs->putULong("serialConfig", _serialConfig);
}

uint8_t Config::getSerialStopBits(){
    return (_serialConfig & 0x30) >> 4;
}

void Config::setSerialStopBits(uint8_t value){
    auto stopbits = getSerialStopBits();
    value = (value << 4) & 0x30;
    if (stopbits == value) return;
    _serialConfig = (_serialConfig & 0xffffffcf) | value;
    _prefs->putULong("serialConfig", _serialConfig);
}

void Config::setClientIsRTU(bool value){
    if (_clientIsRTU == value) return;
    _clientIsRTU = value;
    _prefs->putBool("clientIsRTU", _clientIsRTU);
}

bool Config::getClientIsRTU(){
    return _clientIsRTU;
}

unsigned long Config::getPollingInterval(){
    return _pollingInterval;
}

void Config::setPollingInterval(unsigned long value){
    if (_pollingInterval == value) return;
    _pollingInterval = value;
    _prefs->putULong("pollingInterval", _pollingInterval);
}


String Config::getHostname() const {
    return _hostname;
}

void Config::setHostname(const String& hostname) {
    _hostname = hostname;
    _prefs->putString("hostname", _hostname); // Save hostname to preferences
}

void Config::setStaticIP(const String& ip) {
    if (_staticIP == ip) return;
    _staticIP = ip;
    _prefs->putString("staticIP", _staticIP);
}

String Config::getStaticIP() const {
    return _staticIP;
}

void Config::setStaticGateway(const String& gateway) {
    if (_staticGateway == gateway) return;
    _staticGateway = gateway;
    _prefs->putString("staticGateway", _staticGateway);
}

String Config::getStaticGateway() const {
    return _staticGateway;
}

void Config::setStaticSubnet(const String& subnet) {
    if (_staticSubnet == subnet) return;
    _staticSubnet = subnet;
    _prefs->putString("staticSubnet", _staticSubnet);
}

String Config::getStaticSubnet() const {
    return _staticSubnet;
}

void Config::setUseStaticIP(bool useStatic) {
    if (_useStaticIP == useStatic) return;
    _useStaticIP = useStatic;
    _prefs->putBool("useStaticIP", _useStaticIP);
}

bool Config::getUseStaticIP() const {
    return _useStaticIP;
}
