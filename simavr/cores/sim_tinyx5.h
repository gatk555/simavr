/*
	sim_tinyx5.h

	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>
	                     Jon Escombe <lists@dresco.co.uk>

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


#ifndef __SIM_TINYX5_H__
#define __SIM_TINYX5_H__

#include "sim_core_declare.h"
#include "avr_eeprom.h"
#include "avr_flash.h"
#include "avr_watchdog.h"
#include "avr_extint.h"
#include "avr_ioport.h"
#include "avr_adc.h"
#include "avr_timer.h"
#include "avr_acomp.h"
#include "avr_usi.h"

void tx5_init(struct avr_t * avr);
void tx5_reset(struct avr_t * avr);

/*
 * This is a template for all of the tinyx5 devices, hopefully
 */
struct mcu_t {
	avr_t core;
	avr_eeprom_t 	eeprom;
	avr_watchdog_t	watchdog;
	avr_flash_t 	selfprog;
	avr_extint_t	extint;
	avr_ioport_t	portb;
	avr_acomp_t		acomp;
	avr_adc_t		adc;
	avr_timer_t	timer0, timer1;
	avr_usi_t		usi;
};

#ifdef SIM_CORENAME

#ifndef SIM_VECTOR_SIZE
#error SIM_VECTOR_SIZE is not declared
#endif
#ifndef SIM_MMCU
#error SIM_MMCU is not declared
#endif

static const avr_pin_info_t adc_pins[] = {
	{'B', 5}, {'B', 2}, {'B', 4}, {'B', 3}, // ADC0-3
	{0, 0}
};

static const avr_pin_info_t acomp_pins[] = {
	{'B', 0}, {'B', 1}, // AIN0-1
	{0, 0}
};

static const avr_pin_info_t common_pins[] = {
	{'B', 0}, // AREF
	{0, 0}
};

