#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for bin/pg_passwd
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

OBJS= pg_passwd.o

all: pg_passwd

pg_passwd: $(OBJS)
	$(CC) $(CFLAGS) -o pg_passwd $(OBJS) $(LDFLAGS)

install: pg_passwd
	$(INSTALL) $(INSTL_EXE_OPTS) pg_passwd$(X) $(BINDIR)/pg_passwd$(X)

.PHONY: submake
submake:
	$(MAKE) -C $(LIBPQDIR) libpq.a

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean:
	rm -f pg_passwd$(X) $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
