#
# Makefile for example programs
#

CPP_PROG = true

MKDIR=	../../mk
include $(MKDIR)/postgres.mk

CXXFLAGS+= -I$(HEADERDIR) -I$(srcdir)/libpq -I$(srcdir)/backend \
 	 -I$(srcdir)/backend/include

LD_ADD+=-L$(LIBDIR) -lpq++ -lpq

#
# And where libpq goes, so goes the authentication stuff...
#
ifdef KRBVERS
LD_ADD+= $(KRBLIBS)
CXXFLAGS+= $(KRBFLAGS)
endif

P0_PROG:= testlibpq0
P0_OBJS:= testlibpq0.o

$(P0_PROG):  $(addprefix $(objdir)/,$(P0_OBJS))
	$(CXX) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

P1_PROG:= testlibpq1
P1_OBJS:= testlibpq1.o

$(P1_PROG):  $(addprefix $(objdir)/,$(P1_OBJS))
	$(CXX) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

P2_PROG:= testlibpq2
P2_OBJS:= testlibpq2.o

$(P2_PROG):  $(addprefix $(objdir)/,$(P2_OBJS))
	$(CXX) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

P3_PROG:= testlibpq3
P3_OBJS:= testlibpq3.o

$(P3_PROG):  $(addprefix $(objdir)/,$(P3_OBJS))
	$(CXX) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

P4_PROG:= testlibpq4
P4_OBJS:= testlibpq4.o

$(P4_PROG):  $(addprefix $(objdir)/,$(P4_OBJS))
	$(CXX) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

P5_PROG:= testlo
P5_OBJS:= testlo.o

$(P5_PROG):  $(addprefix $(objdir)/,$(P5_OBJS))
	$(CXX) $(CDEBUG) -o $(objdir)/$(@F) $< $(LD_ADD)

OBJS:= $(P0_OBJS) $(P1_OBJS) $(P2_OBJS) $(P3_OBJS) $(P4_OBJS) $(P5_OBJS)
PROGS:= $(P0_PROG) $(P1_PROG) $(P2_PROG) $(P3_PROG) $(P4_PROG) $(P5_PROG)

CLEANFILES+= $(OBJS) $(PROGS)

all:: $(PROGS)

install:: $(PROGS)
	@for i in ${PROGS}; do \
		echo "Installing $$i"; \
		$(INSTALL) $(objdir)/$$i $(DESTDIR)$(BINDIR)/$$i;\
	done
