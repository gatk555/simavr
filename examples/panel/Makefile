#
# 	Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>
#
#	This file is part of simavr.
#
#	simavr is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	(at your option) any later version.
#
#	simavr is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
#	You should have received a copy of the GNU General Public License
#	along with simavr.  If not, see <http://www.gnu.org/licenses/>.

firm_src = ${wildcard at*.c}
firmware = ${firm_src:.c=.axf}
simavr = ../../

IPATH = .
IPATH += ${simavr}/include
IPATH += ${simavr}/simavr/sim

VPATH = .

all: ${firmware}

include ${simavr}/Makefile.common

clean: clean-${OBJ}
	rm -rf *.a *.axf *.vcd *.hex
