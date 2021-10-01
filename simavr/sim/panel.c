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
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/time.h>

#include "blink/sim.h"

#include "sim_avr.h"
#include "sim_core.h"
#include "sim_irq.h"
#include "sim_elf.h"
#include "sim_cycle_timers.h"
#include "sim_vcd_file.h"
#include "avr_ioport.h"
#include "avr_adc.h"
#include "sim_core_config.h"

static avr_t *The_avr;  // Ugly

/* Data structures to track the simulated MCU's I/O ports. */

#define HANDLES_PER_PORT 3

struct port {
    avr_t       *avr;
    avr_irq_t   *base_irq;
    char         port_letter, vcd_letter;
    uint8_t      output, ddr, actual;
    uint8_t      sor, sow;                              // Stop indicators.
    struct port *handle_finder[HANDLES_PER_PORT];       // See push_val().
};

/* Blink uses these to control running of the simulator. */

static struct run_control Brc;
static int Burst_preset;                                // For stop/start

/* Blink library function table and initialisation argument. */

static struct blink_functs *Bfp;

static int  push_val(Sim_RH handle, unsigned int value);
static void stop(void);
static const struct simulator_calls blink_callbacks =
    {.sim_push_val = push_val,
     .sim_done = stop,
    };

/* For ADC input. */

#define ADC_CHANNEL_COUNT 16

static avr_irq_t   *ADC_base_irq;
static int          ADC_sor;
static unsigned int ADC_chan_pos, ADC_chan_neg;

static unsigned int ADC_update_chan = ADC_CHANNEL_COUNT;
static Sim_RH       ADC_update_handle;
static char         ADC_vcd_letter;

/* Blink handles associated with ports. */

#define PORT_HANDLE(pp, i) (pp->handle_finder + i)

#define SOR     1       // Stop on read
#define SOW     2       // Stop on write

/* Handles for non-port Blink items. */

#define PC_handle              ((Sim_RH)1)
#define Cycles_handle          ((Sim_RH)2)
#define ADC_input_pos_handle   ((Sim_RH)3)
#define ADC_channel_pos_handle ((Sim_RH)4)
#define ADC_input_neg_handle   ((Sim_RH)5)
#define ADC_channel_neg_handle ((Sim_RH)6)
#define ADC_SOR_handle         ((Sim_RH)7)

/* Make handles useable as case labels. */

#define CS(x) ((intptr_t)(x))

/* File handle and timestamp for VCD recording of input. */

static FILE              *vcd_fh;
static long unsigned int  Last_stamp = (long unsigned int)-1;

/* Ugly way to avoid deadlock during VCD playback, perhaps the IRQ
 * mechanism should be modified to eliminate this.
 */

static int Blink_input_active;

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
    uint32_t  input;

    /* Show the channel(s) being read and current value(s). */

    e.v = value;
    if (e.mux.src != ADC_chan_pos) {
        ADC_chan_pos = e.mux.src;
        Bfp->new_value(ADC_channel_pos_handle, ADC_chan_pos);
        input = ADC_base_irq[ADC_chan_pos].value;
        Bfp->new_value(ADC_input_pos_handle, input);
    }

    if (e.mux.kind == ADC_MUX_DIFF && e.mux.diff != ADC_chan_neg) {
        ADC_chan_neg = e.mux.diff;
        Bfp->new_value(ADC_channel_neg_handle, ADC_chan_neg);
        input = ADC_base_irq[ADC_chan_neg].value;
        Bfp->new_value(ADC_input_neg_handle, input);
    }

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

static void d_out_notify(avr_irq_t *irq, uint32_t value, void *param)
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
    if (pp->sow)
        stop_on_event(pp->avr, PORT_HANDLE(pp, SOW));
}

/* Notification of a pin change.  Used to display VCD input, but will
 * also be called for output.
 */

