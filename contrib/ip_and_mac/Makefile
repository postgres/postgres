#	PostgreSQL type definitions for IP and MAC addresses.

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

#	eof