const struct mcu_t SIM_CORENAME = {
	.core = {
		.mmcu = SIM_MMCU,
		DEFAULT_CORE(SIM_VECTOR_SIZE),

		.init = tx5_init,
		.reset = tx5_reset,
                .pin_info = common_pins,
	},
	.selfprog = {
		.flags = 0,
		.r_spm = SPMCSR,
		.spm_pagesize = SPM_PAGESIZE,
		.selfprgen = AVR_IO_REGBIT(SPMCSR, SPMEN),
		.pgers = AVR_IO_REGBIT(SPMCSR, PGERS),
		.pgwrt = AVR_IO_REGBIT(SPMCSR, PGWRT),
		.blbset = AVR_IO_REGBIT(SPMCSR, RFLB),
	},
	AVR_EEPROM_DECLARE(EE_RDY_vect),
	AVR_WATCHDOG_DECLARE(WDTCR, WDT_vect),
	.extint = {
		AVR_EXTINT_TINY_DECLARE(0, 'B', PB2, GIFR),
	},
	.portb = {
		.name = 'B',  .r_port = PORTB, .r_ddr = DDRB, .r_pin = PINB,
		.pcint = {
			.enable = AVR_IO_REGBIT(GIMSK, PCIE),
			.raised = AVR_IO_REGBIT(GIFR, PCIF),
			.vector = PCINT0_vect,
		},
		.r_pcint = PCMSK,
	},
	.acomp = {
		.mux_inputs = 4,
		.mux = { AVR_IO_REGBIT(ADMUX, MUX0), AVR_IO_REGBIT(ADMUX, MUX1),
				AVR_IO_REGBIT(ADMUX, MUX2), AVR_IO_REGBIT(ADMUX, MUX3) },
		.pradc = AVR_IO_REGBIT(PRR, PRADC),
		.aden = AVR_IO_REGBIT(ADCSRA, ADEN),
		.acme = AVR_IO_REGBIT(ADCSRB, ACME),

		.r_acsr = ACSR,
		.acis = { AVR_IO_REGBIT(ACSR, ACIS0), AVR_IO_REGBIT(ACSR, ACIS1) },
		.aco = AVR_IO_REGBIT(ACSR, ACO),
		.acbg = AVR_IO_REGBIT(ACSR, ACBG),
		.disabled = AVR_IO_REGBIT(ACSR, ACD),

		.ac = {
			.enable = AVR_IO_REGBIT(ACSR, ACIE),
			.raised = AVR_IO_REGBIT(ACSR, ACI),
			.vector = ANA_COMP_vect,
		},
		.pin_info = acomp_pins
	},
	.adc = {
		.r_admux = ADMUX,
		.mux = { AVR_IO_REGBIT(ADMUX, MUX0), AVR_IO_REGBIT(ADMUX, MUX1),
					AVR_IO_REGBIT(ADMUX, MUX2), AVR_IO_REGBIT(ADMUX, MUX3),},
		.ref = { AVR_IO_REGBIT(ADMUX, REFS0), AVR_IO_REGBIT(ADMUX, REFS1), AVR_IO_REGBIT(ADMUX, REFS2), },
		.ref_values = {
				[0] = ADC_VREF_VCC,
				[2] = ADC_VREF_V110,
				[4] = ADC_VREF_VCC,
				[6] = ADC_VREF_V256, [7] = ADC_VREF_V256,
		},

		.adlar = AVR_IO_REGBIT(ADMUX, ADLAR),
		.r_adcsra = ADCSRA,
		.aden = AVR_IO_REGBIT(ADCSRA, ADEN),
		.adsc = AVR_IO_REGBIT(ADCSRA, ADSC),
		.adate = AVR_IO_REGBIT(ADCSRA, ADATE),
		.adps = { AVR_IO_REGBIT(ADCSRA, ADPS0), AVR_IO_REGBIT(ADCSRA, ADPS1), AVR_IO_REGBIT(ADCSRA, ADPS2),},

		.r_adch = ADCH,
		.r_adcl = ADCL,

		.r_adcsrb = ADCSRB,
		.adts = { AVR_IO_REGBIT(ADCSRB, ADTS0), AVR_IO_REGBIT(ADCSRB, ADTS1), AVR_IO_REGBIT(ADCSRB, ADTS2),},
		.adts_op = {
			[0] = avr_adts_free_running,
			[1] = avr_adts_analog_comparator_0,
			[2] = avr_adts_external_interrupt_0,
			[3] = avr_adts_timer_0_compare_match_a,
			[4] = avr_adts_timer_0_overflow,
			[5] = avr_adts_timer_0_compare_match_b,
			[6] = avr_adts_pin_change_interrupt,
		},

		.bin = AVR_IO_REGBIT(ADCSRB, BIN),
		.ipr = AVR_IO_REGBIT(ADCSRB, IPR),

		.muxmode = {
			[0] = AVR_ADC_SINGLE(0), [1] = AVR_ADC_SINGLE(1),
			[2] = AVR_ADC_SINGLE(2), [3] = AVR_ADC_SINGLE(3),

			[ 4] = AVR_ADC_DIFF(2, 2,   1), [ 5] = AVR_ADC_DIFF(2, 2,  20),
			[ 6] = AVR_ADC_DIFF(2, 3,   1), [ 7] = AVR_ADC_DIFF(2, 3,  20),
			[ 8] = AVR_ADC_DIFF(0, 0,   1), [ 9] = AVR_ADC_DIFF(0, 0,  20),
			[10] = AVR_ADC_DIFF(0, 1,   1), [11] = AVR_ADC_DIFF(0, 1,  20),
			[12] = AVR_ADC_REF(1100),	// Vbg
			[13] = AVR_ADC_REF(0),		// GND
			[15] = AVR_ADC_TEMP(),
		},

		.adc = {
			.enable = AVR_IO_REGBIT(ADCSRA, ADIE),
			.raised = AVR_IO_REGBIT(ADCSRA, ADIF),
			.vector = ADC_vect,
		},
		.pin_info = adc_pins
	},
	.timer0 = {
		.name = '0',
		.wgm = { AVR_IO_REGBIT(TCCR0A, WGM00), AVR_IO_REGBIT(TCCR0A, WGM01), AVR_IO_REGBIT(TCCR0B, WGM02) },
		.wgm_op = {
			[0] = AVR_TIMER_WGM_NORMAL8(),
			[2] = AVR_TIMER_WGM_CTC(),
			[3] = AVR_TIMER_WGM_FASTPWM8(),
			[7] = AVR_TIMER_WGM_OCPWM(),
		},
		.cs = { AVR_IO_REGBIT(TCCR0B, CS00), AVR_IO_REGBIT(TCCR0B, CS01), AVR_IO_REGBIT(TCCR0B, CS02) },
		.cs_div = { 0, 0, 3 /* 8 */, 6 /* 64 */, 8 /* 256 */, 10 /* 1024 */ },

		.r_tcnt = TCNT0,

		.overflow = {
			.enable = AVR_IO_REGBIT(TIMSK, TOIE0),
			.raised = AVR_IO_REGBIT(TIFR, TOV0),
			.vector = TIMER0_OVF_vect,
		},
		.comp = {
			[AVR_TIMER_COMPA] = {
				.r_ocr = OCR0A,
				.com = AVR_IO_REGBITS(TCCR0A, COM0A0, 0x3),
				.com_pin = AVR_IO_REGBIT(PORTB, 0),
				.interrupt = {
					.enable = AVR_IO_REGBIT(TIMSK, OCIE0A),
					.raised = AVR_IO_REGBIT(TIFR, OCF0A),
					.vector = TIMER0_COMPA_vect,
				},
			},
			[AVR_TIMER_COMPB] = {
				.r_ocr = OCR0B,
				.com = AVR_IO_REGBITS(TCCR0A, COM0B0, 0x3),
				.com_pin = AVR_IO_REGBIT(PORTB, 1),
				.interrupt = {
					.enable = AVR_IO_REGBIT(TIMSK, OCIE0B),
					.raised = AVR_IO_REGBIT(TIFR, OCF0B),
					.vector = TIMER0_COMPB_vect,
				},
			},
		},
	},
	.timer1 = {
		.name = '1',
		// timer1 has no wgm bits, but we still need to define a wgm op so that
		// we can set a proper kind/size to the timer
		.wgm_op = {
			[0] = AVR_TIMER_WGM_NORMAL8(),
		},
		.cs = { AVR_IO_REGBIT(TCCR1, CS10), AVR_IO_REGBIT(TCCR1, CS11), AVR_IO_REGBIT(TCCR1, CS12), AVR_IO_REGBIT(TCCR1, CS13) },
		.cs_div = { 0, 0, 1 /* 2 */, 2 /* 4 */, 3 /* 8 */, 4 /* 16 */, 5 /* 32 */, 6 /* 64 */, 7 /* 128 */, 8 /* 256 */, 9 /* 512 */, 10 /* 1024 */, 11 /* 2048 */, 12 /* 4096 */, 13 /* 8192 */, 14 /* 16384 */ },

		.r_tcnt = TCNT1,

		.overflow = {
			.enable = AVR_IO_REGBIT(TIMSK, TOIE1),
			.raised = AVR_IO_REGBIT(TIFR, TOV1),
			.vector = TIMER1_OVF_vect,
		},
		.comp = {
			[AVR_TIMER_COMPA] = {
				.r_ocr = OCR1A,
				.com = AVR_IO_REGBITS(TCCR1, COM1A0, 0x3),
				.com_pin = AVR_IO_REGBIT(PORTB, 1),
				.interrupt = {
					.enable = AVR_IO_REGBIT(TIMSK, OCIE1A),
					.raised = AVR_IO_REGBIT(TIFR, OCF1A),
					.vector = TIMER1_COMPA_vect,
				},
			},
			[AVR_TIMER_COMPB] = {
				.r_ocr = OCR1B,
				.com = AVR_IO_REGBITS(GTCCR, COM1B0, 0x3),
				.com_pin = AVR_IO_REGBIT(PORTB, 4),
				.interrupt = {
					.enable = AVR_IO_REGBIT(TIMSK, OCIE1B),
					.raised = AVR_IO_REGBIT(TIFR, OCF1B),
					.vector = TIMER1_COMPB_vect,
				},
			},
			[AVR_TIMER_COMPC] = {
				.r_ocr = OCR1C,
			},
		},
	},
	AVR_USI_DECLARE('B', PORTB, 0, 1, 2)
};
#endif /* SIM_CORENAME */

#endif /* __SIM_TINYX5_H__ */
