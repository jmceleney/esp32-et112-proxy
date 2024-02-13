#ifndef MODBUS_RTU_WRAPPER_H
#define MODBUS_RTU_WRAPPER_H

// Undefine DEFAULTTIMEOUT if previously defined
#ifdef DEFAULTTIMEOUT
#undef DEFAULTTIMEOUT
#endif

// Define DEFAULTTIMEOUT specifically for RTU usage
#define DEFAULTTIMEOUT 2000

// Include the Modbus RTU client
#include "ModbusClientRTU.h"

// Additional wrapper functionality can go here

#endif // MODBUS_RTU_WRAPPER_H
