subdir = src/interfaces/ecpg/include
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

install: all installdirs install-headers

.PHONY: install-headers
ecpg_headers = ecpgerrno.h ecpglib.h ecpgtype.h sqlca.h sql3types.h
install-headers: $(ecpg_headers)
	for i in $^; do $(INSTALL_DATA) $$i $(DESTDIR)$(includedir); done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(includedir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(includedir)/, $(ecpg_headers))
