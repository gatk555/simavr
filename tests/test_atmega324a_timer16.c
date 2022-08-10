#include <stddef.h>
#include <stdlib.h>
#include "tests.h"
#include "avr_ioport.h"

static avr_t *avr;
static avr_cycle_count_t base;
static struct action {
	enum { Stop, Ignore, Record, Check}
		              action;
	int               value;
	avr_cycle_count_t when;
} actions[] = {
    /* Normal mode WGM 0. */

    {Record, 1, 0}, {Ignore, 0, 0}, {Check, 1, 50}, {Check, 0, (1 << 16)},
    {Check, 1, (1 << 16) + 50 - 1},

    /* Phase-correct, 8-bit: WGM 1. Two full cycles with waggle on overflow.*/

    {Record, 0, 0}, {Check, 1, 200}, {Check, 0, 310},
    {Check, 1, 510}, {Ignore, 0, 0}, {Check, 1, 710}, {Check, 0, 820},
    {Check, 1, 1022},
    
    /* Phase-correct, 9-bit: WGM 2. Very like WGM 1 so change OCR as well. */

    {Record, 0, 0}, {Check, 1, 300}, {Check, 0, 722},
    {Check, 1, 1322}, {Check, 0, 1441}, // Incorrect, but pass for now.
    {Stop, 0}};
    
static int index, stage = -1, step;

static void monitor(struct avr_irq_t *irq, uint32_t value, void *param)
{
	struct action *ap;

	ap = actions + index;
	if ((value & 1) != ap->value) {
		fail("Unexpected output %#x at step %d/%d after %lu cycles.\n",
			 value, stage, step, avr->cycle - base);
	}
	switch (ap->action) {
	case Record:
		++stage;
		step = 0;
		base = avr->cycle - 1; // Assumes single-cycle instruction.
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
	avr_irq_t *pin_irq;

	tests_init(argc, argv);
	avr = tests_init_avr("atmega324a_timer16.axf");
	pin_irq = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('D'), 4);
	avr_irq_register_notify(pin_irq, monitor, NULL);

	/* Run ... */

	tests_assert_uart_receive_avr(avr, 100000, "", '0');
        if (index != (sizeof actions/sizeof (struct action)) - 1) {
            fail("Not enough pin changes (%d/%lu) at %d/%d\n",
                 index, (sizeof actions/sizeof (struct action)) - 1,
				 stage, step);
        }
	tests_success();
	return 0;
}
