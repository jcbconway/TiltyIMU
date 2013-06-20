// VirtualWire.cpp
//
// Virtual Wire implementation for Arduino
// See the README file in this directory fdor documentation
//
// Changes:
// 2008-05-25: fixed a bug that could prevent messages with certain
//  bytes sequences being received (false message start detected)
//
// Author: Mike McCauley (mikem@open.com.au)
// Copyright (C) 2008 Mike McCauley
// $Id: VirtualWire.cpp,v 1.4 2009/03/31 20:49:41 mikem Exp mikem $

#include "WProgram.h"
#include "VirtualWire.h"
#include <util/crc16.h>

static uint8_t vw_tx_buf[(VW_MAX_MESSAGE_LEN * 2) + VW_HEADER_LEN] 
     = {0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x38, 0x2c};

// Number of symbols in vw_tx_buf to be sent;
static uint8_t vw_tx_len = 0;

// Index of the next symbol to send. Ranges from 0 to vw_tx_len
static uint8_t vw_tx_index = 0;

// Bit number of next bit to send
static uint8_t vw_tx_bit = 0;

// Sample number for the transmitter. Runs 0 to 7 during one bit interval
static uint8_t vw_tx_sample = 0;

// Flag to indicated the transmitter is active
static volatile uint8_t vw_tx_enabled = 0;

// Total number of messages sent
static uint16_t vw_tx_msg_count = 0;

// The digital IO pin number of the press to talk, enables the transmitter hardware
static uint8_t vw_ptt_pin = 10;
static uint8_t vw_ptt_inverted = 0;

// The digital IO pin number of the receiver data
static uint8_t vw_rx_pin = 11;

// The digital IO pin number of the transmitter data
static uint8_t vw_tx_pin = 12;

// Current receiver sample
static uint8_t vw_rx_sample = 0;

// Last receiver sample
static uint8_t vw_rx_last_sample = 0;

// PLL ramp, varies between 0 and VW_RX_RAMP_LEN-1 (159) over 
// VW_RX_SAMPLES_PER_BIT (8) samples per nominal bit time. 
// When the PLL is synchronised, bit transitions happen at about the
// 0 mark. 
static uint8_t vw_rx_pll_ramp = 0;

// This is the integrate and dump integral. If there are <5 0 samples in the PLL cycle
// the bit is declared a 0, else a 1
static uint8_t vw_rx_integrator = 0;

// Flag indictate if we have seen the start symbol of a new message and are
// in the processes of reading and decoding it
static uint8_t vw_rx_active = 0;

// Flag to indicate that a new message is available
static volatile uint8_t vw_rx_done = 0;

// Flag to indicate the receiver PLL is to run
static uint8_t vw_rx_enabled = 0;

// Last 12 bits received, so we can look for the start symbol
static uint16_t vw_rx_bits = 0;

// How many bits of message we have received. Ranges from 0 to 12
static uint8_t vw_rx_bit_count = 0;

// The incoming message buffer
static uint8_t vw_rx_buf[VW_MAX_MESSAGE_LEN];

// The incoming message expected length
static uint8_t vw_rx_count = 0;

// The incoming message buffer length received so far
static volatile uint8_t vw_rx_len = 0;

// Number of bad messages received and dropped due to bad lengths
static uint8_t vw_rx_bad = 0;

// Number of good messages received
static uint8_t vw_rx_good = 0;

// 4 bit to 6 bit symbol converter table
// Used to convert the high and low nybbles of the transmitted data
// into 6 bit symbols for transmission. Each 6-bit symbol has 3 1s and 3 0s 
// with at most 2 consecutive identical bits
static uint8_t symbols[] =
{
    0xd,  0xe,  0x13, 0x15, 0x16, 0x19, 0x1a, 0x1c, 
    0x23, 0x25, 0x26, 0x29, 0x2a, 0x2c, 0x32, 0x34
};

