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
	return ((iostate.ddr >> ip->port_pin) & 1) ?
		   ((iostate.port >> ip->port_pin) & 1) :
		   ((iostate.pin >> ip->port_pin) & 1);
}

/* New value for a controlling I/O port bit. Called via connected IRQs. */

static void avr_extint_irq_notify(struct avr_irq_t * irq, uint32_t value,
				  void * param)
{
	avr_extint_t        * p = (avr_extint_t *)param;
	avr_t               * avr = p->io.avr;
	struct avr_eint_i_t * ip = p->eint + irq->irq;
        uint8_t               old_value;

        value &= 0xff;
        old_value = ip->port_irq->value & 0xff;

	int up = !old_value && value;
	int down = old_value && !value;

	uint8_t isc_bits = ip->isc[1].reg ? 2 : 1;
	uint8_t mode = avr_regbit_get_array(avr, ip->isc, isc_bits);

	// Asynchronous interrupts, eg int2 in m16, m32 etc. support only down/up
	if (isc_bits == 1)
		mode +=2;

	//printf("EXTINT IRQ %d %#x -> %#x mode %d\n",
	//	 irq->irq, old_value, value, mode);
	switch (mode) {
		case 0: // Level triggered (low level) interrupt.
                        // FIXME? avr_extint_test_level(p->eint + irq->irq);
			if (down)
				avr_raise_level(avr, &ip->vector);
			break;
		case 1: // Toggle-triggered interrupt
			if (up || down)
				avr_raise_interrupt(avr, &ip->vector);
			break;
		case 2: // Falling edge triggered
			if (down)
				avr_raise_interrupt(avr, &ip->vector);
			break;
		case 3: // Rising edge trigggerd
			if (up)
				avr_raise_interrupt(avr, &ip->vector);
			break;
	}
}

/* The level-triggered interrupt has been enabled. */

static void avr_extint_LT_enabled(struct avr_eint_i_t * ip)
{
        avr_t *avr;

        // Clear any pending interrupt.

        avr = ip->owner->io.avr;
	avr_clear_interrupt(avr, &ip->vector);

	// Check the pin.

        if (avr_extint_get_bit(ip) == 0)
                avr_raise_level(avr, &ip->vector);
}

/* An enable or control bit has changed. */

static void avr_exint_enable_change(struct avr_t  * avr,
                                    avr_io_addr_t   addr,
                                    uint8_t         v,
                                    void          * param)
{
	avr_extint_t * p = (avr_extint_t *)param;

	avr_core_watch_write(avr, addr, v);
	for (int i = 0; i < EXTINT_COUNT; i++) {
                uint8_t              enable;
                struct avr_eint_i_t * ip = p->eint + i;

		if (!ip->port_ioctl)
                        return;
                enable = avr_regbit_from_value(avr, ip->vector.enable, v);
                if (enable != ip->previous_enable) {
			//printf("i = %d enable %d->%d\n",
			//	 i, ip->previous_enable, enable);
                        if (enable) {
                                // Watch the pin.

				//printf("i = %d connecting irq %d\n",
				//	 i, ip->port_irq->irq);
                                avr_connect_irq(ip->port_irq, p->io.irq + i);
                                if (ip->previous_mode == 0)
                                        avr_extint_LT_enabled(ip);
                        } else {
                                // Forget the pin.

                                avr_unconnect_irq(ip->port_irq, p->io.irq + i);

				// Forget any active interrupt.

                                if (ip->previous_mode == 0)
					avr_clear_level(avr, &ip->vector);
				else
					avr_clear_interrupt(avr, &ip->vector);
			}
                }
                ip->previous_enable = enable;
        }
}

static void avr_exint_control_change(struct avr_t  * avr,
                                     avr_io_addr_t   addr,
                                     uint8_t         v,
                                     void          * param)
{
	avr_extint_t * p = (avr_extint_t *)param;

	avr_core_watch_write(avr, addr, v);
	for (int i = 0; i < EXTINT_COUNT; i++) {
                uint8_t               mode;
                struct avr_eint_i_t * ip = p->eint + i;

		if (!ip->port_ioctl)
                        return;
                mode = avr_regbit_from_value(avr, ip->isc[0], v) +
                            (avr_regbit_from_value(avr, ip->isc[1], v) << 1);
                if (!ip->isc[1].reg)
                        mode += 2;
                if (mode != ip->previous_mode) {
			//printf("i = %d mode %d->%d\n",
			//	 i, ip->previous_mode, mode);
			if (ip->previous_enable) {
				if (ip->previous_mode == 0) {
					// Forget the interrupt

					avr_clear_level(avr, &ip->vector);
				} else if (mode == 0) {
					avr_extint_LT_enabled(ip);
				}
                        } else if (mode == 0) {
				// Forget any pending interrupt.

				avr_clear_interrupt(avr, &ip->vector);
			}
                }
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
			if (p->eint[j].vector.enable.reg == ip->vector.enable.reg) {
				// Already registered.

				break;
			}
		}
		if (i == j) {
			avr_register_io_write(avr,
								  ip->vector.enable.reg,
								  avr_exint_enable_change, p);
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
								  avr_exint_control_change, p);
		}
		if (ip->isc[1].reg &&
			ip->isc[1].reg != ip->isc[0].reg) {
			for (j = 0; j < i; j++) {
				if ((p->eint[j].isc[0].reg == ip->isc[1].reg) ||
					(p->eint[j].isc[1].reg == ip->isc[1].reg)) {
					// Already registered.

					break;
				}
			}
			if (i == j) {
				avr_register_io_write(avr,
									  ip->isc[0].reg,
									  avr_exint_control_change,
									  p);
			}
		}

		if (!p->eint[i].port_ioctl)
			break;
		avr_register_vector(avr, &p->eint[i].vector);
	}
	// allocate this module's IRQ

	avr_io_setirqs(&p->io, AVR_IOCTL_EXTINT_GETIRQ(), EXTINT_COUNT, NULL);
}
