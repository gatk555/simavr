/*
	panel.c: connect simavr to the Blink panel library.

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

#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>

#include "blink/sim.h"
#include "sim_avr.h"
#include "sim_core.h"
#include "sim_irq.h"
#include "sim_cycle_timers.h"
#include "avr_ioport.h"

/* Blink library function table. */

static struct blink_functs *Bfp;

/* Data structures to track the simulated MCU's I/O ports. */

struct port {
    avr_t      *avr;
    avr_irq_t  *base_irq;
    char        port_letter;
    uint8_t     output, ddr, input;
};

/* Handles for Blink. */

#define PC_handle     ((Sim_RH)1)
#define Cycles_handle ((Sim_RH)2)

/* Notification of output to a port. This is called whenever the output
 * or data direction registers are written.  Writing to the input register
 * to toggle output register bits is handled internally by simavr
 * and appears as an output register change.
 *
 * New outputs are calculated ignoring pullups, including simavr's
 * external pullup feature - the panel provides "strong" inputs.
 */

static void d_out_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    struct port *pp;
    uint8_t      ddr, out;

    pp = (struct port *)param;
    if (irq->irq == IOPORT_IRQ_DIRECTION_ALL) {
        Bfp->new_flags(pp, ~value);
        ddr = pp->ddr = (uint8_t)value;
        out = pp->output;
    } else {
        ddr = pp->ddr;
        out = pp->output = (uint8_t)value;
    }
    Bfp->new_value(pp, (out & ddr) | (pp->input & ~ddr));
}

/* Function called by Blink with new input values. */

int push_val(Sim_RH handle, unsigned int value)
{
    struct port  *pp;
    unsigned int  changed, mask, dirty, i;

    if ((uintptr_t)handle <= 'Z') {
        /* Attempt to change PC or DDR. Ignore.  FIX ME */

        if (handle == PC_handle)
            fprintf(stderr, "Changed PC!\n");
        else
            fprintf(stderr, "Changed DDR%c!\n", (int)(uintptr_t)handle);
        return 0;
    }
    pp = (struct port *)handle;
    changed = value ^ pp->input;
    if (changed) {
        /* Scan for changed bits. */

        dirty = 0;
        for (i = 0, mask = 1; i < 8; ++i, mask <<= 1) {
            if (mask & changed) {
                if (mask & pp->ddr) {
                    /* Trying to modify bit set as simulator output. */

                    dirty = 1;
                } else {
                    int bit;

                    bit = ((value & mask) != 0);

                    /* Push changed bit into simavr.
                     * The first 8 IRQs set individual bits.
                     */

                    avr_raise_irq(pp->base_irq + i, bit);
                }
                if (!(changed ^= mask))
                    break;
            }
        }
        if (dirty) {
            /* Rare. Do not try to correct this: deadlock danger. */

            fprintf(stderr,
                    "Dirty write %02x to port %c (DDR %02x input %02x).",
                    value, pp->port_letter, pp->ddr, pp->input);
        }
        pp->input = value;
    }
    return 0;
}

/* Blink uses this to control running of the simulator. */

static struct run_control Brc;
static int burst_done;

/* The simulator calls back here after each burst of execution. */

static avr_cycle_count_t burst_complete(struct avr_t      *avr,
                                        avr_cycle_count_t  when,
                                        void              *param)
{
    burst_done = 1;
    return 0;
}

/* Run the simulation.  Code lifted from simavr/tests/tests.c. */

static int my_avr_run(avr_t *avr)
{
    avr_cycle_count_t sleep;
    uint16_t          new_pc;

    if (avr->state == cpu_Stopped)
        return avr->state;

    new_pc = avr->pc;
    if (avr->state == cpu_Running)
        new_pc = avr_run_one(avr);

    // Run the cycle timers, get the suggested sleep time
    // until the next timer is due.

    sleep = avr_cycle_timer_process(avr);
    avr->pc = new_pc;

    if (avr->state == cpu_Sleeping) {
        if (!avr->sreg[S_I]) {
            printf("simavr: sleeping with interrupts off, quitting.\n");
            avr_terminate(avr);
            return cpu_Done;
        }
        avr->cycle += 1 + sleep;
    }

    // Interrupt servicing might change the PC too, during 'sleep'

    if (avr->state == cpu_Running || avr->state == cpu_Sleeping)
        avr_service_interrupts(avr);

    // if we were stepping, use this state to inform remote gdb

    return avr->state;
}

