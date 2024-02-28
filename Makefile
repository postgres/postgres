# The PostgreSQL make files exploit features of GNU make that other
# makes do not have. Because it is a common mistake for users to try
# to build Postgres with a different make, we have this make file
# that, as a service, will look for a GNU make and invoke it, or show
# an error message if none could be found.

# If the user were using GNU make now, this file would not get used
# because GNU make uses a make file named "GNUmakefile" in preference
# to "Makefile" if it exists. PostgreSQL is shipped with a
# "GNUmakefile". If the user hasn't run the configure script yet, the
# GNUmakefile won't exist yet, so we catch that case as well.


# AIX make defaults to building *every* target of the first rule.  Start with
# a single-target, empty rule to make the other targets non-default.
# (We don't support AIX anymore, but if someone tries to build on AIX anyway,
# at least they'll get the instructions to run 'configure' first.)
all:

all check install installdirs installcheck installcheck-parallel uninstall clean distclean maintainer-clean dist distcheck world check-world install-world installcheck-world:
	@if [ ! -f GNUmakefile ] ; then \
	   echo "You need to run the 'configure' program first. Please see"; \
	   echo "<https://www.postgresql.org/docs/devel/installation.html>" ; \
	   false ; \
	 fi
	@IFS=':' ; \
	 for dir in $$PATH; do \
	   for prog in gmake gnumake make; do \
	     if [ -f $$dir/$$prog ] && ( $$dir/$$prog -f /dev/null --version 2>/dev/null | grep GNU >/dev/null 2>&1 ) ; then \
	       GMAKE=$$dir/$$prog; \
	       break 2; \
	     fi; \
	   done; \
	 done; \
	\
	 if [ x"$${GMAKE+set}" = xset ]; then \
	   echo "Using GNU make found at $${GMAKE}"; \
	   unset MAKELEVEL; \
	   $${GMAKE} $@ ; \
	 else \
	   echo "You must use GNU make to build PostgreSQL." ; \
	   false; \
	 fi
