subdir = src/interfaces/ecpg/test/preproc
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress


TESTS = array_of_struct array_of_struct.c \
	autoprep autoprep.c \
	comment comment.c \
	cursor cursor.c \
	define define.c \
	init init.c \
	strings strings.c \
	outofscope outofscope.c \
	type type.c \
	variable variable.c \
	whenever whenever.c \
	pointer_to_struct pointer_to_struct.c

all: $(TESTS)

array_of_struct.c: array_of_struct.pgc $(ECPG_TEST_DEPENDENCIES)
	$(ECPG) -c -o $@ $<

pointer_to_struct.c: pointer_to_struct.pgc $(ECPG_TEST_DEPENDENCIES)
	$(ECPG) -c -o $@ $<

autoprep.c: autoprep.pgc $(ECPG_TEST_DEPENDENCIES)
	$(ECPG) -r prepare -o $@ $<

strings.c: strings.pgc strings.h $(ECPG_TEST_DEPENDENCIES)
	$(ECPG) -i -o $@ $<
