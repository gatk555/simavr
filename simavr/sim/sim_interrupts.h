/*
	sim_interrupts.h

	Copyright 2008-2012 Michel Pollet <buserror@gmail.com>

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

#ifndef __SIM_INTERRUPTS_H__
#define __SIM_INTERRUPTS_H__

#include "sim_avr_types.h"
#include "sim_irq.h"
#include "fifo_declare.h"

#ifdef __cplusplus
extern "C" {
#endif

// Interrupt structure for the IO modules

typedef struct avr_int_vector_t {
	uint8_t 	vector;	// vector number, zero (reset) is reserved
	uint8_t		pending : 1, 	    // Wants to run.
			trace : 1,	    // For debug.
			level : 1,          // Level-triggered.
			raise_sticky : 1,   // Do not auto-clear .raised.
			clear_both : 1;     // Auto-clear .enable as well.

	// Mask and shift for PCINTs.  This is needed for chips like the 2560
	// where PCINT do not align with IRQs.

	uint8_t 	mask;
	int8_t 		shift;
	avr_regbit_t 	enable;	// Peripheral's interrupt enable bit.
	avr_regbit_t 	raised;	// Peripheral's interrupt flag  bit.
} avr_int_vector_t, *avr_int_vector_p;

/* Interrupt control table, embedded in struct avr_t. */

#define MAX_VECTOR_COUNT 64
typedef struct avr_int_table_t {
	uint8_t		 	 max_vector, pending_count, next_vector;
	avr_int_vector_t	*vectors[MAX_VECTOR_COUNT];
} avr_int_table_t, *avr_int_table_p;

/*
 * Interrupt Helper Functions
 */

// Register an interrupt vector.

void
avr_register_vector(
		struct avr_t *avr,
		avr_int_vector_t * vector);

// Raise an interrupt (if enabled). The interrupt is latched and will be
// called later.  Return non-zero if the interrupt was raised and
// is now pending.

int
avr_raise_interrupt(
		struct avr_t * avr,
		avr_int_vector_t * vector);

// Return non-zero if the AVR core has any pending interrupts.

int
avr_has_pending_interrupts(
		struct avr_t * avr);

// Return nonzero if a specific interrupt vector is pending.

int
avr_is_interrupt_pending(
		struct avr_t * avr,
		avr_int_vector_t * vector);

// Clear the "pending" status of an interrupt.

void
avr_clear_interrupt(
		struct avr_t * avr,
		avr_int_vector_t * vector);

// Called by the core at each cycle to check whether an interrupt is pending.

void
avr_service_interrupts(
		struct avr_t * avr);

// Called by the core when RETI opcode is run.

void
avr_interrupt_reti(
		struct avr_t * avr);

// Clear the interrupt (inc pending) if "raised" flag is 1.

int
avr_clear_interrupt_if(
		struct avr_t * avr,
		avr_int_vector_t * vector,
		uint8_t old);

#ifdef IRQ_IRQS
// Return the IRQ that is raised when the vector is enabled and called/cleared.
// This allows tracing of pending interrupts.

avr_irq_t *
avr_get_interrupt_irq(
		struct avr_t * avr,
		uint8_t v);
#endif

// Initializes the interrupt table.

void
avr_interrupt_init(
		struct avr_t * avr );

// Reset the interrupt table and the fifo.

void
avr_interrupt_reset(
		struct avr_t * avr );

#ifdef __cplusplus
};
#endif

#endif /* __SIM_INTERRUPTS_H__ */
