/*
	sim_gdb.c

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

#include "sim_network.h"
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "sim_avr.h"
#include "sim_core.h" // for SET_SREG_FROM, READ_SREG_INTO
#include "sim_hex.h"
#include "avr_eeprom.h"
#include "sim_gdb.h"

// For debug printfs: "#define DBG(w) w"
#define DBG(w)

#define WATCH_LIMIT (32)

typedef struct {
	uint32_t len; /**< How many points are taken (points[0] .. points[len - 1]). */
	struct {
		uint32_t addr; /**< Which address is watched. */
		uint32_t size; /**< How large is the watched segment. */
		uint32_t kind; /**< Bitmask of enum avr_gdb_watch_type values. */
	} points[WATCH_LIMIT];
} avr_gdb_watchpoints_t;

typedef struct avr_gdb_t {
	avr_t * avr;
	int	listen;	// listen socket
	int	s;	// current gdb connection

	avr_gdb_watchpoints_t breakpoints;
	avr_gdb_watchpoints_t watchpoints;

	// These are used by gdb's "info io_registers" command.

	uint16_t ior_base;
	uint8_t  ior_count, mad;
} avr_gdb_t;


/**
 * Returns the index of the watchpoint if found, -1 otherwise.
 */
static int
gdb_watch_find(
		const avr_gdb_watchpoints_t * w,
		uint32_t addr )
{
	for (int i = 0; i < w->len; i++) {
		if (w->points[i].addr > addr) {
			return -1;
		} else if (w->points[i].addr == addr) {
			return i;
		}
	}

	return -1;
}

/**
 * Contrary to gdb_watch_find, this actually checks the address against
 * a watched memory _range_.
 */
static int
gdb_watch_find_range(
		const avr_gdb_watchpoints_t * w,
		uint32_t addr )
{
	for (int i = 0; i < w->len; i++) {
		if (w->points[i].addr > addr) {
			return -1;
		} else if (w->points[i].addr <= addr && addr < w->points[i].addr + w->points[i].size) {
			return i;
		}
	}

	return -1;
}

/**
 * Returns -1 on error, 0 otherwise.
 */
static int
gdb_watch_add_or_update(
		avr_gdb_watchpoints_t * w,
		enum avr_gdb_watch_type kind,
		uint32_t addr,
		uint32_t size )
{
	/* If the watchpoint exists, update it. */
	int i = gdb_watch_find(w, addr);
	if (i != -1) {
		w->points[i].size = size;
		w->points[i].kind |= kind;
		return 0;
	}

	/* Otherwise add it. */
	if (w->len == WATCH_LIMIT) {
		return -1;
	}

	/* Find the insertion point. */
	for (i = 0; i < w->len; i++) {
		if (w->points[i].addr > addr) {
			break;
		}
	}

	w->len++;

	/* Make space for new element, moving old ones from the end. */
	for (int j = w->len; j > i; j--) {
		w->points[j] = w->points[j - 1];
	}

	/* Insert it. */
	w->points[i].kind = kind;
	w->points[i].addr = addr;
	w->points[i].size = size;

	return 0;
}

/**
 * Returns -1 on error or if the specified point does not exist, 0 otherwise.
 */
static int
gdb_watch_rm(
		avr_gdb_watchpoints_t * w,
		enum avr_gdb_watch_type kind,
		uint32_t addr )
{
	int i = gdb_watch_find(w, addr);
	if (i == -1) {
		return -1;
	}

	w->points[i].kind &= ~kind;
	if (w->points[i].kind) {
		return 0;
	}

	for (i = i + 1; i < w->len; i++) {
		w->points[i - 1] = w->points[i];
	}

	w->len--;

	return 0;
}

static void
gdb_watch_clear(
		avr_gdb_watchpoints_t * w )
{
	w->len = 0;
}

static void
gdb_send_reply(
		avr_gdb_t * g,
		char * cmd )
{
	uint8_t reply[1024];
	uint8_t * dst = reply;
	uint8_t check = 0;
	*dst++ = '$';
	while (*cmd) {
		check += *cmd;
		*dst++ = *cmd++;
	}
	sprintf((char*)dst, "#%02x", check);
	DBG(printf("%s '%s'\n", __FUNCTION__, reply);)
	send(g->s, reply, dst - reply + 3, 0);
}

