/* -*- mode: C; eval: (setq c-basic-offset 8); -*- (emacs magic) */
/*
	avr_extint.c

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>
        Changes copyright 2021 Giles Atkinson

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
#include "avr_extint.h"
#include "avr_ioport.h"

/**
 * Fossil function, retained for backward compatability.
 *
 * @brief avr_extint_is_strict_lvl_trig
 * @param avr
 * @param extint_no: an ext interrupt number, e.g. 0 or 1 (corresponds to INT0 or INT1)
 * @return -1 if irrelevant extint_no given, strict
 * level triggering flag otherwise.
 */
int avr_extint_is_strict_lvl_trig(avr_t * avr, uint8_t extint_no)
{
        return -1;
}

/**
 * Fossil function, retained for backward compatability.
 *
 * @brief avr_extint_set_strict_lvl_trig
 * @param avr
 * @param extint_no: an ext interrupt number, e.g. 0 or 1 (corresponds to INT0 or INT1)
 * @param strict: new value for level triggering flag
 */
void avr_extint_set_strict_lvl_trig(avr_t * avr, uint8_t extint_no, uint8_t strict)
{
}

/* Get the bit that controls an interrupt. */

static unsigned int avr_extint_get_bit(struct avr_eint_i_t * ip)
{
        char               port; 
	avr_ioport_state_t iostate;

        port = ip->port_ioctl & 0xFF;
	if (avr_ioctl(ip->owner->io.avr, AVR_IOCTL_IOPORT_GETSTATE(port),
                      &iostate) < 0) {
		return 1;
        }
	return (iostate.pin >> ip->port_pin) & 1;
}

/* Raise level-triggered interrupt? */

static void avr_extint_test_level(struct avr_eint_i_t * ip)
{
        if (ip->previous_enable && ip->previous_mode &&
            avr_extint_get_bit(ip) == 0) {
                /* Low level-triggered is enabled and pin is low.
                 * TODO/FIX ME this is not really correct and the datasheets
                 * say that the level-triggered interrupt does not set the
                 * flag and the interrupt source is monitored continually.
                 */

                avr_raise_interrupt(ip->owner->io.avr, &ip->vector);
        }
}

/* New value for a controlling I/O port bit. */

static void avr_extint_irq_notify(struct avr_irq_t * irq, uint32_t value, void * param)
{
	avr_extint_t * p = (avr_extint_t *)param;
	avr_t * avr = p->io.avr;

	int up = !irq->value && value;
	int down = irq->value && !value;

	// ?? uint8_t isc_bits = p->eint[irq->irq + 1].isc->reg ? 2 : 1;
	uint8_t isc_bits = p->eint[irq->irq].isc[1].reg ? 2 : 1;
	uint8_t mode = avr_regbit_get_array(avr, p->eint[irq->irq].isc, isc_bits);

	// Asynchronous interrupts, eg int2 in m16, m32 etc. support only down/up
	if (isc_bits == 1)
		mode +=2;

	switch (mode) {
		case 0: // Level triggered (low level) interrupt
                        avr_extint_test_level(p->eint + irq->irq);
			break;
		case 1: // Toggle-triggered interrupt
			if (up || down)
				avr_raise_interrupt(avr, &p->eint[irq->irq].vector);
			break;
		case 2: // Falling edge triggered
			if (down)
				avr_raise_interrupt(avr, &p->eint[irq->irq].vector);
			break;
		case 3: // Rising edge trigggerd
			if (up)
				avr_raise_interrupt(avr, &p->eint[irq->irq].vector);
			break;
	}
}

/* The interrupt is starting or returning. */

static void avr_extint_interrupt_notify(struct avr_irq_t * irq,
                                        uint32_t value, void * param)
{
        struct avr_eint_i_t * ip = (struct avr_eint_i_t *)param;

        if (value == 0) {
                // Interrupt is returning, retrigger?

                avr_extint_test_level(ip);
        }
}

/* The level-triggered interrupt has been enabled. */

static void avr_extint_LT_enabled(struct avr_eint_i_t * ip)
{
        /* Watch the interrupt so that it can be re-triggered when the
         * interrupt handler returns.
         *
         * TODO/FIX ME this is not really correct and the datasheets
         * say that the level-triggered interrupt does not set the
         * flag and the interrupt source is monitored continually.
         * It really its own code in the core.
         */

        avr_irq_register_notify(ip->vector.irq + AVR_INT_IRQ_RUNNING,
                                avr_extint_interrupt_notify, ip);

        // Check the pin

        if (avr_extint_get_bit(ip) == 0)
                avr_raise_interrupt(ip->owner->io.avr, &ip->vector);
}

/* An enable or control bit has changed. */

