# Generated automatically from Makefile.in by configure.
POSTGRESTOP=@POSTGRESERVER@
POSTGRES_BIN=$(POSTGRESTOP)/bin
POSTGRES_LIB=$(POSTGRESTOP)/lib

CC=gcc
LEX=flex
LEXLIB=-lfl
YACC=bison -y


CFLAGS=-I../include -O2 -g -Wall

all:: ecpg

clean::
	rm -f *.o core a.out ecpg y.tab.h y.tab.c *~

install:: all
	install -c -d -m755 $(POSTGRES_LIB)/ecpg
	install -c -m555 preproc $(POSTGRES_LIB)/ecpg
	install -c -m555 ecpg $(POSTGRES_BIN)

uninstall::
	rm -f $(POSTGRES_BIN)/ecpg
	rm -f $(POSTGRES_LIB)/ecpg/preproc

# Rule that really do something.
ecpg: y.tab.o pgc.o type.o ecpg.o
	$(CC) -g -O2 -Wall -o ecpg y.tab.o pgc.o type.o ecpg.o -L../lib -lecpg $(LEXLIB)

y.tab.h y.tab.c: preproc.y
	$(YACC) -d $<

y.tab.o : y.tab.h ../include/ecpgtype.h
type.o : ../include/ecpgtype.h
pgc.o : ../include/ecpgtype.h
