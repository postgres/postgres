#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/misc
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/misc/Makefile,v 1.16 2000/06/04 01:44:34 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include $(SRCDIR)/Makefile.global

OBJS = database.o superuser.o guc.o guc-file.o ps_status.o

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
