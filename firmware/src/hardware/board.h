#pragma once

#include "../expression/display.h"

// hardware_config.h is included only by board.cpp.

bool board_init();
Display* make_display();
