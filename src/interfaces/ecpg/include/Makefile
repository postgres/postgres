subdir = src/interfaces/ecpg/include
top_builddir = ../../../..
include ../../../Makefile.global

install: all installdirs install-headers

.PHONY: install-headers
ecpg_headers = ecpgerrno.h ecpglib.h ecpgtype.h sqlca.h sql3types.h
install-headers: $(ecpg_headers)
	for i in $^; do $(INSTALL_DATA) $$i $(includedir); done

installdirs:
	$(mkinstalldirs) $(includedir)

uninstall:
	rm -f $(addprefix $(includedir)/, $(ecpg_headers))
