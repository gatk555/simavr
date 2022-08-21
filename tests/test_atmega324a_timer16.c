#include <stddef.h>
#include <stdlib.h>
#include "tests.h"
#include "avr_ioport.h"

#define F 0 // Pin numbers in port D, pin 0 set by firmware.
#define A 5 // OCR1A
#define B 4 // OCR1B

static avr_t *avr;
static avr_cycle_count_t base;
static struct action {
	enum { Stop, Ignore, Record, Check}
		              action;
    int               pin, value;
	avr_cycle_count_t when;
} actions[] = {
	/* Normal mode WGM 0. */

	{Record, F, 1, 0}, {Check, B, 1, 49}, {Check, F, 0, (1 << 16)},
	{Ignore, B, 0, 0}, {Check, B, 1, (1 << 16) + 50 - 2}, {Ignore, B, 0, 0},

	/* Phase-correct, 8-bit: WGM 1. Two full cycles with waggle on overflow.*/

	{Record, F, 1, 0}, {Check, B, 1, 200}, {Check, B, 0, 309},
	{Check, F, 0, 511}, {Check, B, 1, 710}, {Check, B, 0, 820},
	{Check, F, 1, 1022},
    
	/* Phase-correct, 9-bit: WGM 2. Very like WGM 1 so change OCR as well. */

	{Record, F, 0, 0}, {Check, B, 1, 300}, {Check, B, 0, 721},
	{Check, F, 1, 1023},   // Firmware waggle at overflow
	// OCR1B changed to 400 here.
	{Check, B, 1, 1322}, {Check, B, 0, 1643},

	/* Phase-correct, 10-bit: WGM 3. Start with TCNT = 400 OCR1B = 500
	 * so reported counts are all 400 less than cycle counts.
	 */

	{Record, F, 0, 0}, {Check, B, 1, 100}, {Check, B, 0, 1146},
	// OCR1B changed to 100 here, counting down, takes effect at TOP
	{Check, B, 1, 2146}, {Check, B, 0, 3592},

	/* CTC, clear counter and toggle on OCRA, count changed during count. */

	{Record, F, 1, 0}, {Check, A, 1, 245}, {Check, A, 0, 746},
	// OCR1A changed while counting
	{Check, A, 1, 847}, {Check, A, 0, 948},

	{Stop, 0}};
    
static int index, stage = -1, step;

static void monitor(struct avr_irq_t *irq, uint32_t value, void *param)
{
	struct action *ap;
	int    pin = *(int *)param;

	ap = actions + index;
	if (pin != ap->pin) {
		fail("Output %#x on unexpected pin %d (not %d) "
			 "at step %d/%d after %lu cycles.\n",
			 value, pin, ap->pin, stage, step, avr->cycle - base);
	}
	if ((value & 1) != ap->value) {
		fail("Unexpected output %#x at step %d/%d after %lu cycles.\n",
			 value, stage, step, avr->cycle - base);
	}
	switch (ap->action) {
	case Record:
		++stage;
		step = 0;
		base = avr->cycle; // PORTD write instruction still in progress.
		break;
	case Ignore:
		break;
	case Check:
		// Allow for delay by 3-cycle instuction
		if (avr->cycle - base < ap->when ||
			avr->cycle - base > ap->when + 2) {
			fail("Expected %lu cycles but found %lu at step %d/%d\n",
				 ap->when, avr->cycle - base, stage, step);
		}
		break;
	default:
		fail("Unexpected action at step %d/%d\n", step, stage);
		break;
	}
	++index;
	++step;
}

int main(int argc, char **argv) {
	static int  i0 = 0, i4 = 4, i5 = 5;
	avr_irq_t  *base_irq;

	tests_init(argc, argv);
	avr = tests_init_avr("atmega324a_timer16.axf");
	base_irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), 0);
	avr_irq_register_notify(base_irq, monitor, &i0);
	avr_irq_register_notify(base_irq + 4, monitor, &i4);
	avr_irq_register_notify(base_irq + 5, monitor, &i5);

	/* Run ... */

	tests_assert_uart_receive_avr(avr, 1000000, "", '0');
	if (index != (sizeof actions/sizeof (struct action)) - 1) {
		fail("Not enough pin changes (%d/%lu) at %d/%d\n",
			 index, (sizeof actions/sizeof (struct action)) - 1,
			 stage, step);
	}
	tests_success();
	return 0;
}
