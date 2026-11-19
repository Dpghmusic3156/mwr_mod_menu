#pragma once

#include <cstdint>

bool ExtTouch_Init(int width, int height, uint32_t orientation);
void ExtTouch_UpdateScreen(int width, int height, uint32_t orientation);
void ExtTouch_FeedImGui();
void ExtTouch_Close();
