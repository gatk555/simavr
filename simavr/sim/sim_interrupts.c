/*
	sim_interrupts.c

	Copyright 2008-2012 Michel Pollet <buserror@gmail.com>
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
#include <strings.h>
#include "sim_interrupts.h"
#include "sim_avr.h"
#include "sim_core.h"

/* Macro to handle the indirect bit. */

#define INDIRECT(v) \
	if ((v)->indirect) (v) = avr->interrupts.vectors[(v)->vector]

void
avr_interrupt_init(
		avr_t * avr )
{
	avr_int_table_p table = &avr->interrupts;

	memset(table, 0, sizeof(*table));
}

void
avr_interrupt_reset(
		avr_t * avr )
{
	avr_int_table_p table = &avr->interrupts;

	avr->interrupt_state = 0;
	table->pending_count = 0;
	table->next_vector = 0;
	for (int i = 0; i < table->max_vector; i++) {
		if (table->vectors[i]) {
			table->vectors[i]->pending = 0;
			table->vectors[i]->level = 0;
		}
	}
}

/* Peripherals call this to claim their vectors. */

void
avr_register_vector(
		avr_t *avr,
		avr_int_vector_t * vector)
{
	avr_int_table_p table = &avr->interrupts;
	uint8_t 	vec_num = vector->vector;

	if (!vec_num)
		return;
	if (vec_num >= MAX_VECTOR_COUNT) {
		fprintf(stderr,
			"Vector %d out-of range in avr_register_vector()\n",
			vec_num);
		return;
	}
	if (table->vectors[vec_num]) {
		avr_int_vector_t *old;

		/* Request for a vector already in use. 
		 * It may be legitamate, as for the odd shared pin-change
		 * interrupt of the ATmega2560.
		 */

		old = table->vectors[vec_num];
		if (REGBIT_EQUAL(old->enable, vector->enable) &&
		    REGBIT_EQUAL(old->raised, vector->raised) &&
		    old->raise_sticky == vector->raise_sticky) {
			/* Vector will be replaced by old when used. */

			vector->indirect = 1;
			return;
		}
		fprintf(stderr,
			"Attempted double registration of interrupt vector %d "
			"ignored.\n",
			vec_num);
		return;
	}
	table->vectors[vec_num] = vector;
	if (table->max_vector < vec_num)
		table->max_vector = vec_num;
	if (vector->trace)
		printf("IRQ%d registered (enabled %04x:%d)\n",
			vec_num, vector->enable.reg, vector->enable.bit);
	if (!vector->enable.reg)
		AVR_LOG(avr, LOG_WARNING, "IRQ%d No 'enable' bit !\n",
			vec_num);
}

int
avr_has_pending_interrupts(
		avr_t * avr)
{
	return avr->interrupts.pending_count;
}

int
avr_is_interrupt_pending(
		avr_t * avr,
		avr_int_vector_t * vector)
{
	INDIRECT(vector);
	return vector->pending;
}

int
avr_is_interrupt_enabled(
		avr_t * avr,
		avr_int_vector_t * vector)
{
	return avr_regbit_get(avr, vector->enable);
}

int
avr_raise_interrupt(
		avr_t * avr,
		avr_int_vector_t * vector)
{
	uint8_t vec_num;

	if (!vector)
		return 0;
	vec_num = vector->vector;
	if (!vec_num)
		return 0;
	INDIRECT(vector);
	if (vector->trace) {
		printf("IRQ%d raising (enabled %d)\n",
			vec_num, avr_regbit_get(avr, vector->enable));
	}

	// always mark the 'raised' flag to one, even if the interrupt is disabled
	// this allow "polling" for the "raised" flag, like for non-interrupt
	// driven UART and so so. These flags are often "write one to clear"

	if (vector->raised.reg)
		avr_regbit_set(avr, vector->raised);

	if (vector->pending) {
		if (vector->trace) {
			printf("IRQ%d: I=%d already raised (enabled %d) "
			       "(cycle %lld pc 0x%x)\n",
			       vec_num, !!avr->sreg[S_I],
			       avr_regbit_get(avr, vector->enable),
				(long long int)avr->cycle, avr->pc);
		}
		return 0;
	}

	// If the interrupt is enabled, attempt to wake the core

	if (avr_regbit_get(avr, vector->enable)) {
		avr_int_table_p table = &avr->interrupts;

		// Mark the interrupt as pending.

		vector->pending = 1;

		/* Priority policy here. */

		if (table->pending_count++ == 0 ||
		    vec_num < table->next_vector ||
		    table->next_vector == 0) {
			table->next_vector = vec_num;
		}

		if (avr->sreg[S_I] && avr->interrupt_state == 0)
			avr->interrupt_state = 1;
		if (avr->state == cpu_Sleeping) {
			if (vector->trace)
				printf("IRQ%d Waking CPU due to interrupt\n",
					vec_num);
			avr->state = cpu_Running;
		}
	}
	return 1;
}

