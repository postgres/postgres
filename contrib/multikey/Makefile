
POSTSRC= /home/postgres/postgres95/src

include $(POSTSRC)/Makefile.global
include $(POSTSRC)/Makefile.custom

CFLAGS= $(CFLAGS_SL) -Wall -Wmissing-prototypes -I$(POSTSRC)/include

%.so: %.o
	$(LD) -x -r -o $<.obj $<
	@echo building shared object $@
	@rm -f $@.pic
	@${AR} cq $@.pic `lorder $<.obj | tsort`
	${RANLIB} $@.pic
	@rm -f $@
	$(LD) -x -Bshareable -Bforcearchive \
	  -o $@ $@.pic

#
# build dynamically-loaded object file
#
DLOBJS= multikey.so
objdir= ./

CLEANFILES+= $(DLOBJS)

all:: $(DLOBJS)

$(DLOBJS):	multikey.c
