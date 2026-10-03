
# Sixgill: Static assertion checker for C/C++ programs.
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

# The plugin is compiled with the clang it will be loaded into, using LLVM's
# own flags (in particular -fno-rtti and the libstdc++ ABI setting), and links
# libxgill statically. libxgill is built by the host compiler, which must use
# the same C++ standard library as clang (libstdc++).
PLUGIN_CXX = $(CLANG_DIR)/bin/clang++

XGILL_CLANG_OBJS = plugin.o translate.o types.o names.o

all: xgill-clang.so

# -Bsymbolic and --exclude-libs keep libxgill and the libraries it links
# statically (zlib, sqlite, gmp) from interposing on symbols inside clang.
xgill-clang.so: $(XGILL_CLANG_OBJS) ../bin/libxgill.a
	$(PLUGIN_CXX) -shared -fPIC -o $@ $^ ${HOST_LDFLAGS} \
	  -L$(CLANG_DIR)/lib -lclang-cpp -lLLVM \
	  -lz -lsqlite3 -Wl,-Bstatic -lgmp -Wl,-Bdynamic \
	  -Wl,-Bsymbolic -Wl,--exclude-libs,ALL

%.o: %.cpp xgill-clang.h ../imlang/interface.h ../config.mk
	$(PLUGIN_CXX) $(LLVM_CXXFLAGS) $(HOST_CFLAGS) -I.. -g -O1 -fPIC \
	  -fvisibility-inlines-hidden -Wall -Wno-unused-function -c -o $@ $<

clean:
	rm -f *.o *.so

.PHONY: all clean