// Cant really do this as a real C++ class, since we need to have 
// an ISR
extern "C"
{

// Compute CRC over count bytes.
// This should only be ever called at user level, not interrupt level
uint16_t vw_crc(uint8_t *ptr, uint8_t count)
{
    uint16_t crc = 0xffff;

    while (count-- > 0) 
	crc = _crc_ccitt_update(crc, *ptr++);
    return crc;
}

// Convert a 6 bit encoded symbol into its 4 bit decoded equivalent
uint8_t vw_symbol_6to4(uint8_t symbol)
{
    uint8_t i;
    
    // Linear search :-( Could have a 64 byte reverse lookup table?
    for (i = 0; i < 16; i++)
	if (symbol == symbols[i]) return i;
    return 0; // Not found
}

// Set the output pin number for transmitter data
void vw_set_tx_pin(uint8_t pin)
{
    vw_tx_pin = pin;
}

// Set the pin number for input receiver data
void vw_set_rx_pin(uint8_t pin)
{
    vw_rx_pin = pin;
}

// Set the output pin number for transmitter PTT enable
void vw_set_ptt_pin(uint8_t pin)
{
    vw_ptt_pin = pin;
}

// Set the ptt pin inverted (low to transmit)
void vw_set_ptt_inverted(uint8_t inverted)
{
    vw_ptt_inverted = inverted;
}

// Called 8 times per bit period
// Phase locked loop tries to synchronise with the transmitter so that bit 
// transitions occur at about the time vw_rx_pll_ramp is 0;
// Then the average is computed over each bit period to deduce the bit value
void vw_pll()
{
    // Integrate each sample
    if (vw_rx_sample)
	vw_rx_integrator++;

    if (vw_rx_sample != vw_rx_last_sample)
    {
	// Transition, advance if ramp > 80, retard if < 80
	vw_rx_pll_ramp += ((vw_rx_pll_ramp < VW_RAMP_TRANSITION) 
			   ? VW_RAMP_INC_RETARD 
			   : VW_RAMP_INC_ADVANCE);
	vw_rx_last_sample = vw_rx_sample;
    }
    else
    {
	// No transition
	// Advance ramp by standard 20 (== 160/8 samples)
	vw_rx_pll_ramp += VW_RAMP_INC;
    }
    if (vw_rx_pll_ramp >= VW_RX_RAMP_LEN)
    {
	// Add this to the 12th bit of vw_rx_bits, LSB first
	// The last 12 bits are kept
	vw_rx_bits >>= 1;

	// Check the integrator to see how many samples in this cycle were high.
	// If < 5 out of 8, then its declared a 0 bit, else a 1;
	if (vw_rx_integrator >= 5)
	    vw_rx_bits |= 0x800;

	vw_rx_pll_ramp -= VW_RX_RAMP_LEN;
	vw_rx_integrator = 0; // Clear the integral for the next cycle

	if (vw_rx_active)
	{
	    // We have the start symbol and now we are collecting message bits,
	    // 6 per symbol, each which has to be decoded to 4 bits
	    if (++vw_rx_bit_count >= 12)
	    {
		// Have 12 bits of encoded message == 1 byte encoded
		// Decode as 2 lots of 6 bits into 2 lots of 4 bits
		// The 6 lsbits are the high nybble
		uint8_t this_byte = 
		    (vw_symbol_6to4(vw_rx_bits & 0x3f)) << 4 
		    | vw_symbol_6to4(vw_rx_bits >> 6);

		// The first decoded byte is the byte count of the following message
		// the count includes the byte count and the 2 trailing FCS bytes
		// REVISIT: may also include the ACK flag at 0x40
		if (vw_rx_len == 0)
		{
		    // The first byte is the byte count
		    // Check it for sensibility. It cant be less than 4, since it
		    // includes the bytes count itself and the 2 byte FCS
		    vw_rx_count = this_byte;
		    if (vw_rx_count < 4 || vw_rx_count > VW_MAX_MESSAGE_LEN)
		    {
			// Stupid message length, drop the whole thing
			vw_rx_active = false;
			vw_rx_bad++;
                        return;
		    }
		}
		vw_rx_buf[vw_rx_len++] = this_byte;

		if (vw_rx_len >= vw_rx_count)
		{
		    // Got all the bytes now
		    vw_rx_active = false;
		    vw_rx_good++;
		    vw_rx_done = true; // Better come get it before the next one starts
		}
		vw_rx_bit_count = 0;
	    }
	}
	// Not in a message, see if we have a start symbol
	else if (vw_rx_bits == 0xb38)
	{
	    // Have start symbol, start collecting message
	    vw_rx_active = true;
	    vw_rx_bit_count = 0;
	    vw_rx_len = 0;
	    vw_rx_done = false; // Too bad if you missed the last message
	}
    }
}

// Speed is in bits per sec RF rate
void vw_setup(uint16_t speed)
{
    // Calculate the OCR1A overflow count based on the required bit speed
    // and CPU clock rate
    uint16_t ocr1a = (F_CPU / 8UL) / speed;

#ifndef TEST
    // Set up timer1 for a tick every 62.50 microseconds 
    // for 2000 bits per sec
    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS10);
    // Caution: special procedures for setting 16 bit regs
    OCR1A = ocr1a;
    // Enable interrupt
#ifdef TIMSK1
    // atmega168
    TIMSK1 |= _BV(OCIE1A);
#else
    // others
    TIMSK |= _BV(OCIE1A);
#endif

#endif

    // Set up digital IO pins
    pinMode(vw_tx_pin, OUTPUT);
    pinMode(vw_rx_pin, INPUT);
    pinMode(vw_ptt_pin, OUTPUT);
    digitalWrite(vw_ptt_pin, vw_ptt_inverted);
}

