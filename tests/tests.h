#ifndef __TESTS_H__
#define __TESTS_H__

#include "sim_avr.h"
#include "sim_elf.h"

enum tests_finish_reason {
	LJR_CYCLE_TIMER = 1,
	LJR_SPECIAL_DEINIT = 2,
	// LJR_SLEEP_WITH_INT_OFF - LJR_SPECIAL_DEINIT happens...
};

#define ATMEGA48_UDR0 0xc6
#define ATMEGA88_UDR0 0xc6
#define ATMEGA644_UDR0 0xc6

/* Direct access to an output buffer for tests with their own run(). */

struct output_buffer {
	char *str;
	int currlen;
	int alloclen;
	int maxlen;
};

void init_output_buffer(struct output_buffer *buf);
void buf_output_cb(struct avr_irq_t *irq, uint32_t value, void *param);
void reg_output_cb(struct avr_t *avr, avr_io_addr_t addr, uint8_t v,
                   void *param);

/* Commonly-used functions. */

#define fail(s, ...) _fail(__FILE__, __LINE__, s, ## __VA_ARGS__)

void __attribute__ ((noreturn,format (printf, 3, 4)))
_fail(const char *filename, int linenum, const char *fmt, ...);

avr_t *tests_init_avr(const char *elfname);
void tests_init(int argc, char **argv);
void tests_success(void);

int tests_run_test(avr_t *avr, unsigned long run_usec, int (*run)(avr_t *));
int tests_run_avr(avr_t *avr, unsigned long run_usec);
int tests_init_and_run_test(const char *elfname, unsigned long run_usec);
void tests_assert_uart_receive(const char *elfname,
			       unsigned long run_usec,
			       const char *expected, // what we should get
			       char uart);
void tests_assert_uart_receive_avr(avr_t *avr,
			       unsigned long run_usec,
			       const char *expected,
			       char uart);				   
void tests_assert_register_receive(const char    *elfname,
                                   unsigned long  run_usec,
                                   const char    *expected,
                                   avr_io_addr_t  reg_addr);
void tests_assert_register_receive_avr(avr_t         *avr,
                                       unsigned long  run_usec,
                                       const char    *expected,
                                       avr_io_addr_t  reg_addr);
void tests_assert_uart_receive_fw(elf_firmware_t *fw,
								  const char *firmware,
								  unsigned long run_usec,
								  const char *expected,
								  char uart);

void tests_assert_cycles_at_least(unsigned long n);
void tests_assert_cycles_at_most(unsigned long n);

// the range is inclusive
void tests_assert_cycles_between(unsigned long min, unsigned long max);

extern avr_cycle_count_t tests_cycle_count;
extern int tests_disable_stdout;

#endif
