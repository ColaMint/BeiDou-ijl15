#pragma once

#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files
#include <windows.h>
#include <timeapi.h>
#include <strsafe.h>
#include <comdef.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

// reference additional headers your program requires here

#include <iostream>
#include "Client.h"
#include "Memory.h"
