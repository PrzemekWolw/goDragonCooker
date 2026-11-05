#pragma once

#include "imgui.h"

constexpr ImVec4 colBg(0.09f, 0.12f, 0.16f, 1.0f);
constexpr ImVec4 colBar(0.13f, 0.15f, 0.20f, 1.0f);
constexpr ImVec4 colText(0.80f, 0.80f, 0.83f, 1.0f);
constexpr ImVec4 colTextDim(0.50f, 0.54f, 0.60f, 1.0f);
constexpr ImVec4 colCookOk(60 / 255.0f, 179 / 255.0f, 113 / 255.0f, 1.0f);
constexpr ImVec4 colCookErr(0.76f, 0.28f, 0.28f, 1.0f);
constexpr ImVec4 colWarn(0.86f, 0.66f, 0.26f, 1.0f);

extern float uiScale;
// Scales a design-size pixel value by uiScale, rounded to the nearest pixel.
float px(float value);
void applyTheme(float scale);
void loadFont();
