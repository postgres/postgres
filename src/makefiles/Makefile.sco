override CFLAGS += -dy
export_dynamic = -W l,-Bexport
AROPT = cr

DLSUFFIX = .so
CFLAGS_SL = -K PIC

%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