static void
gdb_send_quick_status(
		avr_gdb_t * g,
		uint8_t signal )
{
	char cmd[64];
	uint8_t sreg;

	READ_SREG_INTO(g->avr, sreg);

	sprintf(cmd, "T%02x20:%02x;21:%02x%02x;22:%02x%02x%02x00;",
		signal ? signal : 5, sreg,
		g->avr->iobase[R_SPL], g->avr->iobase[R_SPH],
		g->avr->pc & 0xff, (g->avr->pc >> 8) & 0xff,
		(g->avr->pc >> 16) & 0xff);
	gdb_send_reply(g, cmd);
}

static int
gdb_change_breakpoint(
		avr_gdb_watchpoints_t * w,
		int set,
		enum avr_gdb_watch_type kind,
		uint32_t addr,
		uint32_t size )
{
	DBG(printf("set %d kind %d addr %08x len %d\n", set, kind, addr, size);)

	if (set) {
		return gdb_watch_add_or_update(w, kind, addr, size);
	} else {
		return gdb_watch_rm(w, kind, addr);
	}

	return -1;
}

static int
gdb_write_register(
		avr_gdb_t * g,
		int regi,
		uint8_t * src )
{
	switch (regi) {
		case 0 ... 31:
			g->avr->base[regi] = *src;
			return 1;
		case 32:
			g->avr->iobase[R_SREG] = *src;
			SET_SREG_FROM(g->avr, *src);
			return 1;
		case 33:
			g->avr->iobase[R_SPL] = src[0];
			g->avr->iobase[R_SPH] = src[1];
			return 2;
		case 34:
			g->avr->pc = src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24);
			return 4;
	}
	return 1;
}

static int
gdb_read_register(
		avr_gdb_t * g,
		int regi,
		char * rep )
{
	switch (regi) {
		case 0 ... 31:
			sprintf(rep, "%02x", g->avr->base[regi]);
			break;
		case 32: {
				uint8_t sreg;
				READ_SREG_INTO(g->avr, sreg);
				sprintf(rep, "%02x", sreg);
			}
			break;
		case 33:
			sprintf(rep, "%02x%02x",
				g->avr->iobase[R_SPL], g->avr->iobase[R_SPH]);
			break;
		case 34:
			sprintf(rep, "%02x%02x%02x00",
				g->avr->pc & 0xff, (g->avr->pc>>8) & 0xff,
				(g->avr->pc >> 16) & 0xff);
			break;
	}
	return strlen(rep);
}


static uint8_t
handle_monitor(avr_t * avr, avr_gdb_t * g, char * cmd)
{
	char         *ip, *op;
	unsigned int  c1, c2;
	char          dehex[64];

	if (*cmd++ != ',')
		return 1;		// Bad format
	for (op = dehex; op < dehex + (sizeof dehex - 1); ++op) {
		if (!*cmd)
			break;
		if (sscanf(cmd, "%1x%1x", &c1, &c2) != 2)
			return 2;	// Bad format
		*op = (c1 << 4) + c2;
		cmd += 2;
	}
	*op = '\0';
	if (*cmd)
		return 3;		// Too long
	ip = dehex;
	while (*ip) {
		while (*ip == ' ' || *ip == '\t')
			++ip;

		if (strncmp(ip, "reset", 5) == 0) {
			avr->state = cpu_StepDone;
			avr_reset(avr);
			ip += 5;
		} else if (strncmp(ip, "halt", 4) == 0) {
			avr->state = cpu_Stopped;
			ip += 4;
		} else if (strncmp(ip, "ior", 3) == 0) {
			unsigned int base;
			int          n, m, count;

			// Format is "ior <base> <count>
			// or just "ior" to reset.

			ip += 3;
			m = sscanf(ip, "%x %i%n", &base, &count, &n);
			if (m <= 0) {
				// Reset values.

				g->ior_base = g->ior_count = 0;
				n = 0;
			} else if (m != 2) {
				return 1;
			} else {
				if (count <= 0 ||
					base + count + avr->io_offset - 1 > avr->ioend) {
					return 4;	// bad value
				}
				g->ior_base = base;
				g->ior_count = count;
			}
			ip += n;
		} else {
			return 5;
		}
	}
	return 0;
}

