#ifndef HEADER_A3F8E2D1B4C95F6E7A8B9C0D1E2F3G4H // -*- mode:c++ -*-
#define HEADER_A3F8E2D1B4C95F6E7A8B9C0D1E2F3G4H

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include <string>
#include <shared/enum_decl.h>
#include "FujiNetConfig.inl"
#include <shared/enum_end.h>

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// B2-specific FujiNet configuration

struct B2FujiNetConfig {
    // Interface type (Serial ACIA or User Port)
    Enum<FujiNetInterfaceType> interface_type{FujiNetInterfaceType_Serial};
    
    // Device mode (PTY for virtual/testing, or Hardware for real serial)
    Enum<FujiNetDeviceMode> device_mode{FujiNetDeviceMode_PTY};
    
    // Path to device (PTY path like /tmp/fn-tty or serial device like /dev/ttyUSB0)
    std::string device_path;
    
    // Auto-connect on emulator startup
    bool auto_connect = false;
    
    // Enable debug logging
    bool debug = false;
};

// JSON serialization support
#include "json.h"
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(B2FujiNetConfig, interface_type, device_mode, device_path, auto_connect, debug);

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif

