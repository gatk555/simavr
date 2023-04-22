/*
	attiny84_servo.c
		RC servo relay firmware.

	Copyright 2023 Giles Atkinson

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This program receives serial data and uses it to control RC servos.
 * The servos require a 0.9-2.1 mS pulse at least every 20mS (50HZ), done by
 * busy-waiting for accuracy.  But the chip must also watch for serial input.
 * At 1200 baud a bit arrives every 833.3 mS, so three samples of each of
 * 2 bits and the leading part of a third can be accumulated in the USI buffer
 * while busy-waiting.
 */

#undef F_CPU
#define F_CPU 4000000UL

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include "avr_mcu_section.h"


//#define T85 // ATtiny85
#ifdef T85
AVR_MCU(F_CPU, "attiny85");
#define T_FLAGS TIFR
#else
AVR_MCU(F_CPU, "attiny84");
#define T_FLAGS TIFR0

#if 1 //Temporary
#define SET_A
#define CLR_A
#else
#define SET_A PORTA |= _BV(PA7)		// Set output A - PA7
#define CLR_A PORTA &= ~_BV(PA7)
#define GET_L (PINA & _BV(PA6))   // Get serial line: DI is PA6
#endif
#endif

//#define SIMAVR
#ifdef SIMAVR
#include <stdio.h>

/* No UART in tiny84, so simply write to unused register TCNT1L. */

static int uart_putchar(char c, FILE *stream) {
#ifdef WINDOWS
	if (c == '\n')
		uart_putchar('\r', stream);
#endif
	TCNT1L = c;
	return 0;
}

static FILE mystdout = FDEV_SETUP_STREAM(uart_putchar, NULL,
										 _FDEV_SETUP_WRITE);
#endif

uint16_t tick_a, tick_b, tick_c;

/* Macro for micro-second delays - no FP, unlike _delay_us(), accurate if
 * clock F_CPU is in whole MHz.
 */

#define delay_us(n) \
	__builtin_avr_delay_cycles((uint16_t)(F_CPU / 1000000) * n)

/* Check for serial input. */

static void get_byte(void)
{
	static uint8_t bits, value;
	static int8_t  carried;
	uint8_t        data, odata;
	int8_t		   samples = 0;  // Silence gcc

	/* Exits from this loop: wait for more bits, or stop bit/idle line. */

	for (;;) {
		/* Check for idle. */

		data = USIDR;
		if (data == 0xff && !carried)
			goto sync;      // Line is idle

		if (carried) {
			/* Second or later entry for current byte.
			 * Captured bits were counted, but synchronise count and data.
			 */

			do {
				T_FLAGS |= _BV(OCF0A);
				samples = USISR & 0x0f;
				data = USIDR;
				odata = data; // Save for comparison.
				data <<= (8 - samples);
			} while (T_FLAGS & _BV(OCF0A));

			if (carried < 0) {
				/* First bit already accounted for. */

				--samples;
				data <<= 1;
			} else if (carried == 1) {
				/* Pretend there is an extra sample. */

				++samples;
				data >>= 1;
			}
		} else {
			/* The game's afoot!  Find the leading edge of the start bit. */

			odata = data; // Save for comparison.
			samples = 8;
			do {
				if (--samples == 0)
					break;
				data <<= 1;
			} while (data & 0x80);
		}

		/* Eat the rest of the buffer. */

		while (samples >= 3) {
			value >>= 1;

			/* Use the middle sample. */

			data <<= 1;
			value |= (data & 0x80);
			data <<= 2;
			samples -= 3;
			if (++bits == 9)
				break;
		}

		if (bits < 9) {
			/* Synchronise with the USI's sampling. */

			T_FLAGS |= _BV(OCF0A);
			if (USIDR == odata) {
				/* Wait for next sample. */

				while (!(T_FLAGS & _BV(OCF0A)))
					;
			}
			++samples;
		
			switch (samples) {
			default:
			case 1:
				break;
			case 2:
				++bits;
				value >>= 1;
				if (USIDR & 1)
					value |= 0x80;
				samples = -1;
				break;
			case 3:
				++bits;
				value >>= 1;
				data <<= 1;
				value |= (data & 0x80);
				samples = 0;
				break;
			}
		}

		if (bits == 9) {
			carried = bits = 0;

			/* Byte complete: do something; about 888 clocks are available. */

			PORTA = value;

			/* Synchronise with the input clock so that we have 222 uS before
			 * the next sample.
			 */
		sync:
			T_FLAGS |= _BV(OCF0A);
			while (!(T_FLAGS & _BV(OCF0A)))
				;
			USIDR |= (0xff << ++samples);  // Blank processed samples
			if (USIDR == 0xff)		       // Stop bit or idle.
				return;
		} else {
			carried = samples ? samples : 2; // 2 indicates active, not carried

			/* Reset counter for next time. */

			USISR = 0;
			return;
		}
	}
}

int main(void)
{
	int i;

#ifdef SIMAVR
	stdout = &mystdout;
#endif
	/* Drop the clock rate from 8 to 4 MHz. */

	CLKPR = _BV(CLKPCE);                            // Unlock pre-scalar
	CLKPR = _BV(CLKPS0);                            // Divide by 2

	/* Set Counter 0 to count from 0 to 138 with the counter pre-scaled by 8:
	 * the desired division ratio is 4000/3.6 or 1111.1 recurring.
	 */

	OCR0A = 138;
	TCCR0A = _BV(WGM01);        // CTC mode: clear count on OCR0A match.
	TCCR0B = _BV(CS01);         // Run, pre-scale factor 8.

	/* Set USI for input clocked by OCR0A. */

	USICR = _BV(USICS0);

	/* Enable output pins. */

//	DDRA |= _BV(PA7);
	DDRA = ~_BV(PA6); // All pins except DI
	PORTA = 0x55;

	/* Wait until the serial input line is idle for 24 cycles. */

	i = 0;
	do {
		if (!(USIDR & 1)) {
			i = 0;
			continue;
		}
		while (!(T_FLAGS & _BV(OCF0A)))
			;
		T_FLAGS |= _BV(OCF0A);
	} while (++i < 24);        
	
	/* All set, loop receiving commands and sending them out. */

	for (;;) {
		for (i = 0; i < 30; ++i) {
			get_byte();                     // Check serial,
			SET_A;
			delay_us(900);
			CLR_A;
		}
		for (i = 0; i < 30; ++i) {
			get_byte();                     // Check serial,
			SET_A;
			delay_us(2100);
			CLR_A;
		}
	}
}
