#
# $Header: /cvsroot/pgsql/contrib/spi/Makefile,v 1.20 2001/04/04 21:15:56 tgl Exp $
#

subdir = contrib/spi
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

OBJS	= autoinc.o insert_username.o moddatetime.o refint.o timetravel.o
DOCS	= README.spi
SQLS	= $(OBJS:.o=.sql)
EXAMPLES= $(OBJS:.o=.example)
MODS	= $(OBJS:.o=$(DLSUFFIX))

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)

ifdef REFINT_VERBOSE
override CPPFLAGS+= -DREFINT_VERBOSE
endif

override DLLLIBS := $(BE_DLLLIBS) $(DLLLIBS)


all: $(MODS) $(SQLS)

%.sql: %.sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/contrib/$(@:.sql=$(DLSUFFIX)):g' < $< > $@

install: all installdirs
	for inst_file in $(SQLS); do \
	    $(INSTALL_DATA) $$inst_file $(datadir)/contrib || exit; \
	done
	for inst_file in $(MODS); do \
	    $(INSTALL_SHLIB) $$inst_file $(libdir)/contrib || exit; \
	done
	for inst_file in $(EXAMPLES) README.spi; do \
	    $(INSTALL_DATA) $$inst_file $(docdir)/contrib/spi || exit; \
	done

installdirs:
	$(mkinstalldirs) $(datadir)/contrib $(libdir)/contrib $(docdir)/contrib/spi

uninstall:
	rm -f $(addprefix $(datadir)/contrib/, $(SQLS)) \
	  $(addprefix $(libdir)/contrib/, $(MODS)) \
	  $(addprefix $(docdir)/contrib/spi/, $(EXAMPLES) README.spi)

clean distclean maintainer-clean:
	rm -f $(MODS) $(SQLS) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
