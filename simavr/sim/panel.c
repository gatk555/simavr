/*
	panel.c: connect simavr to the Blink panel library
        and show a control panel for the simulation.

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
#include "avr_adc.h"

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

/* Blink uses these to control running of the simulator. */

static struct run_control Brc;
static int Burst_preset;                                // For stop/start

/* Blink library function table and initialisation argument. */

static struct blink_functs *Bfp;

static int push_val(Sim_RH handle, unsigned int value);
static const struct simulator_calls blink_callbacks =
    {.sim_push_val = push_val};

/* For ADC input. */

#define ADC_CHANNEL_COUNT 16

static avr_irq_t   *ADC_base_irq;
static int          ADC_sor;
static unsigned int ADC_update_chan = ADC_CHANNEL_COUNT;
static Sim_RH       ADC_update_handle;

/* Blink handles associated with ports. */

#define PORT_HANDLE(pp, i) (pp->handle_finder + i)

#define SOR     1       // Stop on read

/* Handles for non-port Blink items. */

#define PC_handle            ((Sim_RH)1)
#define Cycles_handle        ((Sim_RH)2)
#define ADC_input_1_handle   ((Sim_RH)3)
#define ADC_channel_1_handle ((Sim_RH)4)
#define ADC_input_2_handle   ((Sim_RH)5)
#define ADC_channel_2_handle ((Sim_RH)6)
#define ADC_SOR_handle       ((Sim_RH)7)

/* Ask Blink for the number of cycles to simulate. */

static void get_next_burst(void)
{
    do {
        Bfp->run_control(&Brc);
        if (ADC_update_chan < ADC_CHANNEL_COUNT) {
            uint32_t  input;

            /* Deferred ADC channel update. */

            input = ADC_base_irq[ADC_update_chan].value;
            Bfp->new_value(ADC_update_handle, input);
            ADC_update_chan = ADC_CHANNEL_COUNT; // Sentinel value.
        }
    } while (Brc.burst == 0);
}

/* Stop the simulation when some event occurs.  Argument is the
 * local handle for the control button for the cause of the stop.
 */

static avr_cycle_count_t burst_complete(struct avr_t      *avr,
                                        avr_cycle_count_t  when,
                                        void              *param);

static void stop_on_event(avr_t *avr, Sim_RH button)
{
    /* This cancels the previous end-of-burst callback and re-schedules it
     * for immediate execution.  That stops simavr from running.
     */

    avr_cycle_timer_register(avr, 0, burst_complete, NULL);

    /* Tell UI. */

    Bfp->stopped();                             /* Notify UI. */
    Bfp->new_flags(button, 1);                  /* Change lamp colour. */

    /* Get next execution burst from Blink. */

    get_next_burst();
    Burst_preset = 1;

    /* Revert SoR control lamp colour. */

    Bfp->new_flags(button, 0);                  /* Change lamp colour. */
}

/* Notification of reading from a GPIO port.  Enabled for Stop on Read. */

static void d_read_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    struct port *pp;

    pp = (struct port *)param;
    stop_on_event(pp->avr, PORT_HANDLE(pp, SOR));
}

/* ADC input is being read. */

static void adc_read_notify(struct avr_irq_t *irq, uint32_t value, void *param)
{
    union {
        avr_adc_mux_t mux;
        uint32_t      v;
    }         e;
    uint32_t  channel, input;

    /* Show the channel(s) being read and current value(s). */

    e.v = value;
    channel = e.mux.src;
    Bfp->new_value(ADC_channel_1_handle, channel);
    input = ADC_base_irq[channel].value;
    Bfp->new_value(ADC_input_1_handle, input);

    channel = e.mux.diff;
    Bfp->new_value(ADC_channel_2_handle, channel);
    input = ADC_base_irq[channel].value;
    Bfp->new_value(ADC_input_2_handle, input);

    if (ADC_sor) {
        /* Stop so that the entries can be changed. */

        stop_on_event((avr_t *)param, ADC_SOR_handle);
    }
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

/* New port bits from Blink. */

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

static int push_val(Sim_RH handle, unsigned int value)
{
    struct port  *pp, **ppp;
    uintptr_t     handle_type;

    handle_type = (uintptr_t)handle;
    if (handle_type <= 'Z') {
        static int adc_chan_1, adc_chan_2;

        switch (handle_type) {
        case 1:
            fprintf(stderr, "Changed PC!\n");
            break;
        case 2:
            fprintf(stderr, "Changed cycle count!\n");
            break;
        case 3:
            avr_raise_irq(ADC_base_irq + adc_chan_1, value);
            break;
        case 4:
            if (value < ADC_CHANNEL_COUNT) {
                adc_chan_1 = value;
                ADC_update_chan = value;
                ADC_update_handle = ADC_input_1_handle;
            }
            break;
        case 5:
            avr_raise_irq(ADC_base_irq + adc_chan_2, value);
            break;
        case 6:
            if (value < ADC_CHANNEL_COUNT) {
                adc_chan_2 = value;
                ADC_update_chan = value;
                ADC_update_handle = ADC_input_2_handle;
            }
            break;
        case 7:
            /* Stop on read. */

            ADC_sor = value;
            break;
        }

        /* Immediate return from Blink_run_control() if ADC channel changed.*/

        return ADC_update_chan < ADC_CHANNEL_COUNT;
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

static void show_adc(void)
{
    Blink_RH   row;

    row = Bfp->new_row("ADC");
    Bfp->add_register("mV", ADC_input_2_handle, 13, RO_STYLE_DECIMAL, row);
    Bfp->add_register("Channel -", ADC_channel_2_handle, 4,
                      RO_STYLE_SPIN, row);
    Bfp->add_register("mV", ADC_input_1_handle, 13, RO_STYLE_DECIMAL, row);
    Bfp->add_register("Channel +", ADC_channel_1_handle, 4,
                      RO_STYLE_SPIN, row);
    Bfp->add_register("SoR", ADC_SOR_handle, 1, RO_ALT_COLOURS, row);
    Bfp->close_row(row);
}

/* Set-up the Blink panel library in run_avr, and run the simulator.
 * Return 0 on failure, 1 for success.
 */

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
    }

    /* ADC set-up. */

    ADC_base_irq = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, 0);
    avr_irq_register_notify(ADC_base_irq + ADC_IRQ_OUT_TRIGGER,
                            adc_read_notify, avr);
    show_adc();

    /* Run. */

    do {
        if (Burst_preset) {
            Burst_preset = 0;
        } else {
            /* Get next execution burst from Blink. */

            get_next_burst();
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
