AROPT = cr
export_dynamic = -Wl,-Bexport
shlib_symbolic = -Wl,-Bsymbolic

DLSUFFIX = .so
ifeq ($(GCC), yes)
CFLAGS_SL = -fpic
else
CFLAGS_SL = -K PIC
endif
ifeq ($(GXX), yes)
CXXFLAGS_SL = -fpic
else
CXXFLAGS_SL = -K PIC
endif

%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
