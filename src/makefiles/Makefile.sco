CFLAGS += -dy
LDFLAGS += -W l,-Bexport

%.so: %.o
	$(LD) -G -Bdynamic -o $@ $<
