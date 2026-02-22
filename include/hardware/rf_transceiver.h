/**
 * @file rf_transceiver.h
 * @brief HAL wrapper for 433 MHz RF receive/transmit using RCSwitch.
 */

#pragma once

#include <RCSwitch.h>
#include <cstdint>

class RFTransceiver
{
public:
    static RFTransceiver &instance();

    /// Initialise the RF receiver (interrupt-driven via RCSwitch).
    bool initReceive();
    /// Initialise the RF transmitter.
    bool initTransmit();
    /// Release hardware resources.
    void deinit();

    /// Returns true when a new OOK packet is available.
    bool available() const;
    /// Read the received code and bit-length; clears the available flag.
    bool read(unsigned long &code, unsigned int &bitLength, unsigned int &delay);

    /// Transmit a code with the given bit-length using the stored protocol.
    void send(unsigned long code, unsigned int bitLength);

    bool hasLastCode() const;
    unsigned long lastCode() const;
    unsigned int lastBitLength() const;
    unsigned int lastDelay() const;

private:
    RFTransceiver();

    RCSwitch rf_;
    bool recvActive_;
    bool sendActive_;
    bool hasLast_;
    unsigned long lastCode_;
    unsigned int lastBitLength_;
    unsigned int lastDelay_;
};
