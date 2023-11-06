subdir = src/interfaces/ecpg/include
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I../include -I$(top_srcdir)/src/interfaces/ecpg/include \
	-I$(libpq_srcdir) -I$(top_builddir)/src/port $(CPPFLAGS)

informix_esql_dir = $(pkgincludedir)/informix/esql
ecpg_config_h = $(top_builddir)/src/interfaces/ecpg/include/ecpg_config.h

all: $(ecpg_config_h)

install: all installdirs install-headers

.PHONY: install-headers
ecpg_headers = ecpgerrno.h ecpglib.h ecpgtype.h sqlca.h sql3types.h ecpg_informix.h \
	pgtypes_error.h pgtypes_numeric.h pgtypes_timestamp.h pgtypes_date.h pgtypes_interval.h pgtypes.h \
	sqlda.h sqlda-compat.h sqlda-native.h
informix_headers = datetime.h decimal.h sqltypes.h

install-headers: $(ecpg_headers) $(informix_headers) installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/,$(ecpg_headers)) '$(DESTDIR)$(includedir)/'
	$(INSTALL_DATA) $(addprefix $(srcdir)/,$(informix_headers)) '$(DESTDIR)$(informix_esql_dir)/'
	$(INSTALL_DATA) $(ecpg_config_h) '$(DESTDIR)$(includedir)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(includedir)' '$(DESTDIR)$(informix_esql_dir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(includedir)'/, $(ecpg_headers))
	rm -f $(addprefix '$(DESTDIR)$(informix_esql_dir)'/, $(informix_headers))
	rm -f '$(DESTDIR)$(includedir)'/$(notdir $(ecpg_config_h))

distclean:
	rm -f ecpg_config.h stamp-h
