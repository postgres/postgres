override CFLAGS += -dy
export_dynamic = -W l,-Bexport
AROPT = cq

DLSUFFIX = .so
CFLAGS_SL = -K PIC

%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
