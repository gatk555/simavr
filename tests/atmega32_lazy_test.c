/*
	atmega32_lazy_test.c
        Test for lazy external simulation support in simavr.

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

#include <stdio.h>

#include <avr/io.h>
#include <avr/sleep.h>

#include "avr_mcu_section.h"
AVR_MCU(F_CPU, "atmega32");
AVR_MCU_VOLTAGES(3300, 3300, 3300);	// 3.3V VCC, AVCC, VREF

static int uart_putchar(char c, FILE *stream) {
    if (c == '\n')
        uart_putchar('\r', stream);
    loop_until_bit_is_set(UCSRA, UDRE);
    UDR = c;
    return 0;
}

static FILE mystdout = FDEV_SETUP_STREAM(uart_putchar, NULL,
                                         _FDEV_SETUP_WRITE);

int main(void)
{
    uint16_t  result;

    stdout = &mystdout;

    /* Set-up the ADC and wait for conversion complete. */

    ADMUX = (1 << REFS0) | (1 << REFS1) | 1; // 2.56V ref, input ADC1.
    ADCSRA = (1 << ADEN) | (1 << ADSC) | 6;  // Start with pre-scale = 6.

    while (ADCSRA & (1 << ADSC))
        ;

    result = ADCL;
    printf("%d", (ADCH << 8) + result);

    /* Do it again, this will be handled the lazy way. */

    ADCSRA = (1 << ADEN) | (1 << ADSC) | 6;  // Start with pre-scale = 6.

    while (ADCSRA & (1 << ADSC))
        ;

    result = ADCL;
    printf(" %d", (ADCH << 8) + result);

    ADCSRA = 0;

    /* Read Port B twice.  Second time uses avr_fault_current(). */

    printf(" %c", PINB);
    printf(" %c", PINB);

    /* These may generate the SBIS or SBIC instruction. */

    printf(" %c", (PINB & 0x20) ? 'X' : 'Y');
    printf(" %c", (PINB & 0x40) ? 'W' : 'Z');
    
    sleep_cpu(); // Run function will stop simulation.
}