static void vcd_in_notify(avr_irq_t *irq, uint32_t value, void *param)
{
    struct port *pp;
    uint8_t      mask;

    if (Blink_input_active)
        return;
    pp = (struct port *)param;
    mask = (1 << irq->irq);
    if ((mask & pp->ddr) == 0) {
        /* VCD input. */

        if (value)
            pp->actual |= mask;
        else
            pp->actual &= ~mask;
        Bfp->new_value(PORT_HANDLE(pp, 0), pp->actual);
    }
}

/* Similarly for the ADC. */

static void vcd_adc_in_notify(avr_irq_t *irq, uint32_t value, void *param)
{
    unsigned int channel;

    if (Blink_input_active)
        return;
    channel = irq->irq;
    Bfp->new_value(ADC_channel_pos_handle, channel);
    Bfp->new_value(ADC_input_pos_handle, value);
    if (channel == ADC_chan_neg)
        Bfp->new_value(ADC_input_neg_handle, value);
}

/* New port bits from Blink. */

static void port_input(struct port *pp, unsigned int value)
{
    unsigned int             changed, mask, dirty, i;

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
                    if (vcd_fh) {
                        avr_t             *avr;
                        long unsigned int  stamp;

                        /* Write it to VCD file. */

                        avr = pp->avr;
                        stamp = (avr->cycle * 100*1000*1000) / avr->frequency;
                        if (stamp != Last_stamp) {
                            fprintf(vcd_fh, "\n#%lu", stamp);
                            Last_stamp = stamp;
                        }
                        fprintf(vcd_fh, " %c%c",
                                bit + '0', pp->vcd_letter + i);
                    }
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
            pp->actual = (pp->output & pp->ddr) | (value & ~pp->ddr);
        } else {
            pp->actual = value;
        }
    }
}

/* Write analogue input value to VCD file. */

static void write_adc_vcd(unsigned int chan, unsigned int value)
{
    long unsigned int  stamp;

    stamp = (The_avr->cycle * 100*1000*1000) / The_avr->frequency;
    if (stamp != Last_stamp) {
        fprintf(vcd_fh, "\n#%lu", stamp);
        Last_stamp = stamp;
    }

    /* Using real, what is the correct VCD form for integers? */

    fprintf(vcd_fh, " r%u %c", value, ADC_vcd_letter + chan);
}

/* Function called by Blink with new input values. */