void
avr_clear_interrupt(
		avr_t * avr,
		avr_int_vector_t * vector)
{
	avr_int_table_p table = &avr->interrupts;
	uint8_t 	vec_num;

	if (!vector)
		return;
	vec_num = vector->vector;
	if (vec_num == 0)
		return;
	INDIRECT(vector);
	if (vector->trace)
		printf("IRQ%d cleared\n", vec_num);
	if (vector->raised.reg && !vector->raise_sticky)
		avr_regbit_clear(avr, vector->raised);
	if (!vector->pending)
		return;
	vector->pending = 0;

	// Bookeeping.

	if (--table->pending_count > 0 && table->next_vector == vec_num) {
		int i;

		/* Locate highest-priority pending interrupt. */

		for (i = vec_num + 1; i <= table->max_vector; ++i) {
			if (table->vectors[i] == NULL)
				continue;
			if (table->vectors[i]->pending) {
				table->next_vector = i;
				break;
			}
		}
		if (i > table->max_vector) {
			fprintf(stderr,
				"Internal error: interrupt not found. (%d)\n",
				table->pending_count);
			table->pending_count = 0;
		}
	} else if (table->pending_count == 0) {
		table->next_vector = 0;
		if (avr->interrupt_state > 0)
			avr->interrupt_state = 0;
	}
}

int
avr_clear_interrupt_if(
		avr_t * avr,
		avr_int_vector_t * vector,
		uint8_t old)
{
	if (avr_regbit_get(avr, vector->raised)) {
		avr_clear_interrupt(avr, vector);
		return 1;
	}
	avr_regbit_setto(avr, vector->raised, old);
	return 0;
}

/* To be removed, now that the IRQs are gone.  Or retained for return? FIX */

#ifdef IRQ_IRQS
avr_irq_t *
avr_get_interrupt_irq(
		avr_t * avr,
		uint8_t v)
{
	return NULL;
}
#endif

/* This is called upon RETI. */

void
avr_interrupt_reti(
		struct avr_t * avr)
{
//	avr_int_table_p table = &avr->interrupts;

	/* Move setting of S_I here? FIX */
}

/* Check whether interrupts are pending.
 * If so, check if the interrupt "latency" is reached,
 * and if so triggers the handlers and jump to the vector.
 */

void
avr_service_interrupts(
		avr_t * avr)
{
	avr_int_table_p   table = &avr->interrupts;
	avr_int_vector_t *vp;

        if (!avr->interrupt_state)
		return;
	if (avr->interrupt_state < 0) {
		if (++avr->interrupt_state == 0)
			avr->interrupt_state = table->pending_count;
		return;
	}

	if (!avr->sreg[S_I]) {
            avr->interrupt_state = 0;
            return;
        }

	/* Some sanity checks, maybe temporary. */

        if (table->pending_count == 0 || table->next_vector == 0) {
		fprintf(stderr, "Internal error: no active interrupt: %d/%d\n",
			table->pending_count, table->next_vector);
                table->pending_count = (table->next_vector != 0); // Try it.
	}

	vp = table->vectors[table->next_vector];

	// If that single interrupt is masked, ignore it and continue
	// could also have been disabled, or cleared.

	if ((vp->enable.reg && !avr_regbit_get(avr, vp->enable)) ||
	    (vp->raised.reg && !avr_regbit_get(avr, vp->raised)) ||
	    !vp->pending) {
		fprintf(stderr, "Internal error: interrupt flags: %d/%d/%d\n",
			avr_regbit_get(avr, vp->enable),
			avr_regbit_get(avr, vp->raised),
			vp->pending);
	} else {
		if (vp->trace)
			printf("IRQ%d calling\n", vp->vector);
		avr->cycle += _avr_push_addr(avr, avr->pc);
		avr_sreg_set(avr, S_I, 0);
		avr->pc = vp->vector * avr->vector_size;
	}

	if (!vp->level) {
		avr_clear_interrupt(avr, vp);
		if (vp->clear_both && vp->enable.reg) // Used by watchdog.
			avr_regbit_clear(avr, vp->enable);
	}
}

