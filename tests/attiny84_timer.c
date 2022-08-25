/*
	attiny84_timer.c

	Copyright Giles Atkinson 2022

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

#include <avr/io.h>
#include <stdio.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "attiny84");

static volatile uint8_t tval;

static void do_reads(void) {
	PORTA &= ~1;        // Reset for next.

	// PA1 triggers read, PA2 exits loop.

	while (!(PINA & 4)) {
		if (PINA & 2) {
			uint8_t t;

			t = TCNT0;
			PORTA |= 1;  // Tickle monitor
			PORTA &= ~1; // Reset for next
			tval = t;    // Defeat compiler optimisations by using value.
		}
	}
}

int main()
{
	// This is to test reading counter values.
	// PA0 will be used to tickle monitor program.

	DDRA = 1;

	// Start the 8-bit timer to run 256 cycles with prescaler 64 full count.

	TCCR0B = 3; 		 // Set prescaler ratio to 64 and run.
	PORTA |= 1;          // Tickle monitor to say ready.

	do_reads();

	// Now use CTC mode with TOP not a power of 2.

	TCCR0B = 0;          // Timer stopped
	TCCR0A = _BV(WGM01); // CTC (clear on compare) mode.
	OCR0A = 22;          // 23 counts per cycle
	TCNT0 = 0;           // Restart from zero.
	GTCCR = _BV(PSR10);  // Clear pre-scaler.
	TCCR0B = 3;          // Set prescaler ratio to 64 and run.
	PORTA |= 1;          // Tickle monitor to say ready.

	do_reads();

	// Phase-correct PWM 

	TCCR0B = 0;          // Timer stopped
	TCCR0A = _BV(WGM00); // Phase correct, TOP = 0x3ff
	TCNT0 = 0;           // Restart from zero.
	GTCCR = _BV(PSR10);  // Clear pre-scaler.
	TCCR0B = 3;          // Set prescaler ratio to 8 and run.
	PORTA |= 1;          // Tickle monitor to say ready.

	do_reads();

	// Fast PWM, top is 0xff

	TCCR0B = 0;          // Timer stopped
	TCCR0A = _BV(WGM01) + _BV(WGM00); // Phase correct, TOP = 0x3ff
	TCNT0 = 10;          // Start with non-zero count.
	GTCCR = _BV(PSR10);  // Clear pre-scaler.
	TCCR0B = 3;          // Set prescaler ratio to 8 and run.
	PORTA |= 1;          // Tickle monitor to say ready.

	do_reads();

	// Sleeping with interrupt off is interpreted by simavr as "exit please"

	sleep_cpu();
}
