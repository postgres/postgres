#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/bsdi_2_1
#
# NOTES
#    The BSD/OS port is included here by courtesy of Kurt Lidl.
#
#    (5) 1994, Kurt Lidl, lidl@pix.com
#
#-------------------------------------------------------------------------

CFLAGS+=-DUSE_POSIX_TIME -DNEED_CBRT
LDADD+= -ldl -lipc
