# src/test/modules/Makefile

subdir = src/test/modules
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		  worker_spi \
		  dummy_seclabel \
		  test_shm_mq \
		  test_parser

all: submake-errcodes

submake-errcodes:
	$(MAKE) -C $(top_builddir)/src/backend submake-errcodes

$(recurse)
