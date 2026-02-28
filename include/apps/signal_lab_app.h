/**
 * @file signal_lab_app.h
 * @brief SignalLab – Protocol Analyzer & Waterfall for 433 MHz RF.
 *
 * Provides three analysis modes for the RF 433 MHz transceiver:
 *  - Waterfall Visualizer with scrolling intensity cascade
 *  - Protocol Decoder for Princeton, EV1527, HT6P20B
 *  - Pulse Width Analysis showing Sync / High / Low durations
 */

#pragma once

#include "apps/app_base.h"

AppBase *createSignalLabApp();
