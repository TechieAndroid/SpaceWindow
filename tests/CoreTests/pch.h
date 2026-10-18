// Precompiled header for the SpaceWindow core tests.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <objbase.h>
#include <sddl.h>
#else
#include "../../src/Core/Compat.h"
#include "../../src/Core/Posix.h"

#include <climits>
#include <cwchar>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <wil/resource.h>
#endif
