#ifndef MODBUS_TCP_WRAPPER_H
#define MODBUS_TCP_WRAPPER_H

// Undefine DEFAULTTIMEOUT if previously defined
#ifdef DEFAULTTIMEOUT
#undef DEFAULTTIMEOUT
#endif

// Define DEFAULTTIMEOUT specifically for TCP usage
#define DEFAULTTIMEOUT 10000

// Include the Modbus TCP async client
#include "ModbusClientTCPasync.h"

// Additional wrapper functionality can go here

#endif // MODBUS_TCP_WRAPPER_H
