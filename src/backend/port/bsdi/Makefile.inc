#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/bsdi
#
# NOTES
#    The BSD/OS port is included here by courtesy of Kurt Lidl.
#
#    (5) 1994, Kurt Lidl, lidl@pix.com
#
#-------------------------------------------------------------------------

LDADD+= -ldld -lipc
SUBSRCS= dynloader.c
