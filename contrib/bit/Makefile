# Makefile
# For the bit/varbit data types

SRCDIR= ../../src

include $(SRCDIR)/Makefile.global

INSTALLDIR= $(LIBDIR)
MODDIR= $(INSTALLDIR)/modules
SQLDIR= $(INSTALLDIR)/sql

TARGETS= varbit.sql varbit$(DLSUFFIX)
# vartest
SOURCE= varbit.c varbit_glue.c
OBJ= $(SOURCE:.c=.o)
CFLAGS += -g

all:	$(TARGETS)

vartest: varbit.o vartest.o
	$(CC) -o $@ varbit.o vartest.o

install:
	$(MAKE) all
	-test -d $(INSTALLDIR) || $(INSTALL) -d $(INSTALLDIR)
	-test -d ${MODDIR} || $(INSTALL) -d ${MODDIR}
	-test -d ${SQLDIR} || $(INSTALL) -d ${SQLDIR}
	$(INSTALL) -m 555 $(filter %$(DLSUFFIX), $(TARGETS)) $(MODDIR)
	$(INSTALL) -m 664 $(filter %.sql, $(TARGETS)) $(SQLDIR)

%.sql: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	C=`pwd`; \
	O=${MODDIR}; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$O:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s/_USER_/$$USER/g" < $< > $@

clean: 
	rm -f $(TARGETS) varbit.o