static void
handle_io_registers(avr_t * avr, avr_gdb_t * g, char * cmd)
{
	char *       params;
	char *       reply;
	unsigned int addr, count;
	char         buff[1024];

	if (g->mad) {
		/* For this command, gdb employs a streaming protocol,
		 * with the command being repeated until the stub sends
		 * an empy packet as terminator.  That makes no sense,
		 * as the requests are sized to ensure the reply will
		 * fit in a single packet.
		 */

		reply = "";
		g->mad = 0;
	} else {
		params = cmd + 11;
		if (sscanf(params, ":%x,%x", &addr, &count) == 2) {
			int i;

			// Send names and values.

			addr += avr->io_offset + g->ior_base;
			if (addr + count > avr->ioend)
				count = avr->ioend + 1 - addr;
			reply = buff;
			for (i = 0; i < count; ++i) {
				const char *name;

				name = avr_regname(avr, addr + i);
				reply += sprintf(reply, "%s,%x;",
						 name, avr->data[addr + i]);
				if (reply > buff + sizeof buff - 20)
					break;
			}
		} else {
			// Send register count.

			count = g->ior_count ? g->ior_count : avr->ioend - avr->io_offset + 1;
			sprintf(buff, "%x", count);
		}
		reply = buff;
		g->mad = 1;
	}
	gdb_send_reply(g, reply);
}

static void
handle_v(avr_t * avr, avr_gdb_t * g, char * cmd, int length)
{
	uint32_t  addr;
	uint8_t  *src = NULL;
	int       len, err = -1;

	if (strncmp(cmd, "FlashErase", 10) == 0) {

		sscanf(cmd, "%*[^:]:%x,%x", &addr, &len);
		if (addr < avr->flashend) {
			src = avr->flash + addr;
			if (addr + len > avr->flashend)
				len = avr->flashend - addr;
			memset(src, 0xff, len);
			DBG(printf("FlashErase: %x,%x\n", addr, len);) //Remove
		} else {
			err = 1;
		}
	} else if (strncmp(cmd, "FlashWrite", 10) == 0) {
		if (sscanf(cmd, "%*[^:]:%x:%n", &addr, &len) != 1) {
			err = 2;
		} else {
			DBG(printf("FlashWrite %x\n", addr);) //Remove
			if  (len >= length) {
				err = 99;
			} else if (addr < avr->flashend) {
				char    *end;
				uint8_t *limit;

				end = cmd + length;
				cmd += len;
				src = avr->flash + addr;
				limit = avr->flash + avr->flashend;
				for (; cmd < end && src < limit; ++cmd) {
					int escaped = 0;

					if (escaped) {
						*src++ = *cmd ^ 0x20;
						escaped = 0;
					} else if (*cmd == '}') {
						escaped = 1;
					} else {
						*src++ = *cmd;
					}
				}
				addr = src - avr->flash; // Address of end.
				if (addr > avr->codeend)
					avr->codeend = addr;
				if (cmd != end) {
					DBG(printf("FlashWrite %ld bytes left!\n", end - cmd));
				}
			} else {
				err = 1;
			}
		}
	} else if (strncmp(cmd, "FlashDone", 9) == 0) {
		DBG(printf("FlashDone\n");) //Remove
	} else {
		gdb_send_reply(g, "");
		return;
	}

	if (err < 0) {
		gdb_send_reply(g, "OK");
	} else {
		char b[32];

		sprintf(b, "E %.2d", err);
		gdb_send_reply(g, b);
	}
}

