# ODBC extensions
# Thomas Lockhart 2000-04-03

SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

ifndef PGLIB
  PGLIB= .
endif

CFLAGS+= $(CFLAGS_SL)

TARGETS_7= odbc.sql
TARGETS_PRE7= odbc$(DLSUFFIX) odbc-pre7.sql

TARGETS= $(TARGETS_7)

CLEANFILES+= $(TARGETS) $(TARGETS_PRE7)

all: $(TARGETS)

install: all
ifneq ($(filter odbc$(DLSUFFIX), $(TARGETS)),)
	-test -d $(PGLIB) || $(INSTALL) -d $(PGLIB)
	$(INSTALL) odbc$(DLSUFFIX) $(PGLIB)
endif

pre7:
	$(MAKE) TARGETS="$(TARGETS) $(TARGETS_PRE7)"

install-pre7:
	$(MAKE) TARGETS="$(TARGETS) $(TARGETS_PRE7)" install

odbc-pre7.sql: odbc-pre7.source odbc.sql
	rm -f $@; \
	cat $^ \
	| sed -e "s:_OBJWD_:$(PGLIB):g" \
	      -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	      -e "s:float(15):float8:g" > $@

%.sql: %.source
	rm -f $@; \
	sed -e "s:_OBJWD_:$(PGLIB):g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" < $< > $@

clean:
	rm -f $(TARGETS) *.o
