AROPT = cr
export_dynamic = -Wl,-Bexport

DLSUFFIX = .so
ifeq ($(GCC), yes)
CFLAGS_SL = -fpic
else
CFLAGS_SL = -K PIC
endif

# Rule for building a shared library from a single .o file
%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
