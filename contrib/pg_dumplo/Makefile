
PROGRAM	= pg_dumplo
	
OBJECTS	= pg_dumplo.o

CFLAGS 	= -Wall -fpic -g
CC	= gcc
RM	= rm -f
INCLUDE = -I/usr/include/postgresql
LIBS	=-lpq

COMPILE = $(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDE)
LINK    = $(CC) $(CFLAGS) -o $@ $(LIBS)


all: $(PROGRAM) 	

$(PROGRAM): $(OBJECTS)
	$(LINK) $(OBJECTS)

.c.o: $<
	$(COMPILE) -c $<

clean:
	$(RM) -f *~ $(OBJECTS) $(PROGRAM)