static void avr_exint_status_change(struct avr_t  * avr,
                                    avr_io_addr_t   addr,
                                    uint8_t         v,
                                    void          * param)
{
	avr_extint_t * p = (avr_extint_t *)param;

   printf("In avr_exint_status_change addr = %x value = %02x\n", addr, v);
	avr_core_watch_write(avr, addr, v);
	for (int i = 0; i < EXTINT_COUNT; i++) {
                uint8_t              enable, mode;
                struct avr_eint_i_t * ip = p->eint + i;

		if (!ip->port_ioctl)
                        return;
                enable = avr_regbit_get(avr, ip->vector.enable);
                mode = avr_regbit_get_array(avr, ip->isc, 2);
                printf("i = %d enable %d->%d mode %d->%d\n", i, ip->previous_enable, enable, ip->previous_mode, mode);
                if (!ip->isc[1].reg)
                        mode += 2;
                if (enable != ip->previous_enable) {
                        if (enable) {
                                // Watch the pin.

  printf("i = %d connecting irq %d\n", i, ip->port_irq->irq);
                                avr_connect_irq(ip->port_irq, p->io.irq + i);
                                if (mode == 0)
                                        avr_extint_LT_enabled(ip);
                        } else {
                                // Forget the pin.

                                avr_unconnect_irq(ip->port_irq, p->io.irq + i);
                                if (ip->previous_mode == 0) {
                                        // Forget the interrupt

                                        avr_irq_unregister_notify(
                                          ip->vector.irq + AVR_INT_IRQ_RUNNING,
                                          avr_extint_interrupt_notify, ip);
                                }
                        }
                } else if (enable && mode != ip->previous_mode) {
                        if (ip->previous_mode == 0) {
                                // Forget the interrupt

                                avr_irq_unregister_notify(
                                    ip->vector.irq + AVR_INT_IRQ_RUNNING,
                                    avr_extint_interrupt_notify, ip);
                        } else if (mode == 0) {
                                avr_extint_LT_enabled(ip);
                        }
                }
                ip->previous_enable = enable;
                ip->previous_mode = mode;
        }
}

static void avr_extint_reset(avr_io_t * port)
{
	avr_extint_t * p = (avr_extint_t *)port;

	for (int i = 0; i < EXTINT_COUNT; i++) {
                struct avr_eint_i_t * ip = p->eint + i;

		if (ip->port_ioctl) {
                        avr_irq_register_notify(p->io.irq + i,
                                                avr_extint_irq_notify, p);
                        ip->port_irq = avr_io_getirq(p->io.avr,
                                                     ip->port_ioctl,
                                                     ip->port_pin);
			if (!ip->isc[1].reg)
                                ip->previous_mode = 2;
		}
	}
}

static const char * irq_names[EXTINT_COUNT] = {
	[EXTINT_IRQ_OUT_INT0] = "<int0",
	[EXTINT_IRQ_OUT_INT1] = "<int1",
	[EXTINT_IRQ_OUT_INT2] = "<int2",
	[EXTINT_IRQ_OUT_INT3] = "<int3",
	[EXTINT_IRQ_OUT_INT4] = "<int4",
	[EXTINT_IRQ_OUT_INT5] = "<int5",
	[EXTINT_IRQ_OUT_INT6] = "<int6",
	[EXTINT_IRQ_OUT_INT7] = "<int7",
};

static	avr_io_t	_io = {
	.kind = "extint",
	.reset = avr_extint_reset,
	.irq_names = irq_names,
};

void avr_extint_init(avr_t * avr, avr_extint_t * p)
{
	p->io = _io;

	avr_register_io(avr, &p->io);
	for (int i = 0; i < EXTINT_COUNT; i++) {
                struct avr_eint_i_t * ip = p->eint + i;
                int                   j;

                if (!ip->port_ioctl)
                     break;
                ip->owner = p;
		avr_register_vector(avr, &ip->vector);

                // Watch enable registers - only once.

                for (j = 0; j < i; j++) {
                        if (p->eint[j].vector.enable.reg ==
                            ip->vector.enable.reg) {
                                // Already registered.

                                break;
                        }
                }
                if (i == j) {
                        avr_register_io_write(avr,
                                              ip->vector.enable.reg,
                                              avr_exint_status_change, p);
                }

                // Watch control registers - only once.
        
                for (j = 0; j < i; j++) {
                        if (p->eint[j].isc[0].reg == ip->isc[0].reg ||
                            p->eint[j].isc[1].reg == ip->isc[0].reg) {
                                // Already registered.

                                break;
                        }
                }
                if (i == j) {
                        avr_register_io_write(avr, ip->isc[0].reg,
                                              avr_exint_status_change, p);
                }
                if (ip->isc[1].reg &&
                    ip->isc[1].reg != ip->isc[0].reg) {
                        for (j = 0; j < i; j++) {
                                if ((p->eint[j].isc[0].reg ==
                                     ip->isc[1].reg) ||
                                    (p->eint[j].isc[1].reg ==
                                     ip->isc[1].reg)) {
                                        // Already registered.

                                        break;
                                }
                        }
                        if (i == j) {
                                avr_register_io_write(avr,
                                                      ip->isc[0].reg,
                                                      avr_exint_status_change,
                                                      p);
                        }
                }
        }

	// allocate this module's IRQ

	avr_io_setirqs(&p->io, AVR_IOCTL_EXTINT_GETIRQ(), EXTINT_COUNT, NULL);
}