static void
gdb_handle_command(
		avr_gdb_t * g,
		char      * cmd,
		int         length)
{
	avr_t * avr = g->avr;
	char rep[1024];
	uint8_t command = *cmd++;
	switch (command) {
		case 'q':
			if (strncmp(cmd, "Supported", 9) == 0) {
				/* If GDB asked what features we support, report back
				 * the features we support, which is just memory layout
				 * information for now.
				 */
				gdb_send_reply(g, "qXfer:memory-map:read+");
				break;
			} else if (strncmp(cmd, "Attached", 8) == 0) {
				/* Respond that we are attached to an existing process..
				 * ourselves!
				 */
				gdb_send_reply(g, "1");
				break;
			// Rmoving the following 3 lines fixes #150 issue:
			// } else if (strncmp(cmd, "Offsets", 7) == 0) {
			//	gdb_send_reply(g, "Text=0;Data=800000;Bss=800000");
			//	break;
			} else if (strncmp(cmd, "Xfer:memory-map:read", 20) == 0) {
				snprintf(rep, sizeof(rep),
						"l<memory-map>\n"
						" <memory type='ram' start='0x800000' length='%#x'/>\n"
						" <memory type='flash' start='0' length='%#x'>\n"
						"  <property name='blocksize'>0x80</property>\n"
						" </memory>\n"
						"</memory-map>",
						g->avr->ramend + 1, g->avr->flashend + 1);

				gdb_send_reply(g, rep);
				break;
			} else if (strncmp(cmd, "RegisterInfo", 12) == 0) {
				// Send back the information we have on this register (if any).
				long n = strtol(cmd + 12, NULL, 16);
				if (n < 32) {
					// General purpose (8-bit) registers.
					snprintf(rep, sizeof(rep), "name:r%ld;bitsize:8;offset:0;encoding:uint;format:hex;set:General Purpose Registers;gcc:%ld;dwarf:%ld;", n, n, n);
					gdb_send_reply(g, rep);
					break;
				} else if (n == 32) {
					// SREG (flags) register.
					snprintf(rep, sizeof(rep), "name:sreg;bitsize:8;offset:0;encoding:uint;format:binary;set:General Purpose Registers;gcc:32;dwarf:32;");
					gdb_send_reply(g, rep);
					break;
				} else if (n == 33) {
					// SP register (SPH and SPL combined).
					snprintf(rep, sizeof(rep), "name:sp;bitsize:16;offset:0;encoding:uint;format:hex;set:General Purpose Registers;gcc:33;dwarf:33;generic:sp;");
					gdb_send_reply(g, rep);
					break;
				} else if (n == 34) {
					// PC register
					snprintf(rep, sizeof(rep), "name:pc;bitsize:32;offset:0;encoding:uint;format:hex;set:General Purpose Registers;gcc:34;dwarf:34;generic:pc;");
					gdb_send_reply(g, rep);
					break;
				} else {
					// Register not available.
					// By sending back nothing, the debugger knows it has read
					// all available registers.
				}
			} else if (strncmp(cmd, "Rcmd", 4) == 0) { // monitor command
				uint8_t err = handle_monitor(avr, g, cmd + 4);
				if (err) {
					snprintf(rep, sizeof rep,
						 "E%02x", err);
					gdb_send_reply(g, rep);
				} else {
					gdb_send_reply(g, "OK");
				}
				break;
			} else if (strncmp(cmd, "Ravr.io_reg", 11) == 0) {
				handle_io_registers(avr, g, cmd);
				break;
			}
			gdb_send_reply(g, "");
			break;
		case '?':
			gdb_send_quick_status(g, 0);
			break;
		case 'G': {	// set all general purpose registers
			// get their binary form
			read_hex_string(cmd, (uint8_t*)rep, strlen(cmd));
			uint8_t *src = (uint8_t*)rep;
			for (int i = 0; i < 35; i++)
				src += gdb_write_register(g, i, src);
			gdb_send_reply(g, "OK");
		}	break;
		case 'g': {	// read all general purpose registers
			char * dst = rep;
			for (int i = 0; i < 35; i++)
				dst += gdb_read_register(g, i, dst);
			gdb_send_reply(g, rep);
		}	break;
		case 'p': {	// read register
			unsigned int regi = 0;
			sscanf(cmd, "%x", &regi);
			gdb_read_register(g, regi, rep);
			gdb_send_reply(g, rep);
		}	break;
		case 'P': {	// write register
			unsigned int regi = 0;
			char * val = strchr(cmd, '=');
			if (!val)
				break;
			*val++ = 0;
			sscanf(cmd, "%x", &regi);
			read_hex_string(val, (uint8_t*)rep, strlen(val));
			gdb_write_register(g, regi, (uint8_t*)rep);
			gdb_send_reply(g, "OK");
		}	break;
		case 'm': {	// read memory
			avr_flashaddr_t addr;
			uint32_t len;
			sscanf(cmd, "%x,%x", &addr, &len);
			uint8_t * src = NULL;
			/* GDB seems to also use 0x1800000 for sram ?!?! */
			addr &= 0xffffff;
			if (addr < avr->flashend) {
				src = avr->flash + addr;
			} else if (addr >= 0x800000 && (addr - 0x800000) <= avr->ramend) {
				src = avr->data + addr - 0x800000;
			} else if (addr == (0x800000 + avr->ramend + 1) && len == 2) {
				// Allow GDB to read a value just after end of stack.
				// This is necessary to make instruction stepping work when stack is empty
				AVR_LOG(avr, LOG_TRACE,
						"GDB: read just past end of stack %08x, %08x; returning zero\n", addr, len);
				gdb_send_reply(g, "0000");
				break;
			} else if (addr >= 0x810000 && (addr - 0x810000) <= avr->e2end) {
				avr_eeprom_desc_t ee = {.offset = (addr - 0x810000)};
				avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &ee);
				if (ee.ee)
					src = ee.ee;
				else {
					gdb_send_reply(g, "E01");
					break;
				}
			} else {
				AVR_LOG(avr, LOG_ERROR,
						"GDB: read memory error %08x, %08x (ramend %04x)\n",
						addr, len, avr->ramend+1);
				gdb_send_reply(g, "E01");
				break;
			}
			char * dst = rep;
			while (len--) {
				sprintf(dst, "%02x", *src++);
				dst += 2;
			}
			*dst = 0;
			gdb_send_reply(g, rep);
		}	break;
		case 'M': {	// write memory
			uint32_t addr, len;
			sscanf(cmd, "%x,%x", &addr, &len);
			char * start = strchr(cmd, ':');
			if (!start) {
				gdb_send_reply(g, "E01");
				break;
			}
			if (addr < 0xffff) {
				read_hex_string(start + 1, avr->flash + addr, strlen(start+1));
				gdb_send_reply(g, "OK");
			} else if (addr >= 0x800000 && (addr - 0x800000) <= avr->ramend) {
				read_hex_string(start + 1, avr->data + addr - 0x800000, strlen(start+1));
				gdb_send_reply(g, "OK");
			} else if (addr >= 0x810000 && (addr - 0x810000) <= avr->e2end) {
				read_hex_string(start + 1, (uint8_t*)rep, strlen(start+1));
				avr_eeprom_desc_t ee = {.offset = (addr - 0x810000), .size = len, .ee = (uint8_t*)rep };
				avr_ioctl(avr, AVR_IOCTL_EEPROM_SET, &ee);
				gdb_send_reply(g, "OK");
			} else {
				AVR_LOG(avr, LOG_ERROR, "GDB: write memory error %08x, %08x\n", addr, len);
				gdb_send_reply(g, "E01");
			}
		}	break;
		case 'c': {	// continue
			avr->state = cpu_Running;
		}	break;
		case 's': {	// step
			avr->state = cpu_Step;
		}	break;
		case 'r': {	// deprecated, suggested for AVRStudio compatibility
			avr->state = cpu_StepDone;
			avr_reset(avr);
		}	break;
		case 'Z': 	// set clear break/watchpoint
		case 'z': {
			uint32_t kind, addr, len;
			int set = (command == 'Z');
			sscanf(cmd, "%d,%x,%x", &kind, &addr, &len);
//			printf("breakpoint %d, %08x, %08x\n", kind, addr, len);
			switch (kind) {
				case 0:	// software breakpoint
				case 1:	// hardware breakpoint
					if (addr > avr->flashend ||
							gdb_change_breakpoint(&g->breakpoints, set, 1 << kind, addr, len) == -1) {
						gdb_send_reply(g, "E01");
						break;
					}

					gdb_send_reply(g, "OK");
					break;
				case 2: // write watchpoint
				case 3: // read watchpoint
				case 4: // access watchpoint
					/* Mask out the offset applied to SRAM addresses. */
					addr &= ~0x800000;
					if (addr > avr->ramend ||
							gdb_change_breakpoint(&g->watchpoints, set, 1 << kind, addr, len) == -1) {
						gdb_send_reply(g, "E01");
						break;
					}

					gdb_send_reply(g, "OK");
					break;
				default:
					gdb_send_reply(g, "");
					break;
			}
		}	break;
		case 'K': 	// kill
		case 'D': {	// detach
			avr->state = cpu_Done;
			gdb_send_reply(g, "OK");
		}	break;
		case 'v':
			handle_v(avr, g, cmd, length);
			break;
		default:
			gdb_send_reply(g, "");
			break;
	}
}