static int push_val(Sim_RH handle, unsigned int value)
{
    struct port  *pp, **ppp;
    uintptr_t     handle_type;

    Blink_input_active = 1;
    handle_type = (uintptr_t)handle;
    if (handle_type <= 'Z') {
        switch (handle_type) {
        case CS(PC_handle):
            fprintf(stderr, "Changed PC!\n");
            break;
        case CS(Cycles_handle):
            fprintf(stderr, "Changed cycle count!\n");
            break;
        case CS(ADC_input_pos_handle):
            avr_raise_irq(ADC_base_irq + ADC_chan_pos, value);
            if (ADC_chan_pos == ADC_chan_neg) {
                /* Update other entry field. */

                ADC_update_chan = ADC_chan_pos;
                ADC_update_handle = ADC_input_neg_handle;
            }
            if (vcd_fh)
                write_adc_vcd(ADC_chan_pos, value);
            break;
        case CS(ADC_channel_pos_handle):
            if (value < ADC_CHANNEL_COUNT) {
                ADC_chan_pos = value;
                ADC_update_chan = value;
                ADC_update_handle = ADC_input_pos_handle;
            }
            break;
        case CS(ADC_input_neg_handle):
            avr_raise_irq(ADC_base_irq + ADC_chan_neg, value);
            if (ADC_chan_pos == ADC_chan_neg) {
                /* Update other entry field. */

                ADC_update_chan = ADC_chan_pos;
                ADC_update_handle = ADC_input_pos_handle;
            }
            if (vcd_fh)
                write_adc_vcd(ADC_chan_neg, value);
            break;
        case CS(ADC_channel_neg_handle):
            if (value < ADC_CHANNEL_COUNT) {
                ADC_chan_neg = value;
                ADC_update_chan = value;
                ADC_update_handle = ADC_input_neg_handle;
            }
            break;
        case CS(ADC_SOR_handle):
            /* Stop on read. */

            ADC_sor = value;
            break;
        }

        /* Immediate return from Blink_run_control() if ADC channel changed.*/

        Blink_input_active = 0;
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
    case SOW:
        /* Stop on write. */

        pp->sow = value;
        break;
    }
    Blink_input_active = 0;
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

/* Clean-up function, called by simavr when simulation has finished. */

static void panel_close(avr_t *avr, void *data)
{
    if (vcd_fh)
        fclose(vcd_fh);
    vcd_fh = NULL;
}

/* Clean-up function, called when window closed. */

static void stop(void)
{
    avr_terminate(The_avr);
}

/* Open the VCD output file. */

static void start_vcd(avr_t *avr, elf_firmware_t *fwp,
                      const char *firmware)
{
    avr_vcd_t *vcd;
    time_t     now;
    int        len;
    char       fn_buf[256];


    /* File name is given VCD output file, less any 3-letter extension,
     * with "_input.vcd" appended.
     */

    vcd = avr->vcd;
    len = snprintf(fn_buf, sizeof fn_buf - 10, "%s", vcd->filename);
    if (len > 4 && fn_buf[len - 4] == '.')
        len -= 4;
    strcpy(fn_buf + len, "_input.vcd");
    vcd_fh = fopen(fn_buf, "w");
    if (!vcd_fh) {
        fprintf(stderr,
                "Failed to open file %s for recording panel input: %s\n",
                fn_buf, strerror(errno));
        return;
    }

    /* Request callback at simulation end. */

    avr->custom.deinit = panel_close;

    /* Write VCD file header. */

    time(&now);
    fprintf(vcd_fh, "$date %s$end\n", ctime(&now));
    fprintf(vcd_fh, "$version Simavr " CONFIG_SIMAVR_VERSION " $end\n");
    fprintf(vcd_fh,
            "$comment\n"
            "  Control panel input to %s for simavr processor %s.\n"
            "$end\n",
            firmware, fwp->mmcu);
    fprintf(vcd_fh, "$timescale 10ns $end\n$scope module EXTERNAL $end\n");
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
    Bfp->add_register("SoW", PORT_HANDLE(pp, SOW), 1, RO_ALT_COLOURS, row);
    Bfp->add_register("SoR", PORT_HANDLE(pp, SOR), 1, RO_ALT_COLOURS, row);
    Bfp->close_row(row);
    Bfp->new_flags(PORT_HANDLE(pp, 0), 0xff);  /* All inputs - inverted. */
}

static void show_adc(void)
{
    Blink_RH   row;

    row = Bfp->new_row("ADC");
    Bfp->add_register("mV", ADC_input_neg_handle, 13, RO_STYLE_DECIMAL, row);
    Bfp->add_register("Channel -", ADC_channel_neg_handle, 4,
                      RO_STYLE_SPIN, row);
    Bfp->add_register("mV", ADC_input_pos_handle, 13, RO_STYLE_DECIMAL, row);
    Bfp->add_register("Channel +", ADC_channel_pos_handle, 4,
                      RO_STYLE_SPIN, row);
    Bfp->add_register("SoR", ADC_SOR_handle, 1, RO_ALT_COLOURS, row);
    Bfp->close_row(row);
}

/* Set-up the Blink panel library in run_avr, and run the simulator.
 * Return 0 on failure, 1 for success.
 */

int Run_with_panel(avr_t *avr, elf_firmware_t *fwp, const char *firmware,
                   int vcd_input)
{
    void        *handle;
    struct port *pp;
    Blink_RH     row;
    int          state, len, i;
    char         port_letter, vcd_letter;
    char        *fwcp;
    char         wn[64];

    The_avr = avr;

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

    /* Work out the window name. */

    fwcp = strdup(firmware);
    len = snprintf(wn, sizeof wn, "%s: %s", fwp->mmcu, basename(fwcp));
    free(fwcp);
    if (len > 4 && wn[len - 4] == '.')
        wn[len - 4] = '\0';
    if (!Bfp->init(wn, (struct simulator_calls *)&blink_callbacks))
        return 0;

    /* Display simulated PC and cycle count. */

    row = Bfp->new_row("AVR");
    Bfp->add_register("Cycles", Cycles_handle, 32,
                      RO_INSENSITIVE | RO_STYLE_DECIMAL, row);
    Bfp->add_register("PC", PC_handle, 20,
                      RO_INSENSITIVE | RO_STYLE_HEX, row);
    Bfp->close_row(row);

    /* Check for VCD output. */

    if (avr->vcd) {
        start_vcd(avr, fwp, firmware);
        vcd_letter = '!';
    }

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

            /* If there is a VCD input file, connect to the IRQs
             * that will push its data into the simulation.
             */

            if (vcd_input) {
                for (i = 0; i < 8; ++i)
                    avr_irq_register_notify(base_irq + i, vcd_in_notify, pp);
            }

            /* VCD variable definitions.
             * The "readable" name contains a simavr ioctl.
             */

            if (vcd_fh && vcd_letter <= 120) {
                pp->vcd_letter = vcd_letter;
                for (i = 0; i < 8; ++i, ++vcd_letter) {
                    fprintf(vcd_fh, "$var wire 1 %c iog%c_%d $end\n",
                            vcd_letter, pp->port_letter, i);
                }
            } else {
                pp->vcd_letter = 0;
            }
        }
    }

    /* ADC set-up. */

    ADC_base_irq = avr_io_getirq(avr, AVR_IOCTL_ADC_GETIRQ, 0);
    if (ADC_base_irq) {
        avr_irq_register_notify(ADC_base_irq + ADC_IRQ_OUT_TRIGGER,
                                adc_read_notify, avr);
        show_adc();
        if (vcd_fh && vcd_letter < 127) {
            unsigned int limit;

            limit = 127 - vcd_letter;
            if (limit > ADC_CHANNEL_COUNT)
                limit = ADC_CHANNEL_COUNT;
            ADC_vcd_letter = vcd_letter;
            for (i = 0; i < limit; ++i, ++vcd_letter) {
                fprintf(vcd_fh, "$var real 32 %c adc0_%d $end\n",
                        vcd_letter, i);
            }
        }

        /* If there is a VCD input file, connect to the IRQs
         * that will push its data into the simulation.
         */

        if (vcd_input) {
            for (i = 0; i < ADC_CHANNEL_COUNT; ++i) {
                avr_irq_register_notify(ADC_base_irq + i, vcd_adc_in_notify,
                                        NULL);
            }
        }
    }

    /* Complete VCD header. */

    if (vcd_fh)
        fprintf(vcd_fh, "$upscope $end\n$enddefinitions $end\n");

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

        /* Run the simulation.  Stops on requested cycles done, fatal error
         * or endless sleep.
         */

        burst_done = 0;
        do
            state = my_avr_run(avr);
        while (!burst_done && state < cpu_Done);

        /* Display the PC and cycle count.  Limited to about 10 Hz. */

        {
            static struct timeval last_tv;
            struct timeval        tv;

            gettimeofday(&tv, NULL);
            if (tv.tv_usec - last_tv.tv_usec > 100000 ||
                tv.tv_sec > last_tv.tv_sec) {
                last_tv = tv;
                Bfp->new_value(PC_handle, avr->pc);         // Update PC ...
                Bfp->new_value(Cycles_handle, avr->cycle);  // and cycle count.
            }
        }
    } while (state < cpu_Done);
    return 1;
}
