#-------------------------------------------------------------------------
#
# Makefile --
#
#    Makefile for the user_locks module.
#
#-------------------------------------------------------------------------

PGDIR = ../..
SRCDIR = $(PGDIR)/src

include $(SRCDIR)/Makefile.global

CFLAGS += -I. $(CFLAGS_SL)

MODNAME =	user_locks

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

install-doc:	
		if [ -d "$(DOCDIR)" ]; then \
		    cp -p *.doc $(DOCDIR); \
		else \
		    cp -p *.doc $(SQLDIR); \
		fi

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
		$(CC) -MM $(CFLAGS) *.c >depend

clean:
		rm -f *~ $(MODULE) $(MODNAME).sql

ifeq (depend,$(wildcard depend))
include depend
endif
