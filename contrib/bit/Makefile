ifndef PGDIR
PGDIR= ../..
PGDIR=/data/build/postgresql-7.0beta3
endif

SRCDIR= $(PGDIR)/src

include $(SRCDIR)/Makefile.global

TARGETS= varbit.sql varbit$(DLSUFFIX)
# vartest
SOURCE= varbit.c varbit_glue.c
OBJ= $(SOURCE:.c=.o)
CFLAGS += -g

all:	$(TARGETS)

varbit$(DLSUFFIX):	$(OBJ)
	$(CC) $(CFLAGS) -shared -o varbit$(DLSUFFIX) $(SOURCE) $(CLIBS)

vartest: varbit.o vartest.o
	$(CC) -o $@ varbit.o vartest.o

install:
	$(MAKE) all
	cp -p varbit$(DLSUFFIX) $(LIBDIR)/contrib
	chmod 555 $(LIBDIR)/contrib/varbit$(DLSUFFIX)

%.sql: %.source
	echo $(SRCDIR)
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	C=`pwd`; \
	O=$C; \
	if [ -d ${LIBDIR}/contrib ]; then O=${LIBDIR}/contrib; else \
	echo "contrib directory does not exist."; fi; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$O:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s/_USER_/$$USER/g" < $< > $@

clean: 
	rm -f $(TARGETS) varbit.o

