#pragma once

#include <Arduino.h>

// Compile-time display profile.
constexpr uint8_t DISPLAY_WIDTH = 128;
constexpr uint8_t DISPLAY_HEIGHT = 64;

// Build-time switch for alternative 128x32 panels.
// Set to 1 to target 128x32 geometry.
#ifndef DISPLAY_USE_128X32
#define DISPLAY_USE_128X32 0
#endif

#if DISPLAY_USE_128X32
constexpr uint8_t DISPLAY_ACTIVE_WIDTH = 128;
constexpr uint8_t DISPLAY_ACTIVE_HEIGHT = 32;
#else
constexpr uint8_t DISPLAY_ACTIVE_WIDTH = DISPLAY_WIDTH;
constexpr uint8_t DISPLAY_ACTIVE_HEIGHT = DISPLAY_HEIGHT;
#endif

constexpr uint8_t DISPLAY_I2C_ADDR_PRIMARY = 0x3C;
constexpr uint8_t DISPLAY_I2C_ADDR_FALLBACK = 0x3D;

constexpr unsigned long DISPLAY_REFRESH_INTERVAL_MS = 500;
constexpr uint8_t DISPLAY_ROTATION_DEFAULT = 0;

// Two-tone panel color split for 128x64 yellow/blue modules.
constexpr uint8_t DISPLAY_YELLOW_Y_MIN = 0;
constexpr uint8_t DISPLAY_YELLOW_Y_MAX = 15;
constexpr uint8_t DISPLAY_BLUE_Y_MIN = 16;
constexpr uint8_t DISPLAY_BLUE_Y_MAX = 63;
