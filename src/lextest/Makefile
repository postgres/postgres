#
# Makefile - test for 2.5.3 flex bug
# Bruce Momjian 5/14/97
#
.DEFAULT all : lextest

install:

depend:

lextest: lextest.c scan.l
	$(LEX) scan.l
	$(CC) -c lex.yy.c
	$(CC) -c lextest.c
	$(CC) -o lextest lex.yy.o lextest.o
	@echo "If this fails, flex is broken" | lextest || echo "You have a buggy version of flex.  Read doc/README.flex for a fix."

clean:
	rm -f lextest lex.yy.c lex.yy.o lextest.o
