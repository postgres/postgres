#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for array iterator functions.
#
#-------------------------------------------------------------------------

PGDIR = ../..
SRCDIR = $(PGDIR)/src

include $(SRCDIR)/Makefile.global

INCLUDE_OPT =	-I ./ \
		-I $(SRCDIR)/ \
		-I $(SRCDIR)/include \
		-I $(SRCDIR)/port/$(PORTNAME)

CFLAGS += $(INCLUDE_OPT)

ifeq ($(PORTNAME), linux)
  ifdef LINUX_ELF
    ifeq ($(CC), gcc)
      CFLAGS += -fPIC
    endif
  endif
endif

ifeq ($(PORTNAME), i386_solaris)
  CFLAGS+= -fPIC
endif

MODNAME =	array_iterator

MODULE =	$(MODNAME)$(DLSUFFIX)

all:		module sql

module:		$(MODULE)

sql:		$(MODNAME).sql

install:	$(MODULE)
		cp -p $(MODULE) $(LIBDIR)
		cd $(LIBDIR); strip $(MODULE)

%.sql: %.sql.in
		sed "s|MODULE_PATHNAME|$(LIBDIR)/$(MODULE)|" < $< > $@

.SUFFIXES: $(DLSUFFIX)

%$(DLSUFFIX): %.c
		cc $(CFLAGS) -shared -o $@ $<

depend dep:
		$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean:
		rm -f $(MODULE) $(MODNAME).sql

ifeq (depend,$(wildcard depend))
include depend
endif
