/*
	atmega328p_demo_blink.c

	Copyright 2021 Giles Atkinson

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


/* There are two options, for GPIO ports and ADC.  Repeatedly pressing the
 * the PORTD, bit 0 button first reads input from ports B and C, then adds
 * the two numbers, showing the result in C.  Two button presses make a
 * cycle.  PORTD, bit 1 is similar, but uses B and C to display the ADC
 * input on channel 1.
 */

#ifdef F_CPU
#undef F_CPU
#endif

#define F_CPU 16000000

#include <avr/io.h>

#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega328p");

AVR_MCU_VCD_PORT_PIN('B', 0, "PORTB/0");
AVR_MCU_VCD_PORT_PIN('B', 1, "PORTB/1");
AVR_MCU_VCD_PORT_PIN('B', 2, "PORTB/2");
AVR_MCU_VCD_PORT_PIN('B', 3, "PORTB/3");
AVR_MCU_VCD_PORT_PIN('B', 4, "PORTB/4");
AVR_MCU_VCD_PORT_PIN('B', 5, "PORTB/5");
AVR_MCU_VCD_PORT_PIN('B', 6, "PORTB/6");
AVR_MCU_VCD_PORT_PIN('B', 7, "PORTB/7");

AVR_MCU_VCD_PORT_PIN('C', 0, "PORTC/0");
AVR_MCU_VCD_PORT_PIN('C', 1, "PORTC/1");
AVR_MCU_VCD_PORT_PIN('C', 2, "PORTC/2");
AVR_MCU_VCD_PORT_PIN('C', 3, "PORTC/3");
AVR_MCU_VCD_PORT_PIN('C', 4, "PORTC/4");
AVR_MCU_VCD_PORT_PIN('C', 5, "PORTC/5");
AVR_MCU_VCD_PORT_PIN('C', 6, "PORTC/6");
AVR_MCU_VCD_PORT_PIN('C', 7, "PORTC/7");

int main()
{
    /* Low bit of DDRC is action button, others disabled. */

    DDRD = 0xfc;
    for (;;) {
        uint8_t oldd, result;

        /* Read B and C and show their sum in C. */

        DDRC = DDRB = 0;
        PORTD = 0xfc;
        oldd = PIND;
        while (PIND == oldd)
            ;
        if ((oldd ^ PIND) & 1) {
            /* Button 0: add numbers from B and C, result in C. */

            PORTD = 0;
            result = PINB + PINC;
            DDRC = DDRB = 0xff;
            PORTB = 0;
            PORTC = result;
        } else {
            /* ADC conversion of ADC1. */

            PORTD = 0;
            DDRC = DDRB = 0xff;
            PORTB = PORTC = 0;
            ADMUX = 0xc1; // 1.1V ref, ADC 1. */
            ADCSRA = (1 << ADEN) + (1 << ADSC) + (1 << ADIF); // Scaling = 0.
            while ((ADCSRA & (1 << ADIF)) == 0)
                ;
            PORTC = ADCL;
            PORTB = ADCH;
        }

        /* Wait for button to proceed. */

        PORTD = 0xf0;
        oldd = PIND;
        while (PIND == oldd)
            ;
        PORTD = 0;
    }
}
