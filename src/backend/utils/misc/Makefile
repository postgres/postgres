# $Header: /cvsroot/pgsql/src/backend/utils/misc/Makefile,v 1.17 2000/08/25 10:00:31 petere Exp $

subdir = src/backend/utils/misc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = database.o superuser.o guc.o guc-file.o ps_status.o

# This location might depend on the installation directories. Therefore
# we can't subsitute it into config.h.
ifdef krb_srvtab
CPPFLAGS += -DPG_KRB_SRVTAB='"$(krb_srvtab)"'
endif


all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

guc-file.c: guc-file.l
	$(LEX) $(LFLAGS) $<
	sed -e 's/lex\.yy\.c/guc-file\.c/g' \
	    -e 's/^yy/GUC_yy/g' \
	    -e 's/\([^a-zA-Z0-9_]\)yy/\1GUC_yy/g' < lex.yy.c > $@
	rm -f lex.yy.c


# Note: guc-file.c is not deleted by 'make clean',
# since we want to ship it in distribution tarballs.

clean: 
	rm -f SUBSYS.o $(OBJS) lex.yy.c

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
