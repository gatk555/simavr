/*
	test_atmega32_lazy_test.c
        Test for lazy external simulation support in simavr.
        Lazy simulation support means allowing the program that is using
        simavr to produce values on the AVR input pins only when
        the firmware reads them.

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
#include <stdlib.h>
#include <string.h>

#include "tests.h"
#include "sim_core.h"
#include "avr_ioport.h"
#include "avr_adc.h"

/* Start of the ADC and PORTB's IRQ lists. */

static avr_irq_t *adc_base_irq, *portb_base_irq;

/* These variables contol the lazy external simulation. */

static int Step, Loaf;

/* Callback for A-D conversion sampling. */

static void conversion(struct avr_irq_t *irq, uint32_t value, void *param)
{
    /* Tell the ADC there is one volt on pin ADC1. */

    avr_raise_irq(adc_base_irq + ADC_IRQ_ADC1, 1000 + Step * 100);
    if (++Step > 1) {
        avr_t *avr = (avr_t *)param;

        /* Stop firmware execution.  run_avr() will handle this. */

        avr->state = cpu_Stopped;
        Loaf = 1;
        return;
    }
}

/* Callback for PORTB reads. */

static void send_char_to_portb(int c)
{
    int i, bit;

    /* Values are supplied as individual bits. */

    for (i = 0; i < 8; ++i, c >>= 1) {
        bit = c & 1;

        /* Push bit into simavr. The first 8 IRQs set individual bits. */

        avr_raise_irq(portb_base_irq + i, bit);
    }
}

static void portb_read_notify(avr_irq_t *irq, uint32_t value, void *param)
{
    static avr_cycle_count_t previous_cycles;

    if (++Step == 3) {
        /* Supply new value. */

        send_char_to_portb('A');

    } else {
        avr_t *avr = (avr_t *)param;

        /* Intercept every second attempt, others are retries. */

        if (!(Step & 1)) {
            /* Stop firmware execution.  run_avr() will handle this. */

            avr_fault_current(avr);
            Loaf = 1;

            /* Record cycle count. */

            previous_cycles = avr->cycle;
        } else {
            /* On retry, the cycle count should not have changed. */

            if (previous_cycles != avr->cycle) {
                fail("Unexpected cycle counts: %lu/%lu\n",
                     avr->cycle, previous_cycles);
            }
        }
    }
}

/* This function controls the execution of the firmware.
 * It is called in a loop until done.
 */

static int run_avr(avr_t *avr)
{
    if (avr->state == cpu_Running)
        avr->pc = avr_run_one(avr);

    if (Loaf) {
        int  expected_state;

        /* Lazy action required. In actual use there would be a return here,
         * with the new input injected before re-entering the run function.
         */

        Loaf = 0;

        switch (Step) {
        case 2:
            expected_state = cpu_Stopped;

            /* Set an new ADC input and request it is sampled. */

            avr_raise_irq(adc_base_irq + ADC_IRQ_ADC1, 2000);
            avr_raise_irq(adc_base_irq + ADC_IRQ_RESAMPLE, 0);
            break;
        case 4:
            expected_state = cpu_Running;

            /* Set new PORTB input and restart. */

            send_char_to_portb('O');
            break;
        case 6:
            expected_state = cpu_Running;

            /* Set new PORTB bit and restart. */

            avr_raise_irq(portb_base_irq + 5, 1);
            break;
        case 8:
            expected_state = cpu_Running;

            /* Set new PORTB bit and restart. */

            avr_raise_irq(portb_base_irq + 6, 0);
            break;
        default:
            fail("Unexpected stop at step %d.", Step);
            break;
        }
        if (avr->state != expected_state)
            fail("Unexpected processor state %d at step %d.\n",
                 avr->state, Step);
        avr->state = cpu_Running; // Restart.
    }

    if (avr->state == cpu_Sleeping && !avr->sreg[S_I]) {
        printf("simavr: sleeping with interrupts off, quitting gracefully\n");
        avr_terminate(avr);
        fail("Test case error: special_deinit() returned?");
        exit(0);
    }

    /* Run cycle timers, there are no interrupts or sleep. */

    avr_cycle_timer_process(avr);
    return avr->state;
}

/* This test has some of the support code (tests.c) in-line, as
 * it needs to directly control firmware execution.
 */

int main(int argc, char **argv) {
    static const char    *expected = "399 799 A O X Z";
    struct output_buffer  buf;
    avr_t                *avr;
    int                   reason, good;

    tests_init(argc, argv);
    avr = tests_init_avr("atmega32_lazy_test.axf");

    /* Set-up reception of test results from firmware. */

    init_output_buffer(&buf);
    avr_register_io_write(avr, (avr_io_addr_t)0x2c,  // &UDR
                          reg_output_cb, &buf);

    /* Request callback when a value is sampled by the ADC for conversion. */

    adc_base_irq = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, 0);
    avr_irq_register_notify(adc_base_irq + ADC_IRQ_OUT_TRIGGER,
                            conversion, avr);
    avr_raise_irq(adc_base_irq + ADC_IRQ_ADC1, 100); // Input is 100 mV

    /* Do the same for Port B. */

    portb_base_irq = avr_io_getirq(avr,
                                   (uint32_t)AVR_IOCTL_IOPORT_GETIRQ('B'),
                                   0);
    send_char_to_portb('h'); // This will be overwritten.
    avr_irq_register_notify(portb_base_irq + IOPORT_IRQ_REG_PIN,
                            portb_read_notify,
                            avr);

    /* Run program and check results. */

    reason = tests_run_test(avr, 100000, run_avr);
    good = !strcmp(expected, buf.str);

    if (reason == LJR_CYCLE_TIMER) {
        if (good) {
            _fail(NULL, 0, "Simulation did not finish in time. "
                  "Output is correct and complete.");
        } else {
            _fail(NULL, 0, "Simulation did not finish in time. "
                  "Output so far: \"%s\"", buf.str);
        }
    } else if (!good) {
        _fail(NULL, 0, "Outputs differ: expected \"%s\", got \"%s\"",
              expected, buf.str);
    }
    tests_success();
    return 0;
}
