SRCS		+= $(NAME).c
OBJS		+= $(NAME).o

PG_CPPFLAGS	:=
SHLIB_LINK 	:= $(BE_DLLLIBS)

SO_MAJOR_VERSION := 0
SO_MINOR_VERSION := 0
rpath =

install: all
	$(INSTALL_SHLIB) $(shlib) $(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	$(RM) $(OBJS)

include $(top_srcdir)/src/Makefile.shlib

all: $(shlib)
