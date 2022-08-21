/*
	atmega324a_timer16.c

	Copyright 2022 Giles Atkinson

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
#include <avr/sleep.h>

/*
 * This demonstrates how to use the avr_mcu_section.h file.
 * The macro adds a section to the ELF file with useful
 * information for the simulator.
 */
#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega324a");

int main()
{
	// The test runner monitors the OC1B (PD4) and OC1B (PD4) pins
	// for PWM output and PD0 for firmware signals.

	DDRD = _BV(PD5) + _BV(PD4) + _BV(PD0);

	/**** Start the 16 bits timer 1, with default "normal" waveform. ****/

	// Compare match B after 50 cycles.

	OCR1BL = 49;

	// Timer prescaler to unity, starts count.

	TCCR1B = _BV(CS10);

	// Signal monitor program - should be one cycle

	PORTD |= _BV(PD0);

	// Compare unit B to set pin on match, set pin low.

	TCCR1A = _BV(COM1B1) + _BV(COM1B0);

	// Busy-wait for match

	while (!(TIFR1 & _BV(OCF1B)))
		;
	TIFR1 = 0xff; // Clear flags.

	// Busy-wait for overflow

	while (!(TIFR1 & _BV(TOV1)))
		;
	PORTD &= ~_BV(PD0); // Trigger monitor
	TIFR1 = 0xff; // Clear flags.

	// Clear pin, as timer does not reset it.  With a real device,
	// the TCCR1A changes should be enough, but simavr needs the PORTD write,
	// but not TCCR1A changes!

	TCCR1A = 0;
	PORTD &= ~_BV(PD4); // Reset pin
	TCCR1A = _BV(COM1B1) + _BV(COM1B0);

	// Busy-wait again for match

	while (!(TIFR1 & _BV(OCF1B)))
		;

	// Clear again.

	TCCR1A = 0;
	PORTD &= ~_BV(PD4); // Reset pin
	TCCR1A = _BV(COM1B1) + _BV(COM1B0);

	/* End of normal mode test, start phase-correct 8-bit. */

	OCR1BL = 200;         // Still immediate update
	TCCR1A |= _BV(WGM10);  // WGM = 1, phase correct, 8-bit
	TCNT1H = 0;
	TCNT1L = 0;           // Restart count
	PORTD |= _BV(PD0);    // Trigger monitor
	TIFR1 = 0xff;         // Clear flags.

	// Busy-wait for overflow, expecting two OCRA matches first.

	while (!(TIFR1 & _BV(TOV1)))
		;
        
	// Waggle for monitor

	PORTD &= ~_BV(PD0);

	// Busy-wait again for matches and overflow - check for off-by one.

	TIFR1 = 0xff;         // Clear flags.
	while (!(TIFR1 & _BV(TOV1)))
            ;
	PORTD |= _BV(PD0);    // Waggle again

	/**** Start phase-correct 9-bit. ****/

	TCCR1B = 0;
	TCCR1A = _BV(WGM11) + _BV(COM1B1) + _BV(COM1B0);  // WGM = 2, PC, 9-bit
	OCR1BH = 1;           // Set to 300
	OCR1BL = 44;
	TCNT1H = 0;
	TCNT1L = 0;           // Reset couny
	TCCR1B = _BV(CS10);   // Restart counting
	PORTD &= ~_BV(PD0);   // Trigger monitor
	TIFR1 = 0xff;         // Clear flags.

	// Busy-wait for overflow, after 2 matches.

	while (!(TIFR1 & _BV(TOV1)))
            ;

	// Waggle for monitor

	PORTD |= _BV(PD0);

	TIFR1 = 0xff;         // Clear flags.

	// Now change pulse width.

	OCR1BH = 1  ;         // Set to 400
	OCR1BL = 144;         // Should take effect on overflow.

 	// Busy-wait again

	while (!(TIFR1 & _BV(TOV1)))
            ;
	
	/**** Start phase-correct 10-bit. ****/

	TCCR1B = 0;
	// WGM = 3, PC, 10-bit
	TCCR1A = _BV(WGM11) + _BV(WGM10) + _BV(COM1B1) + _BV(COM1B0);
	OCR1BH = 1;           // Set to 500
	OCR1BL = 244;
	TCNT1H = 1;
	TCNT1L = 144;         // Start with count of 400
	TCCR1B = _BV(CS10);   // Restart counting
	PORTD &= ~_BV(PD0);   // Trigger monitor
	TIFR1 = 0xff;         // Clear flags.

	// Busy-wait for 2 matches.

	while (!(TIFR1 & _BV(OCF1B)))
		;
	TIFR1 = 0xff;         // Clear flags.
	while (!(TIFR1 & _BV(OCF1B)))
		;
	TIFR1 = 0xff;         // Clear flags.

	OCR1BH = 0;           // Set to 100
	OCR1BL = 100;         // Should take effect on overflow.

	// Busy-wait for 2 matches.

	while (!(TIFR1 & _BV(OCF1B)))
		;
	TIFR1 = 0xff;         // Clear flags.
	while (!(TIFR1 & _BV(OCF1B)))
		;

	/**** Start CTC, OCRA at top. ****/

	TCCR1B = 0;
	// WGM = 4, CTC/OCRA
	TCCR1A = _BV(COM1A0); // Toggle at TOP
	OCR1AH = 1;           // Set to 500
	OCR1AL = 244;
	TCNT1H = 0;
	TCNT1L = 0;
	// WGM = 4, CTC/OCRA, scale = 1.
	TCCR1B = _BV(WGM12) + _BV(CS10);   // Restart counting

	TIFR1 = 0xff;         // Clear flags.
	DDRA = 0;             // Waste some time
	DDRB = 0xff;

	// Set the count while it is running.

	TCNT1L = 0xff;
	PORTD |= _BV(PD0);   // Trigger monitor

	// Busy-wait for matches.

	while (!(TIFR1 & _BV(OCF1A)))
		;
	TIFR1 = 0xff;         // Clear flags.
	while (!(TIFR1 & _BV(OCF1A)))
		;
	TIFR1 = 0xff;         // Clear flags.

	OCR1AH = 0;           // Set to 100
	OCR1AL = 100;         // Immediate effect

	// Busy-wait for match.

	while (!(TIFR1 & _BV(OCF1A)))
		;
	TIFR1 = 0xff;         // Clear flags.
	while (!(TIFR1 & _BV(OCF1A)))
		;

	// Sleeping with interrupt off is interpreted by simavr as "exit please".

	sleep_cpu();
}
