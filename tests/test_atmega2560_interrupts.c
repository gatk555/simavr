#include "tests.h"
#include "sim_avr.h"
#include "sim_interrupts.h"

#define LAST_VECTOR 54 // There are 56 but 55 is in use by UART3.

avr_cycle_count_t starting(avr_t *avr, avr_cycle_count_t when, void *param)
{
    int i;

    // Raise in reverse order of execution.

    for (i = LAST_VECTOR; i > 0; --i) {
        avr_int_vector_t *ip;

        ip = avr->interrupts.vectors[i];
        if (ip->enable.reg)
            avr_regbit_set(avr, ip->enable);
        avr_raise_interrupt(avr, ip);
    }
    return 0;
}

int main(int argc, char **argv) {
    static const char *expected =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUV| 9\r\n";
    avr_t             *avr;

    tests_init(argc, argv);

    avr = tests_init_avr("atmega2560_interrupts.axf");

    // Re-gain control once started.

    avr_cycle_timer_register(avr, 1, starting, (void *)0);

    tests_assert_uart_receive_avr(avr, 10000000, expected, '3');
    tests_success();
    return 0;
}
