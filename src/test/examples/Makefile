#
# Makefile for example programs
#

MKDIR=	../../mk
include $(MKDIR)/postgres.mk

CFLAGS+= -I$(HEADERDIR) -I$(srcdir)/backend -I$(srcdir)/backend/include

LIBPQ:=  -L$(LIBDIR) -lpq

LD_ADD+=$(LIBPQ)

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LD_ADD+= $(KRBLIBS)
CFLAGS+= $(KRBFLAGS)
endif

P1_PROG:= testlibpq
P1_OBJS:= testlibpq.o

$(P1_PROG):  $(addprefix $(objdir)/,$(P1_OBJS))
	$(CC) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

P2_PROG:= testlibpq2
P2_OBJS:= testlibpq2.o

$(P2_PROG):  $(addprefix $(objdir)/,$(P2_OBJS))
	$(CC) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)


P3_PROG:= testlibpq3
P3_OBJS:= testlibpq3.o

$(P3_PROG):  $(addprefix $(objdir)/,$(P3_OBJS))
	$(CC) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

P4_PROG:= testlo
P4_OBJS:= testlo.o

$(P4_PROG):  $(addprefix $(objdir)/,$(P4_OBJS))
	$(CC) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

OBJS:= $(P1_OBJS) $(P2_OBJS) $(P3_OBJS) $(P4_OBJS)
PROGS:= $(P1_PROG) $(P2_PROG) $(P3_PROG) $(P4_PROG)

CLEANFILES+= $(OBJS) $(PROGS)

all:: $(PROGS)

install:: $(PROGS)
	@for i in ${PROGS}; do \
		echo "Installing $$i"; \
		$(INSTALL) $(objdir)/$$i $(DESTDIR)$(BINDIR)/$$i;\
	done
	















