/**
 * @file hackos.h
 * @brief HackOS SDK – the ONLY include an application needs.
 *
 * This header provides the complete public API surface for user-space
 * application development.  It exposes:
 *
 *  - **HackOSApp** – the mandatory base class with the four lifecycle
 *    phases (on_alloc, on_start, on_event, on_free).
 *
 *  - **AppContext** – the sandboxed resource tracker (memory, observers,
 *    timers).
 *
 *  - **GUI** – Canvas, View, ViewDispatcher, SceneManager, and the
 *    built-in widget library (StatusBar, Menu, ProgressBar, Dialog).
 *
 *  - **Storage** – VirtualFS for transparent SD/Flash file access.
 *
 *  - **Radio** – RadioManager, SignalRecord, and the ProtocolRegistry
 *    for capture/replay of IR, RF 433 MHz, and NFC signals.
 *
 *  - **Events** – EventSystem, Event types and constants.
 *
 * @code
 * #include "hackos.h"
 *
 * class MyApp : public hackos::HackOSApp {
 * protected:
 *     void on_alloc() override { // ctx().alloc()
 *     }
 *     void on_start() override { // set up views
 *     }
 *     void on_event(Event *e) override { // handle input
 *     }
 *     void on_free()  override { // release resources
 *     }
 * };
 * @endcode
 */

#pragma once

// ── App lifecycle & sandboxing ───────────────────────────────────────────────
#include "apps/hackos_app.h"
#include "apps/app_context.h"

// ── Core event system ────────────────────────────────────────────────────────
#include "core/event.h"
#include "core/event_system.h"

// ── GUI subsystem ────────────────────────────────────────────────────────────
#include "ui/canvas.h"
#include "ui/view.h"
#include "ui/view_dispatcher.h"
#include "ui/scene_manager.h"
#include "ui/gui.h"
#include "ui/widgets.h"

// ── Storage subsystem ────────────────────────────────────────────────────────
#include "storage/vfs.h"

// ── Radio subsystem ──────────────────────────────────────────────────────────
#include "hardware/radio/radio_manager.h"
#include "hardware/radio/radio_protocol.h"
#include "hardware/radio/signal_format.h"

// ── Hardware helpers (display, input) ────────────────────────────────────────
#include "hardware/display.h"
#include "hardware/input.h"
