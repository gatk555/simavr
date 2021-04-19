#include <string.h>
#include "tests.h"
#include "avr_acomp.h"

static char record[256], *bp = record;

static void input_monitor(struct avr_irq_t *irq, uint32_t value, void *param)
{
	union {
		avr_acomp_inputs_t inputs;
		uint32_t           val;
	}             u;

	u.val = value;
	*bp++ = u.inputs.active + '0';
	*bp++ = u.inputs.positive + '0';
	*bp++ = u.inputs.negative + '0';
	*bp++ = '.';
}

int main(int argc, char **argv) {
	static const char *expected =
		"Check analog comparator with polling values\r\n"
		"110110101010000100\r\n"
		"Check analog comparator interrupts\r\n"
		"YYYYYYFY\r\n"
		"Check analog comparator triggering timer capture\r\n"
		"YY";
	static const char *expected_inputs =
		"100.101.102.103.104.105.106.107.108.101.111."
		"110.111.112.113.114.115.116.117.118.111."
		"000.111.112.111.112.111.112.111.112.111.112.111.";
	const avr_pin_info_t *pip;

	tests_init(argc, argv);
	avr_t *avr = tests_init_avr("atmega88_ac_test.axf");

	// Test the pin information ioctl.

	pip = (const avr_pin_info_t *)0;
	if (avr_ioctl(avr, AVR_IOCTL_ACOMP_GETPINS, &pip) < 0 || !pip ||
	    pip[-1].port_letter || pip[2].port_letter ||
	    pip[1].port_letter != 'D' || pip[1].pin != 7) {
		fail("AVR_IOCTL_ACOMP_GETPINS failed.\n");
	}

        // Monitor the input state.

        avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ,
					      ACOMP_IRQ_INPUT_STATE),
				input_monitor, (void *)0);

	// set voltages
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_AIN0), 2000);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_AIN1), 1800);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC0), 200);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC1), 3000);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC2), 1500);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC3), 1500);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC4), 3000);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC5), 200);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC6), 3000);
	avr_raise_irq(avr_io_getirq(avr, AVR_IOCTL_ACOMP_GETIRQ, ACOMP_IRQ_ADC7), 1500);

	tests_assert_uart_receive_avr(avr, 100000,
				   expected, '0');

	if (strcmp(expected_inputs, record)) {
		fail("Expected inputs:\n%s\nactual:\n%s\n",
		     expected_inputs, record);
	}
	tests_success();
	return 0;
}
