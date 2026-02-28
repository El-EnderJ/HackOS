/**
 * @file rf_analyzer_pro_app.h
 * @brief RFAnalyzerPro – Signal Waterfall Visualizer for 433 MHz RF analysis.
 *
 * Provides real-time raw signal visualization using a waterfall (spectrogram)
 * display.  Samples the RF data pin via optimized bit-banging and renders
 * high/low states as a scrolling bitmap on the 128×64 OLED.
 */

#pragma once

#include "apps/app_base.h"

AppBase *createRFAnalyzerProApp();
