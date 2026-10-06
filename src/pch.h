#pragma once
// Precompiled into every source file (CMakeLists.txt). No Winsock: only the network code wants it.
#include <windows.h>
// Direct3D 7 and Direct3D 9 (namespace d9), the one way they're included anywhere.
#include "ddraw9/d3d9_api.h"
// rpcndr.h's (through objbase.h); `small` is a name here.
#undef small

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