static int
gdb_network_handler(
		avr_gdb_t * g,
		uint32_t dosleep )
{
	fd_set read_set;
	int max;
	FD_ZERO(&read_set);

	if (g->s != -1) {
		FD_SET(g->s, &read_set);
		max = g->s + 1;
	} else {
		FD_SET(g->listen, &read_set);
		max = g->listen + 1;
	}
	struct timeval timo = { dosleep / 1000000, dosleep % 1000000 };
	int ret = select(max, &read_set, NULL, NULL, &timo);

	if (ret == 0)
		return 0;

	if (FD_ISSET(g->listen, &read_set)) {
		g->s = accept(g->listen, NULL, NULL);

		if (g->s == -1) {
			perror("gdb_network_handler accept");
			sleep(5);
			return 1;
		}
		int i = 1;
		setsockopt (g->s, IPPROTO_TCP, TCP_NODELAY, &i, sizeof (i));
		g->avr->state = cpu_Stopped;
		DBG(printf("%s connection opened\n", __FUNCTION__);)
	}

	if (g->s != -1 && FD_ISSET(g->s, &read_set)) {
		uint8_t buffer[1024];

		ssize_t r = recv(g->s, buffer, sizeof(buffer)-1, 0);

		if (r == 0) {
			DBG(printf("%s connection closed\n", __FUNCTION__);)
			close(g->s);
			gdb_watch_clear(&g->breakpoints);
			gdb_watch_clear(&g->watchpoints);
			g->avr->state = cpu_Running;	// resume
			g->s = -1;
			return 1;
		}
		if (r == -1) {
			perror("gdb_network_handler recv");
			sleep(1);
			return 1;
		}
		buffer[r] = 0;
		DBG(
			if (!strncmp("+$vFlashWrite", (char *)buffer, 13)) {
				printf("%s: received Flashwrite command %ld bytes\n",
					   __FUNCTION__, r);
			} else {
				printf("%s: received command %ld bytes\n'%s'\n",
					   __FUNCTION__, r, buffer);
			})
	//	hdump("gdb", buffer, r);

		uint8_t * src = buffer;
		while (*src == '+' || *src == '-')
			src++;
		// control C -- lets send the guy a nice status packet
		if (*src == 3) {
			src++;
			g->avr->state = cpu_StepDone;
			printf("GDB hit control-c\n");
		}
		if (*src  == '$') {
			// strip checksum
			uint8_t * end = buffer + r - 1;
			while (end > src && *end != '#')
				*end-- = 0;
			*end = 0;
			src++;
			DBG(
				if (strncmp("vFlashWrite", (char *)src, 11))
					printf("GDB command = '%s'\n", src);)
			send(g->s, "+", 1, 0);
			if (end > src)
				gdb_handle_command(g, (char*)src, end - src);
		}
	}
	return 1;
}

