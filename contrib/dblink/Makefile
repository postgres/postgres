subdir = contrib/dblink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include_srcdir = $(top_builddir)/src/include


NAME	:= dblink
SONAME	:= $(NAME)$(DLSUFFIX)

override CFLAGS += -I$(srcdir)
override CFLAGS += -I$(include_srcdir)
override CFLAGS += -I$(libpq_srcdir)
override CFLAGS += $(CFLAGS_SL)
override DLLLIBS := $(BE_DLLLIBS) $(DLLLIBS)

OBJS	= $(NAME).o

SHLIB_LINK= $(libpq)

all: $(OBJS) $(SONAME) $(NAME).sql 

include $(top_builddir)/src/Makefile.shlib

$(OBJS):  $(NAME).c
	$(CC) -o $@ -c $(CFLAGS) $<

$(NAME).sql: $(NAME).sql.in
	sed -e 's:MODULE_PATHNAME:$(libdir)/contrib/$(SONAME):g' < $< > $@

install: all installdirs
	$(INSTALL_DATA) README.$(NAME)	$(docdir)/contrib
	$(INSTALL_DATA) $(NAME).sql	$(datadir)/contrib
	$(INSTALL_SHLIB) $(SONAME)	$(libdir)/contrib

installdirs:
	$(mkinstalldirs) $(docdir)/contrib $(datadir)/contrib $(libdir)/contrib

uninstall:
	rm -rf $(docdir)/contrib/README.$(NAME) $(datadir)/contrib/$(NAME).sql $(libdir)/contrib/$(SONAME)

clean distclean maintainer-clean:
	rm -f $(SONAME) *.o *.sql

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
