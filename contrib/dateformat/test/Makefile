
PROGRAM	= rand_datetime
	
OBJECTS	= rand_datetime.o

CFLAGS 	= -Wall -fpic -O3
CC	= gcc
RM	= rm -f
LIBS	=
INCLUDE	=

COMPILE = $(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDE)
LINK    = $(CC) $(CFLAGS) -o $@ $(LIBS)


all: $(PROGRAM) 	

$(PROGRAM): $(OBJECTS)
	$(LINK) $(OBJECTS)

.c.o: $<
	$(COMPILE) -c $<

clean:
	$(RM) -f *~ $(OBJECTS) $(PROGRAM)
