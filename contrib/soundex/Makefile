#
# $Header: /cvsroot/pgsql/contrib/soundex/Attic/Makefile,v 1.8 2000/10/04 19:25:34 petere Exp $
#

subdir = contrib/soundex
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

NAME	:= soundex
SONAME	:= $(NAME)$(DLSUFFIX)

CFLAGS += -I. $(CFLAGS_SL)

all: $(SONAME) $(NAME).sql

$(NAME).sql: $(NAME).sql.in
	sed 's,@MODULE_FILENAME@,$(libdir)/contrib/$(SONAME),g' $< >$@

install: all installdirs
	$(INSTALL_SHLIB) $(SONAME)	$(libdir)/contrib
	$(INSTALL_DATA) $(NAME).sql	$(datadir)/contrib
	$(INSTALL_DATA) README.$(NAME)	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(libdir)/contrib $(datadir)/contrib $(docdir)/contrib

uninstall:
	rm -f $(libdir)/contrib/$(SONAME) $(datadir)/contrib/$(NAME).sql $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean:
	rm -f $(SONAME) $(NAME).o $(NAME).sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
