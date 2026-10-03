
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

EMCC ?= emcc
ROOT := $(abspath .)
GMP_WASM ?= $(ROOT)/local/gmp-wasm
export EM_CACHE ?= $(ROOT)/local/emscripten_cache
EM_CPPFLAGS ?= -I$(ROOT) -I$(GMP_WASM)/include -std=c++17 -O2 -DXGILL_WASM

EM_LDFLAGS := \
    -s EXPORT_ES6=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=createModule \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_FUNCTIONS='["_xdb_to_json","_xdb_to_text","_xdb_flush_interned","_xdb_alloc","_xdb_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["UTF8ToString","HEAPU8"]' \
    -sUSE_ZLIB=1 \
    $(GMP_WASM)/lib/libgmp.a

# configure --enable-debug
ifdef DEBUG
  OPT = -O0 -DDEBUG
else
  OPT = -O2
endif

CXXWARNINGS = -Wno-non-virtual-dtor -Wno-delete-non-virtual-dtor -Wno-template-id-cdtor

CXXFLAGS += -std=c++17 $(CXXWARNINGS)
WARNINGS = -Wall -Wno-non-virtual-dtor -Wno-delete-non-virtual-dtor -Wno-strict-aliasing -Werror -Wno-error=pointer-sign
CPPFLAGS = -g $(OPT) -I$(PWD) $(HOST_CFLAGS) $(WARNINGS) -MMD -MP
LDFLAGS = $(HOST_LDFLAGS) -lz -lsqlite3 -lgmp

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

WASM_CPP := \
    util/json.cpp        \
    util/buffer.cpp      \
    util/stream.cpp      \
    util/assert.cpp      \
    util/alloc.cpp       \
    util/primitive.cpp   \
    util/hashcons.cpp    \
    imlang/block.cpp     \
    imlang/type.cpp      \
    imlang/variable.cpp  \
    imlang/exp.cpp       \
    imlang/bit.cpp       \
    imlang/opcode.cpp    \
    wasm/imlang_stubs.cpp \
    wasm/xdb-decode.cpp

WASM_JS := wasm/xdb-decode.js

WASM_OUT := wasm/xdb-json-wasm.mjs

# Test runs under Node, which requires a different module. The test build uses
# the same filename as production, but in the test/ directory, so the shared
# wasm/xdb-decode.js wrapper loads it when the test passes its own URL.
WASM_TEST_OUT := wasm/test/xdb-json-wasm.mjs

# The wasm component requires emcc (Emscripten). release.sh sets WITHOUT_WASM=1
# for --without-emsdk; when set, the wasm target and its install are skipped.
ifeq ($(WITHOUT_WASM),)
WASM_ALL := wasm/xdb-json-wasm.mjs
else
WASM_ALL :=
endif

.PHONY: all profile build-plugin install clean distclean wasm-test
%.o: %.cpp ${INCLUDE}
	${CXX} ${CXXFLAGS} ${CPPFLAGS} -fPIC -c $< -o $@

all: build-libevent ${ALL_LIBS} ${ALL_BINS} build-plugin $(WASM_ALL)

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

ifeq ($(CLANG_PLUGIN_SUPPORT),yes)
build-plugin: build-clang-plugin
endif

.PHONY: build-clang-plugin
build-clang-plugin: $(ALL_LIBS)
	$(MAKE) -C clang

BUILD_LIBS = $(ALL_LIBS) $(LDFLAGS)

bin/xdbfind: main/xdbfind.o $(ALL_LIBS)
	$(CXX) $< -o $@ $(BUILD_LIBS)

bin/xdbkeys: main/xdbkeys.o $(ALL_LIBS)
	$(CXX) $< -o $@ $(BUILD_LIBS)

bin/xsource: main/xsource.o $(ALL_LIBS)
	$(CXX) $< -o $@ $(BUILD_LIBS)

