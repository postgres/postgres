#-------------------------------------------------------------------------
#
# Makefile--
# Adapted from tutorial makefile
#-------------------------------------------------------------------------

subdir = contrib/xml
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CFLAGS+= $(CFLAGS_SL)


#
# DLOBJS is the dynamically-loaded object files.  The "funcs" queries
# include CREATE FUNCTIONs that load routines from these files.
#
DLOBJS= pgxml$(DLSUFFIX)


QUERIES= pgxml.sql

all: $(DLOBJS) $(QUERIES)

# Requires the expat library

%.so: %.o
	$(CC) -shared -lexpat -o $@ $<


%.sql: %.source
	if [ -z "$$USER" ]; then USER=$$LOGNAME; fi; \
	if [ -z "$$USER" ]; then USER=`whoami`; fi; \
	if [ -z "$$USER" ]; then echo 'Cannot deduce $$USER.'; exit 1; fi; \
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_CWD_:$$C:g" \
	    -e "s:_OBJWD_:$$C:g" \
	    -e "s:_DLSUFFIX_:$(DLSUFFIX):g" \
	    -e "s/_USER_/$$USER/g" < $< > $@

clean:
	rm -f $(DLOBJS) $(QUERIES)