// Start the transmitter, call when the tx buffer is ready to go and vw_tx_len is
// set to the total number of symbols to send
void vw_tx_start()
{
    vw_tx_index = 0;
    vw_tx_bit = 0;
    vw_tx_sample = 0;

    // Disable the receiver PLL
    vw_rx_enabled = false;

    // Enable the transmitter hardware
    digitalWrite(vw_ptt_pin, true ^ vw_ptt_inverted);

    // Next tick interrupt will send the first bit
    vw_tx_enabled = true;
}

// Stop the transmitter, call when all bits are sent
void vw_tx_stop()
{
    // Disable the transmitter hardware
    digitalWrite(vw_ptt_pin, false ^ vw_ptt_inverted);
    digitalWrite(vw_tx_pin, false);

    // No more ticks for the transmitter
    vw_tx_enabled = false;

    // Enable the receiver PLL
    vw_rx_enabled = true;
}

// Enable the receiver. When a message becomes available, vw_rx_done flag
// is set, and vw_wait_rx() will return.
void vw_rx_start()
{
    if (!vw_rx_enabled)
    {
	vw_rx_enabled = true;
	vw_rx_active = false; // Never restart a partial message
    }
}

// Disable the receiver
void vw_rx_stop()
{
    vw_rx_enabled = false;
}

// Return true if the transmitter is active
uint8_t vx_tx_active()
{
    return vw_tx_enabled;
}

// Wait for the transmitter to become available
// Busy-wait loop until the ISR says the message has been sent
void vw_wait_tx()
{
    while (vw_tx_enabled)
	;
}

// Wait for the receiver to get a message
// Busy-wait loop until the ISR says a message is available
// can then call vw_get_message()
void vw_wait_rx()
{
    while (!vw_rx_done)
	;
}

// Wait at most max milliseconds for the receiver to receive a message
// Return the truth of whether there is a message
uint8_t vw_wait_rx_max(unsigned long milliseconds)
{
    unsigned long start = millis();

    while (!vw_rx_done && ((millis() - start) < milliseconds))
	;
    return vw_rx_done;
}

