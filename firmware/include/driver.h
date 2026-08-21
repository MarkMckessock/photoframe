// Seeed_GFX board/panel selection.
//
// This file MUST be named driver.h and MUST be on the include path: the library
// (Seeed_Arduino_LCD) looks for it by name from User_Setups/Dynamic_Setup.h.
//
// Combo 510 pulls in Setup510_Seeed_XIAO_EPaper_13inch3_colorful.h, which defines
// T133A01_DRIVER, EPAPER_ENABLE, TFT_WIDTH 1200, TFT_HEIGHT 1600, and -- because
// ENABLE_EPAPER_BOARD_PIN_SETUPS is set -- takes the EE02 branch of
// User_Setups/EPaper_Board_Pins_Setups.h for the pin map. Do not define the pins
// yourself; the fallback branch inside Setup510 is wrong for this board.
#pragma once

#define BOARD_SCREEN_COMBO 510  // 13.3 inch six-colour ePaper screen (T133A01)
#define USE_XIAO_EPAPER_DISPLAY_BOARD_EE02
