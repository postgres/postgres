# $PostgreSQL: pgsql/src/bin/pg_config/Makefile,v 1.8 2004/08/01 06:56:38 momjian Exp $

subdir = src/bin/pg_config
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS=   pg_config.o exec.o

override CPPFLAGS :=  -DFRONTEND -I$(libpq_srcdir) -DVAL_CONFIGURE="\"$(configure_args)\"" $(CPPFLAGS)

all: submake-libpgport pg_config

exec.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

pg_config: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_SCRIPT) pg_config$(X) $(DESTDIR)$(bindir)/pg_config$(X)
  
installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_config

clean distclean maintainer-clean:
	rm -f pg_config$(X) $(OBJS) exec.c