/* Create a displayed register for a port. */

static void port_reg(char port_letter, struct port *pp)
{
        char       name_buff[8];

        sprintf(name_buff, "PORT%c", port_letter);
        Bfp->new_register(name_buff, pp, 8, RO_SENSITIVITY | RO_ALT_COLOURS);
        Bfp->new_flags(pp, 0xff);  /* Default case as flags are inverted. */
}

/* Set-up for the Blink panel library in run_avr.
 * Return 0 on failure, 1 for success.
 */

static const struct simulator_calls blink_callbacks =
    {.sim_push_val = push_val};

int Run_with_panel(avr_t *avr)
{
    void        *handle;
    struct port *pp;
    Blink_RH     row;
    int          state;
    char         port_letter;

    /* Load the Blink library. This is a dynamic load because Blink
     * should not be a hard pre-requisite for simavr.
     */

    handle = dlopen("libblink.so", RTLD_LAZY);
    if (!handle) {
        fprintf(stderr,
                "Failed to load libblink.so. Try setting LD_LIBRARY_PATH."
                "\n%s\n",
                dlerror());
        return 0;
    }

    Bfp = (struct blink_functs *)dlsym(handle, "Blink_FPs");
    if (Bfp == NULL) {
        fprintf(stderr, "%s\n", dlerror());
        return 0;
    }

    if (!Bfp->init((struct simulator_calls *)&blink_callbacks))
        return 0;

    /* Display simulated PC. */

//    Bfp->new_register("PC", (Sim_RH)1, 16, RO_INSENSITIVE);
    row = Bfp->new_row("PC/Cycles");
    Bfp->add_register("Cycles", Cycles_handle, 32,
                      RO_INSENSITIVE | RO_STYLE_DECIMAL, row);
    Bfp->add_register("PC", PC_handle, 20,
                      RO_INSENSITIVE | RO_STYLE_HEX, row);
    Bfp->close_row(row);

    /* Scan the AVR for GPIO ports and create a display register of each. */

    for (port_letter = 'A'; port_letter <= 'Z'; ++port_letter) {
        avr_irq_t *base_irq;
        uint32_t   ioctl;

        ioctl = (uint32_t)AVR_IOCTL_IOPORT_GETIRQ(port_letter);
        base_irq = avr_io_getirq(avr, ioctl, 0);

        if (base_irq != NULL) {
            /* Some port letters are skipped. */

            pp = calloc(1, sizeof *pp);
            if (!pp)
                return 0;
            pp->avr = avr;
            pp->base_irq = base_irq;
            pp->port_letter = port_letter;
            avr_irq_register_notify(base_irq + IOPORT_IRQ_REG_PORT,
                                    d_out_notify, pp);
            avr_irq_register_notify(base_irq + IOPORT_IRQ_DIRECTION_ALL,
                                    d_out_notify, pp);
#if 0
            avr_irq_register_notify(base_irq + IOPORT_IRQ_REG_PIN,
                                    d_in_notify, pp);
#endif

            /* Display the port and DDR for now. */

            port_reg(port_letter, pp);
        }
        /* Do analogue input read.  FIX ME. */
    }

    do {
        do
            Bfp->run_control(&Brc);
        while (Brc.burst == 0);

        /* Request callback on cycles performed. */

        avr_cycle_timer_register(avr, Brc.burst, burst_complete, NULL);

        /* Run the simulation.  Stops on fatal error or endless sleep. */

        burst_done = 0;
        do
            state = my_avr_run(avr);
        while (!burst_done && state < cpu_Done);
        Bfp->new_value(PC_handle, avr->pc);         // Updated PC.
        Bfp->new_value(Cycles_handle, avr->cycle);  // Updated cycle count.
    } while (state < cpu_Done);
    return 1;
}
