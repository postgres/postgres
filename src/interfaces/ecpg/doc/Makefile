#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for doc directory to install man pages
#
#-------------------------------------------------------------------------

SRCDIR=../../..
include $(SRCDIR)/Makefile.global

all: ecpg.texinfo
	-makeinfo ecpg.texinfo

install: install-man install-info

install-man:
	-mkdir -p $(POSTMANDIR)
	-mkdir $(POSTMANDIR)/man1
	#-mkdir $(POSTMANDIR)/man3
	#-mkdir $(POSTMANDIR)/man5
	#-mkdir $(POSTMANDIR)/manl
	install -m644 *.1* $(POSTMANDIR)/man1
	#install -m644 *.3* $(POSTMANDIR)/man3
	#install -m644 *.5* $(POSTMANDIR)/man5
	#install -m644 *.l* $(POSTMANDIR)/manl

install-info: ecpg.info
	install -m 755 -d $(DESTDIR)$(POSTGRESDIR)/info
	install -m 644 ecpg.info $(DESTDIR)$(POSTGRESDIR)/info; \
	#if $(SHELL) -c 'install-info --version' >/dev/null 2>&1; then\
	#	install-info --infodir=$(infodir) ecpg.info; \
	#else true; \
	#fi

clean:
	rm -f ecpg.info ecpg.?? ecpg.??? missfont.log *~ core

distclean: clean
	rm -f Makefile
