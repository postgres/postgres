subdir = src/interfaces/ecpg/include
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

informix_esql_dir = $(pkgincludedir)/informix/esql

install: all installdirs install-headers

.PHONY: install-headers
ecpg_headers = ecpgerrno.h ecpglib.h ecpgtype.h sqlca.h sql3types.h ecpg_informix.h \
	pgtypes_error.h pgtypes_numeric.h pgtypes_timestamp.h pgtypes_date.h pgtypes_interval.h
informix_headers = datetime.h decimal.h sqltypes.h sqlda.h

install-headers: $(ecpg_headers) $(informix_headers)
	for i in $(ecpg_headers); do $(INSTALL_DATA) $(srcdir)/$$i $(DESTDIR)$(includedir) || exit; done
	for i in $(informix_headers); do $(INSTALL_DATA) $(srcdir)/$$i $(DESTDIR)$(informix_esql_dir) || exit; done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(includedir) $(DESTDIR)$(informix_esql_dir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(includedir)/, $(ecpg_headers))
	rm -f $(addprefix $(DESTDIR)$(informix_esql_dir)/, $(informix_headers))
