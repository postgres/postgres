#
#	PostgreSQL types for IP and MAC addresses
#
#	$Id: Makefile,v 1.5 1998/06/16 05:35:10 momjian Exp $

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
	install -c ip.so mac.so /usr/local/pgsql/contrib/ip_and_macs

clean:
	rm -f *.o *.so *.b
#
#	eof
#