/**
 * If an applicable watchpoint exists for addr, stop the cpu and send a status report.
 * type is one of AVR_GDB_WATCH_READ, AVR_GDB_WATCH_WRITE depending on the type of access.
 */
void
avr_gdb_handle_watchpoints(
		avr_t * avr,
		uint16_t addr,
		enum avr_gdb_watch_type type )
{
	avr_gdb_t *g = avr->gdb;

	int i = gdb_watch_find_range(&g->watchpoints, addr);
	if (i == -1) {
		return;
	}

	int kind = g->watchpoints.points[i].kind;
	if (kind & type) {
		/* Send gdb reply (see GDB user manual appendix E.3). */
		char cmd[78];
		uint8_t sreg;

		READ_SREG_INTO(g->avr, sreg);
		sprintf(cmd, "T%02x20:%02x;21:%02x%02x;22:%02x%02x%02x00;%s:%06x;",
				5, sreg,
				g->avr->iobase[R_SPL], g->avr->iobase[R_SPH],
				g->avr->pc & 0xff, (g->avr->pc >> 8) & 0xff,
				(g->avr->pc >> 16) & 0xff,
				kind & AVR_GDB_WATCH_ACCESS ? "awatch" :
					kind & AVR_GDB_WATCH_WRITE ?
						"watch" : "rwatch",
				addr | 0x800000);
		gdb_send_reply(g, cmd);
		avr->state = cpu_Stopped;
	}
}

