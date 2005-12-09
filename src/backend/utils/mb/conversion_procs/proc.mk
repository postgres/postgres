SRCS		+= $(NAME).c
OBJS		+= $(NAME).o

SHLIB_LINK 	:= $(BE_DLLLIBS)

SO_MAJOR_VERSION := 0
SO_MINOR_VERSION := 0
rpath =

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs
ifeq ($(enable_shared), yes)
	$(INSTALL_SHLIB) $(shlib) '$(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)'
endif

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(pkglibdir)'

uninstall:
	rm -f '$(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)'

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
