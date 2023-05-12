/* Command program for RC servos. */

#include <stdio.h>
#include <stdio_ext.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include "attiny84_servo.h"

int main(int argc, char **argv)
{
	unsigned int  pulse;
	int           termfd;
	const char   *term;
	char          c;
	uint8_t       msg;

	term = (argc > 1) ? argv[1] : "/dev/ttyACM0";
	termfd = open(term, O_WRONLY);
	if (termfd < 0) {
		perror(term);
		return 1;
	}

	setbuf(stdout, NULL); // For prompting.
	for (;;) {
		__fpurge(stdin);
		fputs("> ", stdout);
		if (scanf("%c%u", &c, &pulse) == 2 &&
		    c >= 'a' && c <= 'c') {
			if (pulse < MINIMUM_PULSE || pulse > MAXIMUM_PULSE) {
				fputs("Value out of range\n", stderr);
				continue;
			}
			pulse -= MINIMUM_PULSE;
			msg = 0x80 + ((c - 'a') << CHAN_SHIFT) + (pulse >> 7);
                        printf("Sending %02x\n", msg);
			write(termfd, &msg, 1);
			msg = pulse & 0x7f;
                        printf("Sending %02x\n", msg);
			write(termfd, &msg, 1);
		} else {
			if (feof(stdin))
				return 0;
			fprintf(stderr,
					"Input must be one character a-c and "
					"an unsigned number %d-%d\n",
					MINIMUM_PULSE, MAXIMUM_PULSE);
		}
	}
}
