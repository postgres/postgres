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

# LLVM 14 produces deprecation warnings.  We'll need to make some changes
# before the relevant functions are removed, but for now silence the warnings.
ifeq ($(GCC), yes)
LLVM_CFLAGS += -Wno-deprecated-declarations
endif

# All files in this directory use LLVM.
CFLAGS += $(LLVM_CFLAGS)
CXXFLAGS += $(LLVM_CXXFLAGS)
override CPPFLAGS := $(LLVM_CPPFLAGS) $(CPPFLAGS)
SHLIB_LINK += $(LLVM_LIBS)

# Because this module includes C++ files, we need to use a C++
# compiler for linking. Makefile.shlib uses $(COMPILER) to build
# loadable modules.
override COMPILER = $(CXX) $(CXXFLAGS)

OBJS = \
	$(WIN32RES)

# Infrastructure
OBJS += \
	llvmjit.o \
	llvmjit_error.o \
	llvmjit_inline.o \
	llvmjit_wrap.o

# Code generation
OBJS += \
	llvmjit_deform.o \
	llvmjit_expr.o

all: all-shared-lib llvmjit_types.bc

install: all installdirs install-lib install-types

installdirs: installdirs-lib

uninstall: uninstall-lib uninstall-types

# Note this is intentionally not in bitcodedir, as it's not for inlining */
install-types: llvmjit_types.bc
	$(INSTALL_DATA) llvmjit_types.bc '$(DESTDIR)$(pkglibdir)'

uninstall-types:
	rm -f '$(DESTDIR)$(pkglibdir)/llvmjit_types.bc'

include $(top_srcdir)/src/Makefile.shlib

clean distclean: clean-lib
	rm -f $(OBJS)
	rm -f llvmjit_types.bc
