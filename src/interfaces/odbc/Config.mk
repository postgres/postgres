
#
# 
#

#============ Default for all system ==============
OS      = UNX
SHELL   = /bin/sh
AR      = ar r
DLSUFFIX= so
INCDIR = /include
OBJX    =
RANLIB  = ranlib
INSTALL = /usr/bin/install
INSTALL_DATA = $(INSTALL) -c -m 644
MKDIR = mkdir
DESTDIR = /usr/local
LIBDIR = /lib
INSTHEADERS = isql.h isqlext.h iodbc.h
DESTLIBDIR = $(DESTDIR)$(LIBDIR)
DESTINCDIR = $(DESTDIR)$(INCDIR)/iodbc
ODBCSYSTEMDIR = /usr/home/gryschuk

# Remove the comment characters from the section  you want to
# use below, make sure all other sections are commented out.

#============== Linux ELF =========================
CC      = gcc
PIC     = -fPIC
CFLAGSX = -g
#CFLAGSX = -g -Wall -DMY_LOG -DQ_LOG
LDFLAGS = -shared
LIBS    = -ldl

#============= FreeBSD 2.x ========================
# I don't know if this would work but you can always just try it.
# PIC     = -fPIC
# CFLAGSX = -g -Wall
# LDFLAGS = -Bshareable
# LIBS    = 

#===| end of file 'Config.mk' |===


