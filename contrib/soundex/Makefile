#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for soundex
#
#-------------------------------------------------------------------------

PGDIR = ../..
SRCDIR = $(PGDIR)/src

include $(SRCDIR)/Makefile.global

CFLAGS += -I. $(CFLAGS_SL)

MODNAME =	soundex

MODULE =	$(MODNAME)$(DLSUFFIX)

all:		module sql

module:		$(MODULE)

sql:		$(MODNAME).sql

install:	$(MODULE)
		cp -p $(MODULE) $(LIBDIR)/modules
		cd $(LIBDIR)/modules; strip $(MODULE)

%.sql: %.sql.in
		sed "s|MODULE_PATHNAME|$(LIBDIR)/modules/$(MODULE)|" < $< > $@

.SUFFIXES: $(DLSUFFIX)

%$(DLSUFFIX): %.c
		$(CC) $(CFLAGS) -shared -o $@ $<

depend dep:
		$(CC) -MM $(CFLAGS) *.c >depend

clean:
		rm -f $(MODULE) $(MODNAME).sql

ifeq (depend,$(wildcard depend))
include depend
endif
