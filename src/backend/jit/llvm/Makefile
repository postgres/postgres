#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile the LLVM JIT provider, building it into a shared library.
#
# Note that this file is recursed into from src/Makefile, not by the
# parent directory..
#
# IDENTIFICATION
#    src/backend/jit/llvm/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/jit/llvm
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifneq ($(with_llvm), yes)
    $(error "not building with LLVM support")
endif

PGFILEDESC = "llvmjit - JIT using LLVM"
NAME = llvmjit

# All files in this directy use LLVM.
CFLAGS += $(LLVM_CFLAGS)
CXXFLAGS += $(LLVM_CXXFLAGS)
override CPPFLAGS := $(LLVM_CPPFLAGS) $(CPPFLAGS)
SHLIB_LINK += $(LLVM_LIBS)
SHLIB_PREREQS += submake-generated-headers

# Because this module includes C++ files, we need to use a C++
# compiler for linking. Makefile.shlib uses $(COMPILER) to build
# loadable modules.
override COMPILER = $(CXX) $(CFLAGS)

OBJS=$(WIN32RES)

# Infrastructure
OBJS += llvmjit.o llvmjit_error.o
# Code generation
OBJS +=

all: all-shared-lib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

include $(top_srcdir)/src/Makefile.shlib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
