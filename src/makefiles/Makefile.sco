AROPT = cr
export_dynamic = -Wl,-Bexport

DLSUFFIX = .so


# Rule for building a shared library from a single .o file
%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
