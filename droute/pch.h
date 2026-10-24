#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <MinHook.h>
