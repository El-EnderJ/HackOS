/**
 * @file ir_transceiver.h
 * @brief HAL wrapper for IR receive/transmit using IRremoteESP8266.
 */

#pragma once

#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <cstdint>

class IRTransceiver
{
public:
    static IRTransceiver &instance();

    /// Initialise the IR receiver (must be called before decode()).
    bool initReceive();
    /// Initialise the IR transmitter (must be called before send()).
    bool initTransmit();
    /// Release hardware resources.
    void deinit();

    /// Returns true when a new frame has been decoded since the last call.
    bool decode(uint64_t &value, decode_type_t &protocol, uint16_t &bits);

    /// Re-transmit the last decoded (or manually set) code.
    void send(uint64_t value, decode_type_t protocol, uint16_t bits);

    bool hasLastCode() const;
    uint64_t lastValue() const;
    decode_type_t lastProtocol() const;
    uint16_t lastBits() const;

private:
    static constexpr uint16_t CAPTURE_BUF = 1024U;
    static constexpr uint8_t TIMEOUT_MS = 50U;

    IRTransceiver();

    IRrecv *recv_;
    IRsend *send_;
    decode_results results_;
    bool recvActive_;
    bool sendActive_;
    bool hasLast_;
    uint64_t lastValue_;
    decode_type_t lastProtocol_;
    uint16_t lastBits_;
};
