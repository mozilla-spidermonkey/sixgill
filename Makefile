
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

# this file includes the results of the autoconf run
include config.mk

# configure --enable-debug
ifdef DEBUG
  OPT = -O0 -DDEBUG
else
  OPT = -O2
endif

WARNINGS = -Wall -Wno-strict-aliasing -Werror -Wno-error=pointer-sign
CXXWARNINGS = -Wno-non-virtual-dtor -Wno-delete-non-virtual-dtor -Wno-template-id-cdtor

CXXFLAGS += -std=c++17 $(CXXWARNINGS)
CPPFLAGS = -g ${OPT} -I${PWD} ${HOST_CFLAGS} ${WARNINGS}
LDFLAGS = ${HOST_LDFLAGS} -lz -lsqlite3 -lgmp

# run 'make profile' to enable profiling in generated binaries
ifdef PROFILE
  CPPFLAGS += -pg
  LDFLAGS += -pg
endif

BACKEND_OBJS = \
	backend/action.o \
	backend/backend.o \
	backend/backend_block.o \
	backend/backend_compound.o \
	backend/backend_hash.o \
	backend/backend_util.o \
	backend/backend_xdb.o \
	backend/operand.o \
	backend/transaction.o

IMLANG_OBJS = \
	imlang/bit.o \
	imlang/block.o \
	imlang/exp.o \
	imlang/filename.o \
	imlang/interface.o \
	imlang/loopsplit.o \
	imlang/opcode.o \
	imlang/storage.o \
	imlang/trace.o \
	imlang/type.o \
	imlang/variable.o

UTIL_OBJS = \
	util/alloc.o \
	util/assert.o \
	util/buffer.o \
	util/config.o \
	util/hashcons.o \
	util/json.o \
	util/monitor.o \
	util/primitive.o \
	util/stream.o \
	util/timer.o \
	util/xml.o

XDB_OBJS = \
	xdb/xdb.o

LIB_OBJS = \
	${BACKEND_OBJS} \
	${IMLANG_OBJS} \
	${UTIL_OBJS} \
	${XDB_OBJS}

MAIN_OBJS = main/xdbfind.o main/xdbkeys.o main/xsource.o main/xmanager.o

XDB_LIB_OBJS = \
	util/alloc.o \
	util/buffer.o \
	util/json.o \
	util/stream.o \
	${XDB_OBJS} \
	xdb/library.o

ALL_LIBS = \
	bin/libxgill.a

ALL_BINS = \
	bin/xsource \
	bin/xdbfind \
 	bin/xdbkeys \
 	bin/xmanager

%.o: %.cpp ${INCLUDE}
	${CXX} ${CXXFLAGS} ${CPPFLAGS} -fPIC -c $< -o $@

.PHONY: all profile build-plugin clean

all: build-libevent ${ALL_LIBS} ${ALL_BINS} build-plugin

# Auto-generated header dependencies — included after 'all' so they don't
# change the default target.
%.d: %.cpp ${INCLUDE}
	${CXX} ${CXXFLAGS} ${CPPFLAGS} -MM -fPIC -c $< -o $@

DEPS = $(LIB_OBJS:.o=.d) $(MAIN_OBJS:.o=.d)

# Avoid including and therefore depending on and rebuilding the .d files when
# doing a `make clean`.
ifeq ($(filter clean,$(MAKECMDGOALS)),)
-include $(DEPS)
endif

$(ALL_BINS) $(ALL_LIBS): | bin

bin:
	@mkdir bin

profile:
	$(MAKE) all "PROFILE=1"

bin/libxgill.a: $(LIB_OBJS)
	rm -f $@
	ar -r $@ $(LIB_OBJS)

ifeq ($(GCC_PLUGIN_SUPPORT),yes)
build-plugin: $(ALL_LIBS)
	$(MAKE) -C gcc
else
build-plugin: bin/libxgill.a
endif

BUILD_LIBS = $(ALL_LIBS) $(LDFLAGS)

bin/xdbfind: main/xdbfind.o $(ALL_LIBS)
	$(CXX) $< -o $@ $(BUILD_LIBS)

bin/xdbkeys: main/xdbkeys.o $(ALL_LIBS)
	$(CXX) $< -o $@ $(BUILD_LIBS)

bin/xsource: main/xsource.o $(ALL_LIBS)
	$(CXX) $< -o $@ $(BUILD_LIBS)

bin/xmanager: main/xmanager.o ${ALL_LIBS}
	${CXX} libevent/*.o $< -o $@ ${BUILD_LIBS}

# libevent stuff

build-libevent: libevent/Makefile
	$(MAKE) -C libevent

libevent/Makefile:
	cd libevent && CPPFLAGS='$(filter-out -Werror,${CPPFLAGS})' ./configure

# other

clean:
	rm -f ${LIB_OBJS} bin/libxgill.a ${ALL_BINS} main/*.o $(DEPS)
	rm -f config.log config.status
	$(MAKE) -C gcc clean
	$(MAKE) -C libevent clean

distclean: clean
	rm -f config.mk config.h
	$(MAKE) -C libevent distclean
