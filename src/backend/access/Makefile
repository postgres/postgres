#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the access methods module
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/Makefile,v 1.3 1997/06/11 01:11:30 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

OBJS = common/SUBSYS.o gist/SUBSYS.o hash/SUBSYS.o heap/SUBSYS.o \
       index/SUBSYS.o rtree/SUBSYS.o nbtree/SUBSYS.o transam/SUBSYS.o


all: submake SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake
submake:
	$(MAKE) -C common  SUBSYS.o
	$(MAKE) -C gist    SUBSYS.o
	$(MAKE) -C hash    SUBSYS.o
	$(MAKE) -C heap    SUBSYS.o
	$(MAKE) -C index   SUBSYS.o
	$(MAKE) -C rtree   SUBSYS.o
	$(MAKE) -C nbtree  SUBSYS.o
	$(MAKE) -C transam SUBSYS.o

clean:
	rm -f SUBSYS.o
	$(MAKE) -C common   clean
	$(MAKE) -C gist     clean
	$(MAKE) -C hash     clean
	$(MAKE) -C heap     clean
	$(MAKE) -C index    clean
	$(MAKE) -C rtree    clean
	$(MAKE) -C nbtree   clean
	$(MAKE) -C transam  clean

.DEFAULT: 
	$(MAKE) -C common   $@
	$(MAKE) -C gist     $@
	$(MAKE) -C hash     $@
	$(MAKE) -C heap     $@
	$(MAKE) -C index    $@
	$(MAKE) -C rtree    $@
	$(MAKE) -C nbtree   $@
	$(MAKE) -C transam  $@

