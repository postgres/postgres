#
#	PostgreSQL types for ISBN and ISSN identifiers.
#
#	$Id: Makefile,v 1.1 1998/08/17 03:35:04 scrappy Exp $

all: isbn.so issn.so

isbn.so: isbn.o
	ld -Bshareable -o isbn.so isbn.o

isbn.o: isbn.c
	cc -g -O -fPIC -I/usr/local/pgsql/include -c isbn.c

issn.so: issn.o
	ld -Bshareable -o issn.so issn.o

issn.o: issn.c
	cc -g -O -fPIC -I/usr/local/pgsql/include -c issn.c

install: isbn.so issn.so
	install -c isbn.so issn.so /usr/local/pgsql/modules

#
#	eof
#
