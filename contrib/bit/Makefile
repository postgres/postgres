CFLAGS = -g

varbit: vartest.o varbit.o
	$(CC) $(CFLAGS) -o $@ $^

varbit.o: varbit.c varbit.h
vartest.o: vartest.c varbit.h

clean:
	rm -f *.o varbit
