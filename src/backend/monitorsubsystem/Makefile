#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for monitor subsystem
#
# IDENTIFICATION
#    src/backend/monitorsubsystem/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/monitorsubsystem
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# be-fsstubs is here for historical reasons, probably belongs elsewhere

OBJS = \
	monitor_channel_shm_mq.o \
	monitor_event.o

include $(top_srcdir)/src/backend/common.mk
