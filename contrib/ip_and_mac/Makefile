#
#	PostgreSQL types for IP and MAC addresses
#
#	$Id: Makefile,v 1.2 1998/02/14 17:58:02 scrappy Exp $

all: ip.so mac.so

ip.so: ip.o
	ld -Bshareable -o ip.so ip.o

ip.o: ip.c
	cc -g -O -fPIC -I/usr/local/pgsql/include -c ip.c

mac.so: mac.o
	ld -Bshareable -o mac.so mac.o

mac.o: mac.c mac.h
	cc -g -O -fPIC -I/usr/local/pgsql/include -c mac.c

install: ip.so mac.so
	install -c ip.so mac.so /usr/local/pgsql/modules

#
#	eof
#
