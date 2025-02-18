/*
	sim_core.h

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

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

#ifndef __SIM_CORE_H__
#define __SIM_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifdef NO_COLOR
	#define FONT_GREEN
	#define FONT_RED
	#define FONT_DEFAULT
#else
	#define FONT_GREEN	"\e[32m"
	#define FONT_RED	"\e[31m"
	#define FONT_DEFAULT	"\e[0m"
#endif

/*
 * Instruction decoder, run ONE instruction
 */
avr_flashaddr_t avr_run_one(avr_t * avr);

/*
 * These are for internal access to the stack (for interrupts)
 */
uint16_t _avr_sp_get(avr_t * avr);
void _avr_sp_set(avr_t * avr, uint16_t sp);
int _avr_push_addr(avr_t * avr, avr_flashaddr_t addr);

#if CONFIG_SIMAVR_TRACE

/*
 * DEBUG bits follow
 */

void avr_dump_state(avr_t * avr);

// In this file there is always an avr!

#define AVR_REGNAME(reg) avr_regname(avr, reg)

#define DUMP_REG() { \
	for (int i = 0; i < 32; i++) \
		printf("%s=%02x%c", AVR_REGNAME(i), avr->data[i], \
			   (i % 8) == 7 ? '\n' : ' '); \
	uint16_t y = avr->data[R_YL] | (avr->data[R_YH] << 8); \
	for (int i = 0; i < 20; i++) \
		printf("Y+%02d=%02x%c", i, avr->data[y+i], i % 10 == 9 ? '\n' : ' '); \
	printf("\n"); \
}


#if AVR_STACK_WATCH
#define DUMP_STACK() \
		for (int i = avr->trace_data->stack_frame_index; i; i--) {\
			int pci = i-1;\
			printf(FONT_RED "*** %04x: %-25s sp %04x\n" FONT_DEFAULT,\
					avr->trace_data->stack_frame[pci].pc, \
					avr->trace_data->codeline ? avr->trace_data->codeline[avr->trace_data->stack_frame[pci].pc>>1]->symbol : "unknown", \
							avr->trace_data->stack_frame[pci].sp);\
		}
#else
#define DUMP_STACK()
#endif

#else /* CONFIG_SIMAVR_TRACE */

#define DUMP_STACK()
#define DUMP_REG();

#endif

/**
 * Reconstructs the SREG value from avr->sreg into dst.
 */
#define READ_SREG_INTO(avr, dst) { \
			dst = 0; \
			for (int i = 0; i < 8; i++) \
				if (avr->sreg[i] > 1) { \
					printf("** Invalid SREG!!\n"); \
				} else if (avr->sreg[i]) \
					dst |= (1 << i); \
		}

static inline void avr_sreg_set(avr_t * avr, uint8_t flag, uint8_t ival)
{
	/*
	 *	clear interrupt_state if disabling interrupts.
	 *	set wait if enabling interrupts.
	 *	no change if interrupt flag does not change.
	 */

	if (flag == S_I) {
		if (ival) {
			if (!avr->sreg[S_I])
				avr->interrupt_state = -1;
		} else
			avr->interrupt_state = 0;
	}

	avr->sreg[flag] = ival;
}

/**
 * Splits the SREG value from src into the avr->sreg array.
 */
#define SET_SREG_FROM(avr, src) { \
			for (int i = 0; i < 8; i++) \
				avr_sreg_set(avr, i, (src & (1 << i)) != 0); \
		}

/*
 * Opcode is sitting at the end of the flash to catch PC overflows.
 * Apparently it's used by some code to simulate soft reset?
 */
#define AVR_OVERFLOW_OPCODE 0xf1f1

#ifdef __cplusplus
};
#endif

#endif /*__SIM_CORE_H__*/
