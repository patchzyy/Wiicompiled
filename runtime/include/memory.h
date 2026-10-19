#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "guest_abi/memory.h"

// The translated-code access layer is kept separately from the public memory API.
#include "memory_access.h"
