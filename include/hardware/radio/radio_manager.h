/**
 * @file radio_manager.h
 * @brief Central controller for the HackOS unified radio subsystem.
 *
 * The RadioManager orchestrates all radio hardware through the IRxTxDevice
 * interface and dispatches captured signals to the ProtocolRegistry for
 * automatic decoding.
 *
 * Operational model
 * -----------------
 *  1. **Registration** – at boot each hardware adapter (IR, RF, NFC) is
 *     registered via `registerDevice()`.
 *
 *  2. **Capture mode** – `startCapture()` configures the selected device
 *     for receiving and installs an ISR that pushes raw samples into a
 *     RingBuffer.
 *
 *  3. **Worker task** – a FreeRTOS task (`workerEntry()`) continuously
 *     drains the RingBuffer, runs every registered protocol's
 *     `tryDecode()`, and publishes a `HackOSEvent` (EVT_RADIO / EVT_IR /
 *     EVT_NFC) through the MessageBus when a signal is decoded.
 *
 *  4. **Replay** – `transmit()` takes a previously captured SignalRecord,
 *     encodes it via the matching protocol, and pushes the raw data
 *     through the device's `write()` method.
 *
 * Thread safety
 * -------------
 * The RingBuffer is single-producer (ISR) / single-consumer (worker),
 * requiring no mutex.  All other mutable state is accessed from the
 * worker task only.
 *
 * ## How to register a new 433 MHz garage door protocol
 *
 * @code
 * // ── garage_door_protocol.h ───────────────────────────────────────────
 * #include "hardware/radio/radio_protocol.h"
 *
 * class GarageDoorProtocol : public hackos::radio::RadioProtocol
 * {
 * public:
 *     const char *name() const override { return "GARAGE_EV1527"; }
 *     hackos::radio::Modulation modulation() const override
 *     {
 *         return hackos::radio::Modulation::OOK;
 *     }
 *
 *     bool tryDecode(const int32_t *rawTimings,
 *                    size_t count,
 *                    hackos::radio::SignalRecord &record) override
 *     {
 *         // 1. Search for your proprietary sync preamble.
 *         // 2. Decode bits according to the encoder timing.
 *         // 3. Fill record.frequencyHz, .protocolName, .bitCount,
 *         //    .decodedValue, and optionally copy rawTimings.
 *         // 4. Return true if matched.
 *         return false;
 *     }
 *
 *     size_t encode(const hackos::radio::SignalRecord &record,
 *                   int32_t *timings, size_t max) override
 *     {
 *         // Reverse the decode: turn decodedValue + bitCount back into
 *         // pulse/gap timings.
 *         return 0;
 *     }
 * };
 *
 * // ── main.cpp (during setup) ─────────────────────────────────────────
 * static GarageDoorProtocol garageDoorProto;
 * hackos::radio::ProtocolRegistry::registerProtocol(&garageDoorProto);
 * @endcode
 *
 * After registration the RadioManager worker automatically tries the
 * new protocol against every captured OOK frame.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "irxtx_device.h"
#include "ring_buffer.h"
#include "signal_format.h"

namespace hackos::radio {

// ── RadioManager ─────────────────────────────────────────────────────────────

/**
 * @brief Singleton manager for the unified radio capture/replay pipeline.
 */
class RadioManager
{
public:
    static RadioManager &instance();

    // ── Device registration ──────────────────────────────────────────────

    /// Maximum number of hardware devices that can be registered.
    static constexpr size_t MAX_DEVICES = 8U;

    /**
     * @brief Register a hardware device adapter.
     * @param device  Pointer to a device whose lifetime exceeds the manager's.
     * @return true on success, false if the table is full.
     */
    bool registerDevice(IRxTxDevice *device);

    /**
     * @brief Look up a registered device by name.
     * @return Pointer to the device, or nullptr.
     */
    IRxTxDevice *findDevice(const char *name) const;

    // ── Capture control ──────────────────────────────────────────────────

    /**
     * @brief Start capturing from the specified device.
     *
     * The device is placed in receive mode and incoming samples are routed
     * into the internal RingBuffer for asynchronous protocol matching.
     *
     * @param device  A previously registered device.
     * @return true if capture was started successfully.
     */
    bool startCapture(IRxTxDevice *device);

    /**
     * @brief Stop the active capture session.
     */
    void stopCapture();

    /// @brief True while a capture session is active.
    bool isCapturing() const;

    // ── Replay / Transmit ────────────────────────────────────────────────

    /**
     * @brief Transmit a previously captured signal.
     *
     * The manager selects the matching protocol (by name in the record),
     * encodes the signal into raw timings, and sends it through the
     * appropriate device.
     *
     * @param record  The signal to replay.
     * @return true if the signal was transmitted.
     */
    bool transmit(const SignalRecord &record);

    // ── Last captured signal ─────────────────────────────────────────────

    /// @brief True when at least one signal has been decoded.
    bool hasLastRecord() const;

    /// @brief Reference to the most recently decoded SignalRecord.
    const SignalRecord &lastRecord() const;

    // ── Worker task ──────────────────────────────────────────────────────

    /**
     * @brief Entry point for the FreeRTOS worker task.
     *
     * This function is meant to be passed to `xTaskCreateStatic` (or
     * ThreadManager).  It loops forever, draining the RingBuffer and
     * attempting protocol matches.
     *
     * @param param  Unused (convention-required by FreeRTOS).
     */
    static void workerEntry(void *param);

private:
    RadioManager();

    /// Ring buffer capacity for raw signal samples.
    static constexpr size_t RAW_RING_CAPACITY = 1024U;

    /// Registered device table.
    IRxTxDevice *devices_[MAX_DEVICES];
    size_t deviceCount_;

    /// Currently active capture device (nullptr when idle).
    IRxTxDevice *activeDevice_;

    /// Ring buffer fed by the capture ISR / polling loop.
    RingBuffer<int32_t, RAW_RING_CAPACITY> rawRing_;

    /// Scratch buffer used by the worker to batch-pop samples.
    int32_t workerBuf_[MAX_RAW_SAMPLES];
    size_t workerBufLen_;

    /// Scratch buffer for transmit encoding (avoids large stack allocs).
    int32_t txBuf_[MAX_RAW_SAMPLES];

    /// Scratch buffer for device reads in the worker loop.
    uint8_t readBuf_[256U];

    /// Most recently decoded signal.
    SignalRecord lastRecord_;
    bool hasLast_;

    /// @brief Internal: drain the ring buffer and attempt protocol decode.
    void processBuffer();
};

} // namespace hackos::radio
