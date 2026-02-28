/**
 * @file net_forensics_app.h
 * @brief Network Forensics – PCAP sniffer for WiFi traffic analysis.
 *
 * Captures 802.11 packets in promiscuous mode and writes standard
 * PCAP files to the SD card for offline analysis in Wireshark.
 *
 * Features:
 *  - Selective sniffing by channel (1-13) and frame type
 *  - EAPOL Handshake Hunter mode (WPA/WPA2 4-way handshake)
 *  - Circular RAM buffer with background SD write (IO task)
 *  - Real-time packet/second statistics on OLED
 */

#pragma once

#include "apps/app_base.h"

AppBase *createNetForensicsApp();
