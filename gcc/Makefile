
# Sixgill: Static assertion checker for C/C++ programs.
# Copyright (C) 2009-2010  Stanford University
# Author: Brian Hackett
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

include ../config.mk

XGILL_OBJS= xgill.o type.o variable.o tree.o annot.o missing.o

all: xgill.so

xgill.so: $(XGILL_OBJS) ../bin/libxgill.a
	$(CXX) -shared -fPIC -o $@ $^ -lstdc++ ${HOST_LDFLAGS} -lz -lgmp

%.o: %.c xgill.h ../imlang/interface.h
	$(PLUGIN_CC) $(HOST_CFLAGS) -I$(GCC_PLUGIN_HEADERS) -g3 -fPIC -Wall -Wno-unused-value -c -o $@ $<

clean:
	rm -f *.o *.so
