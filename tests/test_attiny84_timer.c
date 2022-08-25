/*
	test_attiny84_timer.c

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

#include <stdio.h>
#include <stddef.h>
#include "tests.h"
#include "avr_ioport.h"

/* Start of the IOPORT's IRQ list. */

static avr_irq_t *base_irq;

/* Address of TCNT0 */

#define COUNTER_OFFSET 0x52

#define PRESCALE 64

static avr_cycle_count_t base;
static int               step;
static struct {uint32_t cycle, value, next; } tests[] = {
	// Timer in "normal" mode.
	{10, 10}, {250, 250}, {256, 0}, {517, 5, 1},
	// Timer in CTC mode with 23 clocks/cycle.
	{10, 10}, {23, 0}, {252, 22, 1},
	// Phase-correct PWM with TOP == 255.
	{10, 10}, {254, 254}, {255, 255}, {256, 254}, {259, 251}, {260, 250}, 
	{509, 1}, {510, 0}, {511, 1}, {765, 255}, {766, 254}, {1530, 0, 1},
	// Fast PWM with TOP == 255.
	{10, 20}, {244, 254}, {245, 255}, {246, 0}, {300, 54, 1},
	{0}
};

#define TEST_COUNT ((sizeof tests / sizeof tests[0]) - 1)

/* Timer function to initate counter reads. */

static avr_cycle_count_t tickle(avr_t *avr, avr_cycle_count_t when, void *p)
{
	/* Set PA1 to signal firmware. */

	avr_raise_irq(base_irq + IOPORT_IRQ_PIN1, 1);
	return base + tests[step + 1].cycle * PRESCALE; // Prescalar is 64
}

/* This is called when PA0 is changed by the firmware. */

static void monitor(struct avr_irq_t *irq, uint32_t value, void *param)
{
	static int  restart = 1;
	avr_t      *avr = (avr_t *)param;
	uint8_t     counter;

	if (!(value & 1)) // Rising edges only
		return;
	if (restart) {
		/* Firmware is signalling restart. */

		base = avr->cycle;
		avr_raise_irq(base_irq + IOPORT_IRQ_PIN2, 0); // Clear proceed signal.
		avr_cycle_timer_register(avr, tests[step].cycle * PRESCALE,
								 tickle, NULL);
		restart = 0;
		return;
	}

	/* The test program has just read the TCNT0 register. */

	avr_raise_irq(base_irq + IOPORT_IRQ_PIN1, 0); // Reset PA2 signal
	counter = avr->data[COUNTER_OFFSET];
	if (counter != tests[step].value) {
		fail("Counter register was %d (expected %d) at step %d\n",
			 counter, tests[step].value, step);
	}

	if (tests[step++].next) {
		/* Tell the firmware to continue. */

		avr_raise_irq(base_irq + IOPORT_IRQ_PIN2, 1);
		restart = 1;
	}
}

int main(int argc, char **argv) {
	avr_t             *avr;

	tests_init(argc, argv);
	avr = tests_init_avr("attiny84_timer.axf");
	base_irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('A'), 0);
	avr_irq_register_notify(base_irq + IOPORT_IRQ_PIN0, monitor, avr);
	tests_run_avr(avr, 30000);
	if (step != TEST_COUNT)
		fail("Completed %d tests of %ld\n", step, TEST_COUNT);
	tests_assert_cycles_between(90000, 180000);
	tests_success();
	return 0;
}
