#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tutorial
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/tutorial/Makefile,v 1.3 1996/11/13 10:36:24 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ..
include ../Makefile.global

CFLAGS+= -I$(LIBPQDIR) -I../../include

LDADD+= -L$(LIBPQDIR) -lpq
        
#
# DLOBJS is the dynamically-loaded object files.  The "funcs" queries
# include CREATE FUNCTIONs that load routines from these files.
#
DLOBJS= complex$(SLSUFF) funcs$(SLSUFF)

QUERIES= advanced.sql basics.sql complex.sql funcs.sql syscat.sql
#
# plus exports files
#
ifdef EXPSUFF
DLOBJS+= $(DLOBJS:.o=$(EXPSUFF))
endif

all: $(QUERIES)

%.sql: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$C:g" \
	    -e "s:_SLSUFF_:$(SLSUFF):g" \
	    -e "s/_USER_/$$USER/g" < $< > $@

funcs.sql:: $(DLOBJS)

$(DLOBJS): 
	$(MAKE) -C C-code $@
	cp C-code/$@ .

clean:
	$(MAKE) -C C-code clean
	rm -f $(QUERIES)
	rm -f $(DLOBJS)
