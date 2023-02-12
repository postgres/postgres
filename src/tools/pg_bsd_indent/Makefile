#-------------------------------------------------------------------------
#
# Makefile for pg_bsd_indent
#
# Copyright (c) 2017, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_bsd_indent - indent C code nicely"
PGAPPICON = win32

PROGRAM = pg_bsd_indent
OBJS	= args.o err.o indent.o io.o lexi.o parse.o pr_comment.o $(WIN32RES)

# clean junk left behind by "make test"
EXTRA_CLEAN = *.out *.list tests.diff

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# pgxs.mk assumes too much about what "make check" means, so call it "test"
.PHONY: test

test: $(PROGRAM)
	@rm -f tests.diff
	@cp $(srcdir)/tests/*.list .
	@for testsrc in $(srcdir)/tests/*.0; do \
		test=`basename "$$testsrc" .0`; \
		./$(PROGRAM) $$testsrc $$test.out -P$(srcdir)/tests/$$test.pro || echo FAILED >>$$test.out; \
		diff -u $$testsrc.stdout $$test.out >>tests.diff 2>&1 || true; \
	done
	@cat tests.diff
	@test '!' -s tests.diff
	@echo Tests complete.
