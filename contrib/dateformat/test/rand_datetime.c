
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>


char	*month[]	= {
	"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec",NULL        
};

char	*day[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat", NULL };

int num(FILE *f, int min, int max)
{
	int	x, y, one;
	
	one = x = fgetc(f);
	
	
	if (x < min)
		x = min;	
	else if (x > max) {
		while(x > max)
			x /= 2;	 
		return x;
	}
	
	do {
		y = fgetc(f);
		if ((x+y) > max)
			return x;
		x += y;
	} while(--one > 0);
	
	return x;	
}

int main(int argc, char **argv)
{
	FILE	*f; 
	int	count;
	
	if (argc < 5) {
		printf("\nUsage: %s <randfile> <num> <prefix> <postfix>\n", argv[0]);
		printf("\n(C) Karel Zak - Zakkr 1999\n\n");
		exit(1);
	}
	
	if ((f = fopen(argv[1], "r")) == NULL) {
		perror(argv[1]);
		exit(1);
	}

	count = atoi(argv[2]);
	
	for(; count > 0; --count) {
		fprintf(stdout, "%s%s %02d %s %02d:%02d:%02d %d%s\n",
			argv[3],
			day[ num(f, 0, 6) ],
			num(f, 1, 28),
			month[ num(f, 0, 11) ],	
			num(f, 0, 23),
			num(f, 0, 59),
			num(f, 0, 59),
			num(f, 0, 9999),
			argv[4]
		);	
	}	
	exit(0);
}