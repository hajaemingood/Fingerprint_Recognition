#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Creates the UI and processes Windows events until the window is closed.
int runComparisonWindow(HINSTANCE instance, int showCommand);
