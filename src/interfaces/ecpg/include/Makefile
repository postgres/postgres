subdir = src/interfaces/ecpg/include
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

install: all installdirs install-headers

.PHONY: install-headers
ecpg_headers = ecpgerrno.h ecpglib.h ecpgtype.h sqlca.h sql3types.h ecpg_informix.h pgtypes_error.h pgtypes_numeric.h pgtypes_timestamp.h pgtypes_date.h datetime.h decimal.h pgtypes_interval.h sqltypes.h compatlib.h sqlda.h

install-headers: $(ecpg_headers)
	for i in $^; do $(INSTALL_DATA) $$i $(DESTDIR)$(includedir); done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(includedir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(includedir)/, $(ecpg_headers))
