# ------------------------------------------------------------------------
# libfif v3.5 / fif v2.5 - FIF2 fractal image format
#
# libfif and fif:  Oskar Behzadpour
# FIF2 format:     Rezar Behzad
# Web:             www.FractalImageCompression.com
# Contact:         oskar@fractalimagecompression.com
#
# Copyright (c) 2026 Oskar Behzadpour
# ------------------------------------------------------------------------

# libfif - FIF2 fractal image format
# make predefines CXX (as g++), so '?=' would never apply: override only the
# built-in default, never a CXX given on the command line or in the environment.
ifeq ($(origin CXX),default)
  CXX    := clang++
endif
CXXSTD   ?= -std=c++20
OPT      ?= -O3 -fno-math-errno -funroll-loops
WARN     ?= -Wall -Wextra -Wno-unused-parameter

# -mcpu=native is an ARM spelling and is rejected outright by an x86 compiler,
# so pick the tuning flag from the host. Windows builds use build.bat instead.
UNAME_M  := $(shell uname -m)
ifneq (,$(filter $(UNAME_M),x86_64 amd64 i686 i386))
  ARCH   ?= -march=native
else
  ARCH   ?= -mcpu=native
endif

PKGCONFIG ?= pkg-config
HAVE_PNG  := $(shell $(PKGCONFIG) --exists libpng && echo 1)
HAVE_JPEG := $(shell $(PKGCONFIG) --exists libjpeg && echo 1)
HAVE_ZLIB := $(shell $(PKGCONFIG) --exists zlib && echo 1)

CPPFLAGS := -Iinclude -Isrc
LDLIBS   :=

ifeq ($(HAVE_PNG),1)
  CPPFLAGS += -DFIF_HAVE_PNG $(shell $(PKGCONFIG) --cflags libpng)
  LDLIBS   += $(shell $(PKGCONFIG) --libs libpng)
endif
# zlib: compressed chunk and raw sections.  Without it such files still
# decode; only their chunks and raw data are unreadable.
ifeq ($(HAVE_ZLIB),1)
  CPPFLAGS += -DFIF_HAVE_ZLIB $(shell $(PKGCONFIG) --cflags zlib)
  LDLIBS   += $(shell $(PKGCONFIG) --libs zlib)
endif
ifeq ($(HAVE_JPEG),1)
  CPPFLAGS += -DFIF_HAVE_JPEG $(shell $(PKGCONFIG) --cflags libjpeg)
  LDLIBS   += $(shell $(PKGCONFIG) --libs libjpeg)
endif

CXXFLAGS := $(CXXSTD) $(OPT) $(WARN) $(ARCH)
DEPFLAGS := -MMD -MP

LIB_SRC  := src/pool.cpp src/encode.cpp src/serialize.cpp src/decode.cpp \
            src/encode_api.cpp src/render.cpp src/image.cpp src/imageio.cpp src/metrics.cpp \
            src/deblock.cpp src/symtable.cpp src/ctxtrain.cpp \
            src/debug.cpp src/format.cpp src/exif.cpp
LIB_OBJ  := $(LIB_SRC:.cpp=.o)
CLI_OBJ  := tools/fif_cli.o

all: fif libfif.a

libfif.a: $(LIB_OBJ)
	@rm -f $@
	ar rcs $@ $^

fif: $(CLI_OBJ) libfif.a
	$(CXX) $(CXXFLAGS) -o $@ $(CLI_OBJ) libfif.a $(LDLIBS) -lpthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(LIB_OBJ:.o=.d) $(CLI_OBJ:.o=.d)

# Address/UB sanitised build for correctness runs.
san:
	$(MAKE) clean
	$(MAKE) OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" ARCH=

clean:
	rm -f $(LIB_OBJ) $(CLI_OBJ) $(LIB_OBJ:.o=.d) $(CLI_OBJ:.o=.d) libfif.a fif

.PHONY: all clean san
