#-------------------------------------------------------------------------
#
# Makefile --
#
#    Makefile for new datetime module.
#
#-------------------------------------------------------------------------

PGDIR = ../..
SRCDIR = $(PGDIR)/src

include $(SRCDIR)/Makefile.global

INCLUDE_OPT =	-I ./ \
		-I $(SRCDIR)/ \
		-I $(SRCDIR)/include \
		-I $(SRCDIR)/port/$(PORTNAME)

CFLAGS += $(INCLUDE_OPT) $(CFLAGS_SL)

MODNAME =	datetime_functions

SQLDEFS =	$(MODNAME).sql

MODULE =	$(MODNAME)$(DLSUFFIX)

MODDIR =	$(LIBDIR)/modules

SQLDIR =	$(LIBDIR)/sql

all:		module sql

module:		$(MODULE)

sql:		$(SQLDEFS)

install:	$(MODULE) $(SQLDEFS) $(MODDIR) $(SQLDIR)
		cp -p $(MODULE) $(MODDIR)/
		strip $(MODDIR)/$(MODULE)
		cp -p $(SQLDEFS) $(SQLDIR)/

$(MODDIR):
		mkdir -p $@

$(SQLDIR):
		mkdir -p $@

%.sql: %.sql.in
		sed "s|MODULE_PATHNAME|$(MODDIR)/$(MODULE)|" < $< > $@

.SUFFIXES: $(DLSUFFIX)

%$(DLSUFFIX): %.c
		$(CC) $(CFLAGS) -shared -o $@ $<

depend dep:
		$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean:
		rm -f *~ $(MODULE) $(MODNAME).sql

ifeq (depend,$(wildcard depend))
include depend
endif
