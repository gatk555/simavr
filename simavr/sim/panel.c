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

#define HANDLES_PER_PORT 2

struct port {
    avr_t       *avr;
    avr_irq_t   *base_irq;
    char         port_letter;
    uint8_t      output, ddr, actual;
    uint8_t      sor;                                   // Stop indicator.
    struct port *handle_finder[HANDLES_PER_PORT];       // See push_val().
};

static int Burst_preset;                                // For stop/start

/* Blink uses this to control running of the simulator. */

static struct run_control Brc;

/* Blink handles associated with ports. */

#define PORT_HANDLE(pp, i) (pp->handle_finder + i)

#define SOR     1       // Stop on read

/* Handles for non-port Blink items. */

#define PC_handle     ((Sim_RH)1)
#define Cycles_handle ((Sim_RH)2)

/* Notification of reading from a port.  Enabled for Stop on Read. */

static avr_cycle_count_t burst_complete(struct avr_t      *avr,
                                        avr_cycle_count_t  when,
                                        void              *param);

static void d_read_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    struct port *pp;

    pp = (struct port *)param;

    /* This cancels the previous end-of-burst callback and re-schedules it
     * for immediate execution.  That stops simavr from running.
     */

    avr_cycle_timer_register(pp->avr, 0, burst_complete, NULL);

    /* Tell UI. */

    Bfp->stopped();                             /* Notify UI. */
    Bfp->new_flags(PORT_HANDLE(pp, SOR), 1);    /* Change lamp colour. */

    /* Get next execution burst from Blink. */

    do
        Bfp->run_control(&Brc);
    while (Brc.burst == 0);
    Burst_preset = 1;

    /* Revert SoR control lamp colour. */

    Bfp->new_flags(PORT_HANDLE(pp, SOR), 0);    /* Change lamp colour. */
}

/* Notification of output to a port. This is called whenever the output
 * or data direction registers are written.  Writing to the input register
 * to toggle output register bits is handled internally by simavr
 * and appears as an output register change.
 *
 * New pin values are calculated ignoring pullups, including simavr's
 * external pullup feature - the panel provides "strong" inputs.
 */

static void d_out_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    struct port *pp;
    uint8_t      ddr, out;

    pp = (struct port *)param;
    if (irq->irq == IOPORT_IRQ_DIRECTION_ALL) {
        Bfp->new_flags(PORT_HANDLE(pp, 0), ~value);
        ddr = pp->ddr = (uint8_t)value;
        out = pp->output;
    } else {
        ddr = pp->ddr;
        out = pp->output = (uint8_t)value;
    }
    pp->actual = (out & ddr) | (pp->actual & ~ddr);
    Bfp->new_value(PORT_HANDLE(pp, 0), pp->actual);
}

static void port_input(struct port *pp, unsigned int value)
{
    unsigned int  changed, mask, dirty, i;

    changed = value ^ pp->actual;
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
                    "Dirty write %02x to port %c (DDR %02x actual %02x).",
                    value, pp->port_letter, pp->ddr, pp->actual);
        }
        pp->actual = value;
    }
}

/* Function called by Blink with new input values. */

int push_val(Sim_RH handle, unsigned int value)
{
    struct port  *pp, **ppp;
    unsigned int  handle_type;

    if ((uintptr_t)handle <= 'Z') {
        /* Attempt to change PC or DDR. Ignore.  FIX ME */

        if (handle == PC_handle)
            fprintf(stderr, "Changed PC!\n");
        else
            fprintf(stderr, "Changed cycle count!\n");
        return 0;
    }

    /* Other handles are associated with a port. */

    ppp = (struct port **)handle;
    pp = *ppp;
    handle_type = ppp - pp->handle_finder;
    switch (handle_type) {
    case 0:
        /* New input value for port. */

        port_input(pp, value);
        break;
    case SOR:
        /* Stop on read. */

        if (value && !pp->sor) {
            avr_irq_register_notify(pp->base_irq + IOPORT_IRQ_REG_PIN,
                                    d_read_notify, pp);
            pp->sor = 1;
        } else if (!value && pp->sor) {
            avr_irq_unregister_notify(pp->base_irq + IOPORT_IRQ_REG_PIN,
                                      d_read_notify, pp);
            pp->sor = 0;
        }
        break;
    }
    return 0;
}

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
    Blink_RH   row;
    char       name_buff[8];

    sprintf(name_buff, "PORT%c", port_letter);
    row = Bfp->new_row(name_buff);
    Bfp->add_register(name_buff, PORT_HANDLE(pp, 0), 8,
                      RO_SENSITIVITY | RO_ALT_COLOURS, row);
    Bfp->add_register("SoR", PORT_HANDLE(pp, SOR), 1, RO_ALT_COLOURS, row);
    Bfp->close_row(row);
    Bfp->new_flags(PORT_HANDLE(pp, 0), 0xff);  /* All inputs - inverted. */
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

    if (!Bfp->init("simavr", (struct simulator_calls *)&blink_callbacks))
        return 0;

    /* Display simulated PC and cycle count. */

    row = Bfp->new_row("AVR");
    Bfp->add_register("Cycles", Cycles_handle, 32,
                      RO_INSENSITIVE | RO_STYLE_DECIMAL, row);
    Bfp->add_register("PC", PC_handle, 20,
                      RO_INSENSITIVE | RO_STYLE_HEX, row);
    Bfp->close_row(row);

    /* Scan the AVR for GPIO ports and create a display register of each. */

    for (port_letter = 'A'; port_letter <= 'Z'; ++port_letter) {
        avr_irq_t *base_irq;
        uint32_t   ioctl, i;

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
            for (i = 0; i < HANDLES_PER_PORT; ++i)      // See push_val().
                pp->handle_finder[i] = pp;
            avr_irq_register_notify(base_irq + IOPORT_IRQ_REG_PORT,
                                    d_out_notify, pp);
            avr_irq_register_notify(base_irq + IOPORT_IRQ_DIRECTION_ALL,
                                    d_out_notify, pp);
#if 0
            avr_irq_register_notify(base_irq + IOPORT_IRQ_PIN_ALL,
                                    d_in_notify, pp);
#endif

            /* Display the port and DDR for now. */

            port_reg(port_letter, pp);
        }
        /* Do analogue input read.  FIX ME. */
    }

    do {
        if (Burst_preset) {
            Burst_preset = 0;
        } else {
            /* Get next execution burst from Blink. */

            do
                Bfp->run_control(&Brc);
            while (Brc.burst == 0);
        }

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
