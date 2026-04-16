// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Leland Lucius
// ----------------------------------------------------------------
// DECtalk ESPress Serial Protocol Emulation
//
// This module implements the DECtalk ESPress serial host <-> device protocol,
// allowing the ESP32 to act as a drop-in replacement for a real DECtalk Express
// device. A host computer can communicate using the standard protocol: plain
// ASCII text for speech, control characters (ETX, ENQ, SO, SI), DLE command
// sequences, and XON/XOFF software flow control.
//
// Protocol reference:
//   - USB CDC-ACM: native USB port for host communication
//   - Host -> Device: ASCII text + control characters + DLE commands
//   - Device -> Host: DLE status sequences + XON/XOFF flow control
// ----------------------------------------------------------------

#ifndef DECTALK_ESPRESS_H
#define DECTALK_ESPRESS_H

#include <stdint.h>
#include "port.h"
#include "esc.h"
#include "pcport.h"

// -- ASCII Control Characters ----------------------------------------
//
// The authoritative control character definitions live in esc.h (included
// above).  The ESPress protocol uses:
//
//   SOH   (0x01)  Start of Heading - flush acknowledge
//   ETX   (0x03)  End of Text - flush/cancel all speech
//   ENQ   (0x05)  Enquiry - request status from device
//   VT    (0x0B)  Vertical Tab - synchronization marker
//   SO    (0x0E)  Shift Out - pause speech output
//   SI    (0x0F)  Shift In - resume speech output
//   DLE   (0x10)  Data Link Escape - start 4-byte sequence
//   XON   (0x11)  DC1 (Ctrl-Q) - resume transmission
//   XOFF  (0x13)  DC3 (Ctrl-S) - pause transmission
//   RDEL  (0xFF)  Internal sync marker (CMD_sync_char)
// ----------------------------------------------------------------

// -- DLE Byte 1 Prefixes ---------------------------------------------
//
// DLE sequences are exactly 4 bytes: DLE, type/command, param1, param2.
// The first data byte (byte 1) determines the sequence type:
//
//   0x20-0x2F: Command from host (CMD class in upper nibble)
//   0x30-0x3F: Data from host (stored as input data)
//   0x40-0x4F: Status update from device
//   0x50-0x5F: Index marker data from device
//   0x70 ('p'): DMA sync
//   0x71 ('q'): Flush-and-speak-character
// ----------------------------------------------------------------
#define DLE_PREFIX_CMD_LO   0x20    // Command class range start
#define DLE_PREFIX_CMD_HI   0x2F    // Command class range end
#define DLE_PREFIX_DATA_LO  0x30    // Data range start
#define DLE_PREFIX_DATA_HI  0x3F    // Data range end
#define DLE_PREFIX_STATUS   0x40    // Status update (device -> host)
#define DLE_PREFIX_INDEX    0x50    // Index marker (device -> host)
#define DLE_PREFIX_SYNC     0x70    // 'p' - DMA sync
#define DLE_PREFIX_FLUSHCH  0x71    // 'q' - flush + speak character

// -- Device Status Bits (from pcport.h) ------------------------------
//
// The authoritative status bit definitions live in pcport.h (included
// above).  The ESPress protocol uses:
//
//   STAT_int           (0x0001)    Running in interrupt mode
//   STAT_tr_char       (0x0002)    Has data to transmit
//   STAT_rr_char       (0x0004)    Ready to receive characters
//   STAT_cmd_ready     (0x0008)    Ready for commands
//   STAT_dma_ready     (0x0010)    DMA ready
//   STAT_digitized     (0x0020)    In digitized audio mode
//   STAT_new_index     (0x0040)    New index marker available
//   STAT_new_status    (0x0080)    New status posted
//   STAT_index_valid   (0x0200)    Index value is valid
//   STAT_flushing      (0x0400)    Flush operation in progress
// ----------------------------------------------------------------

// -- SPC Command Classes (from pcport.h) -----------------------------
//
// The authoritative command class definitions live in pcport.h (included
// above).  The ESPress protocol uses:
//
//   CMD_null           (0x0000)    Post current status
//   CMD_control        (0x1000)    Hardware control operation
//   CMD_test           (0x2000)    Self-test
//   CMD_id             (0x3000)    Return software identification
// ----------------------------------------------------------------

// -- Control Sub-Commands (from pcport.h, OR'd with CMD_control) -----
//
//   CTRL_vol_up        (0x0100)    Increase volume
//   CTRL_vol_down      (0x0200)    Decrease volume
//   CTRL_vol_set       (0x0300)    Set specific volume level
//   CTRL_pause         (0x0400)    Pause speech output
//   CTRL_resume        (0x0500)    Resume speech output
//   CTRL_flush         (0x0600)    Flush all pending text
// ----------------------------------------------------------------

// -- DLE Encoding/Decoding Helpers -----------------------------------

// Encode a 6-bit value for DLE transmission.
// Values below 0x20 are shifted up by 0x40 to stay in printable range.
static inline uint8_t dle_encode_byte(uint8_t val6)
{
    val6 &= 0x3F;
    return (val6 < 0x20)
        ? (val6 + 0x40)
        : val6;
}

// Decode a DLE-encoded byte back to its 6-bit value.
static inline uint8_t dle_decode_byte(uint8_t encoded)
{
    return encoded & 0x3F;
}

// Encode a 16-bit status/index word into a 4-byte DLE sequence.
//   buf[0] = DLE (0x10)
//   buf[1] = prefix | upper 4 bits
//   buf[2] = middle 6 bits (adjusted)
//   buf[3] = lower 6 bits (adjusted)
//
// prefix: DLE_PREFIX_STATUS (0x40) for status, DLE_PREFIX_INDEX (0x50) for index
static inline void dle_encode_word(uint8_t prefix, uint16_t word, uint8_t *buf)
{
    buf[0] = DLE;
    buf[1] = prefix | ((word >> 12) & 0x0F);
    buf[2] = dle_encode_byte((word >> 6) & 0x3F);
    buf[3] = dle_encode_byte(word & 0x3F);
}

// Decode a 16-bit word from a 4-byte DLE sequence.
static inline uint16_t dle_decode_word(const uint8_t *buf)
{
    return (uint16_t)(((buf[1] & 0x0F) << 12) |
                      ((buf[2] & 0x3F) << 6)  |
                       (buf[3] & 0x3F));
}

// ----------------------------------------------------------------
// The original DECtalk Express serial path accepts raw 0xFF and passes it
// into the command parser as CMD_sync_char.  It is a synchronisation marker,
// not spoken text.
// ----------------------------------------------------------------
static inline int dectalk_is_sync_char(uint8_t c)
{
    return c == RDEL;
}
#endif // DECTALK_ESPRESS_H
