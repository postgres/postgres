override CFLAGS += -dy
export_dynamic = -W l,-Bexport

%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
