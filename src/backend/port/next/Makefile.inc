#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/next (NeXTStep 3.3 specific stuff)
#
#-------------------------------------------------------------------------

CFLAGS+= -DUSE_POSIX_TIME -DNO_EMPTY_STMTS

SUBSRCS+= load.c externs.c

HEADERS+= machine.h port-protos.h

LDADD+= -lIPC
