D=/usr/postgres
P=$D/lib/datetime_functions.so
CFLAGS=-fpic -O -I../../src/include -I../../src/backend

all: $P datetime_functions.sql

$P:datetime_functions.o 
	ld -Bshareable -o $P datetime_functions.o 

datetime_functions.sql: datetime.prot
	sh datetime.prot $P
	psql -c "\idatetime_functions.sql" template1