// Wait until transmitter is available and encode and queue the message
// into vw_tx_buf
// The message is raw bytes, with no packet structure imposed
// It is transmitted preceded a byte count and followed by 2 FCS bytes
uint8_t vw_send(uint8_t* buf, uint8_t len)
{
    uint8_t i;
    uint8_t index = 0;
    uint16_t crc = 0xffff;
    uint8_t *p = vw_tx_buf + VW_HEADER_LEN; // start of the message area
    uint8_t count = len + 3; // Added byte count and FCS to get total number of bytes

    if (len > VW_MAX_PAYLOAD)
	return false;

    // Wait for transmitter to become available
    vw_wait_tx();

    // Encode the message length
    crc = _crc_ccitt_update(crc, count);
    p[index++] = symbols[count >> 4];
    p[index++] = symbols[count & 0xf];

    // Encode the message into 6 bit symbols. Each byte is converted into 
    // 2 6-bit symbols, high nybble first, low nybble second
    for (i = 0; i < len; i++)
    {
	crc = _crc_ccitt_update(crc, buf[i]);
	p[index++] = symbols[buf[i] >> 4];
	p[index++] = symbols[buf[i] & 0xf];
    }

    // Append the fcs, 16 bits before encoding (4 6-bit symbols after encoding)
    // Caution: VW expects the _ones_complement_ of the CCITT CRC-16 as the FCS
    // VW sends FCS as low byte then hi byte
    crc = ~crc;
    p[index++] = symbols[(crc >> 4)  & 0xf];
    p[index++] = symbols[crc & 0xf];
    p[index++] = symbols[(crc >> 12) & 0xf];
    p[index++] = symbols[(crc >> 8)  & 0xf];

    // Total number of 6-bit symbols to send
    vw_tx_len = index + VW_HEADER_LEN;

    // Start the low level interrupt handler sending symbols
    vw_tx_start();

    return true;
}

// This is the interrupt service routine called when timer1 overflows
// Its job is to output the next bit from the transmitter (every 8 calls)
// and to call the PLL code if the receiver is enabled
//ISR(SIG_OUTPUT_COMPARE1A)
//SIGNAL(TIMER1_COMPA_vect)
ISR(TIMER1_COMPA_vect)
{
    vw_rx_sample = digitalRead(vw_rx_pin);

    // Do transmitter stuff first to reduce transmitter bit jitter due 
    // to variable receiver processing
    if (vw_tx_enabled && vw_tx_sample++ == 0)
    {
        // Send next bit
	// Symbols are sent LSB first
        // Finished sending the whole message? (after waiting one bit period 
	// since the last bit)
        if (vw_tx_index >= vw_tx_len)
	{
	    vw_tx_stop();
	    vw_tx_msg_count++;
	}
        else
        {
	    digitalWrite(vw_tx_pin, vw_tx_buf[vw_tx_index] & (1 << vw_tx_bit++));
	    if (vw_tx_bit >= 6)
	    {
	        vw_tx_bit = 0;
                vw_tx_index++;
	    }
        }
    }
    if (vw_tx_sample > 7)
	vw_tx_sample = 0;

    if (vw_rx_enabled)
	vw_pll();
}

// Return true if there is a message available
uint8_t vw_have_message()
{
    return vw_rx_done;
}

// Get the last message received (without byte count or FCS)
// Copy at most *len bytes, set *len to the actual number copied
// Return true if there is a message and the FCS is OK
uint8_t vw_get_message(uint8_t* buf, uint8_t* len)
{
    uint8_t rxlen;

    // Message available?
    if (!vw_rx_done)
	return false;

    // Wait until vw_rx_done is set before reading vw_rx_len
    // then remove bytecount and FCS
    rxlen = vw_rx_len - 3;

    // Copy message (good or bad)
    if (*len > rxlen)
	*len = rxlen;
    memcpy(buf, vw_rx_buf + 1, *len);

    vw_rx_done = false; // OK, got that message thanks

    // Check the FCS, return goodness
    return (vw_crc(vw_rx_buf, vw_rx_len) == 0xf0b8); // FCS OK?
}

}
