# $Header: /cvsroot/pgsql/src/backend/utils/misc/Makefile,v 1.21 2003/07/04 16:41:21 tgl Exp $

subdir = src/backend/utils/misc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) $(CPPFLAGS)

OBJS = database.o superuser.o guc.o help_config.o ps_status.o

# This location might depend on the installation directories. Therefore
# we can't subsitute it into config.h.
ifdef krb_srvtab
override CPPFLAGS += -DPG_KRB_SRVTAB='"$(krb_srvtab)"'
endif


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

# guc-file is compiled as part of guc
guc.o: $(srcdir)/guc-file.c

$(srcdir)/guc-file.c: guc-file.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) $<
	sed -e 's/^yy/GUC_yy/g' -e 's/\([^a-zA-Z0-9_]\)yy/\1GUC_yy/g' lex.yy.c > $@
	rm -f lex.yy.c
else
	@$(missing) flex $< $@
endif

# Note: guc-file.c is not deleted by 'make clean',
# since we want to ship it in distribution tarballs.
clean: 
	rm -f SUBSYS.o $(OBJS)
	@rm -f lex.yy.c

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
