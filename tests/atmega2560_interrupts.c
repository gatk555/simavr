/*
	atmega2560_interrupts.c

	Test for interrupt simulation.  This AVR code just monitors the
	interrupts and reports back,  They are artificialy raised.
 */

#include <stdio.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#include "avr_mcu_section.h"

/*
 * This demonstrate how to use the avr_mcu_section.h file
 * The macro adds a section to the ELF file with useful
 * information for the simulator
 */
AVR_MCU(F_CPU, "atmega2560");

static int uart_putchar(char c, FILE *stream)
{
	if (c == '\n')
		uart_putchar('\r', stream);
	loop_until_bit_is_set(UCSR3A, UDRE3);
	UDR3 = c;
	return 0;
}

static FILE mystdout = FDEV_SETUP_STREAM(uart_putchar, NULL,
                                         _FDEV_SETUP_WRITE);

volatile uint8_t interrupts;
volatile char    buffer[56];

#define TISR(n) \
ISR(_VECTOR(n)) \
{ \
    buffer[n] = ' ' + ++interrupts; \
}

TISR(1)
TISR(2)
TISR(3)
TISR(4)
TISR(5)
TISR(6)
TISR(7)
TISR(8)
TISR(9)
TISR(10)
TISR(11)
TISR(12)
TISR(13)
TISR(14)
TISR(15)
TISR(16)
TISR(17)
TISR(18)
TISR(19)
TISR(20)
TISR(21)
TISR(22)
TISR(23)
TISR(24)
TISR(25)
TISR(26)
TISR(27)
TISR(28)
TISR(29)
TISR(30)
TISR(31)
TISR(32)
TISR(33)
TISR(34)
TISR(35)
TISR(36)
TISR(37)
TISR(38)
TISR(39)
TISR(40)
TISR(41)
TISR(42)
TISR(43)
TISR(44)
TISR(45)
TISR(46)
TISR(47)
TISR(48)
TISR(49)
TISR(50)
TISR(51)
TISR(52)
TISR(53)

volatile uint8_t done = 0;

ISR(_VECTOR(54))
{
    buffer[54] = ' ' + ++interrupts;
    done = 1;
}

int main()
{
    uint8_t count;

    stdout = &mystdout;
    for (count = 0; count < sizeof buffer; ++count)
        buffer[count] = ' ';
    buffer[(sizeof buffer) - 1] = '\0';

    // Control program will trap early and raise interrputs.

    count = 0;
    sei();

    while (!done && count < 60)
        ++count;

    cli();
    fputs((const char *)buffer, stdout);
    printf("| %d\n", count);

    // this quits the simulator, since interupts are off
    // this is a "feature" that allows running tests cases and exit

    sleep_cpu();
}