int
avr_gdb_processor(
		avr_t * avr,
		int sleep )
{
	if (!avr || !avr->gdb)
		return 0;
	avr_gdb_t * g = avr->gdb;

	if (avr->state == cpu_Running &&
			gdb_watch_find(&g->breakpoints, avr->pc) != -1) {
		DBG(printf("avr_gdb_processor hit breakpoint at %08x\n", avr->pc);)
		gdb_send_quick_status(g, 0);
		avr->state = cpu_Stopped;
	} else if (avr->state == cpu_StepDone) {
		gdb_send_quick_status(g, 0);
		avr->state = cpu_Stopped;
	}
	// this also sleeps for a bit
	return gdb_network_handler(g, sleep);
}


int
avr_gdb_init(
		avr_t * avr )
{
	if (avr->gdb)
		return 0; // GDB server already is active

	avr_gdb_t * g = malloc(sizeof(avr_gdb_t));
	memset(g, 0, sizeof(avr_gdb_t));

	avr->gdb = NULL;

	if ( network_init() ) {
		AVR_LOG(avr, LOG_ERROR, "GDB: Can't initialize network");
		goto error;
	}

	if ((g->listen = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		AVR_LOG(avr, LOG_ERROR, "GDB: Can't create socket: %s", strerror(errno));
		goto error;
	}

	int optval = 1;
	setsockopt(g->listen, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	struct sockaddr_in address = { 0 };
	address.sin_family = AF_INET;
	address.sin_port = htons (avr->gdb_port);

	if (bind(g->listen, (struct sockaddr *) &address, sizeof(address))) {
		AVR_LOG(avr, LOG_ERROR, "GDB: Can not bind socket: %s", strerror(errno));
		goto error;
	}
	if (listen(g->listen, 1)) {
		perror("listen");
		goto error;
	}
	printf("avr_gdb_init listening on port %d\n", avr->gdb_port);
	g->avr = avr;
	g->s = -1;
	avr->gdb = g;
	// change default run behaviour to use the slightly slower versions
	avr->run = avr_callback_run_gdb;
	avr->sleep = avr_callback_sleep_gdb;

	return 0;

error:
	if (g->listen >= 0)
		close(g->listen);
	free(g);

	return -1;
}

void
avr_deinit_gdb(
		avr_t * avr )
{
	if (!avr->gdb)
		return;
	avr->run = avr_callback_run_raw; // restore normal callbacks
	avr->sleep = avr_callback_sleep_raw;
	if (avr->gdb->listen != -1)
		close(avr->gdb->listen);
	avr->gdb->listen = -1;
	if (avr->gdb->s != -1)
		close(avr->gdb->s);
	avr->gdb->s = -1;
	free(avr->gdb);
	avr->gdb = NULL;

	network_release();
}