bin/xmanager: main/xmanager.o ${ALL_LIBS}
	${CXX} libevent/*.o $< -o $@ ${BUILD_LIBS}

# Production artifact: minimal, specialized to the SpiderMonkey JS shell.
# A single emcc invocation emits the .mjs loader and its companion .wasm.
$(WASM_OUT): $(WASM_CPP)
	$(EMCC) $(EM_CPPFLAGS) $(EM_LDFLAGS) -s ENVIRONMENT=shell $(WASM_CPP) -o $(WASM_OUT)

# Test-only artifact: same code built for node, so release.sh can run the wasm
# tests without a JS shell. Not part of 'all' or install.
$(WASM_TEST_OUT): $(WASM_CPP)
	$(EMCC) $(EM_CPPFLAGS) $(EM_LDFLAGS) -s ENVIRONMENT=node $(WASM_CPP) -o $(WASM_TEST_OUT)

wasm-test: $(WASM_TEST_OUT) $(WASM_OUT) bin/xdbfind
	wasm/test/run-tests.sh test/test-output/gchazard/xdb.sqlite

# libevent stuff

build-libevent: libevent/Makefile
	$(MAKE) -C libevent

libevent/Makefile:
	cd libevent && CPPFLAGS='$(filter-out -Werror,${CPPFLAGS})' ./configure

# other

DESTDIR  ?= /
PREFIX   ?= usr
NOSTRIP  ?=

INSTROOT := $(DESTDIR)/$(PREFIX)
BINDIR   := $(INSTROOT)/bin
INSTALL  := install
INSTALL_BIN := $(INSTALL) -m 755
ifeq ($(NOSTRIP),)
  INSTALL_BIN += -s
endif

install: all
	$(INSTALL) -d \
		$(INSTROOT)/bin \
		$(INSTROOT)/libexec/sixgill/gcc \
		$(INSTROOT)/libexec/sixgill/scripts/wrap_gcc \
		$(INSTROOT)/libexec/sixgill/wasm
	$(INSTALL_BIN) $(ALL_BINS) $(BINDIR)/
	$(INSTALL_BIN) gcc/xgill.so $(INSTROOT)/libexec/sixgill/gcc/
	$(INSTALL) scripts/run_complete $(INSTROOT)/libexec/sixgill/scripts/
	$(INSTALL) scripts/wrap_gcc/* $(INSTROOT)/libexec/sixgill/scripts/wrap_gcc/
ifeq ($(CLANG_PLUGIN_SUPPORT),yes)
	$(INSTALL) -d $(INSTROOT)/libexec/sixgill/clang $(INSTROOT)/libexec/sixgill/scripts/wrap_clang
	$(INSTALL_BIN) clang/xgill-clang.so $(INSTROOT)/libexec/sixgill/clang/
	$(INSTALL) scripts/wrap_clang/* $(INSTROOT)/libexec/sixgill/scripts/wrap_clang/
endif
ifeq ($(WITHOUT_WASM),)
	$(INSTALL) $(WASM_JS) $(WASM_OUT) $(WASM_OUT:.mjs=.wasm) $(INSTROOT)/libexec/sixgill/wasm/
endif
	tar -C $(DESTDIR) -Jcvf $(DESTDIR)/sixgill.tar.xz $(PREFIX)
	FOLDER=$$(sha256sum $(DESTDIR)/sixgill.tar.xz | head -c12)-sixgill && $(INSTALL) -d $$FOLDER && $(INSTALL) $(DESTDIR)/sixgill.tar.xz $$FOLDER/ && echo "** Created folder $$FOLDER"

clean:
	rm -f ${LIB_OBJS} bin/libxgill.a ${ALL_BINS} main/*.o $(DEPS)
	rm -f wasm/xdb-json-wasm.* wasm/test/xdb-json-wasm.*
	rm -f config.log config.status
	rm -rf test/test-output
	$(MAKE) -C gcc clean
	$(MAKE) -C clang clean
	$(MAKE) -C libevent clean

distclean: clean
	rm -f config.mk config.h
	$(MAKE) -C libevent distclean
