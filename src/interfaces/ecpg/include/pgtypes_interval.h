#ifndef PGTYPES_INTERVAL
#define PGTYPES_INTERVAL

typedef struct
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		time;			/* all time units other than months and
								 * years */
#else
	double		time;			/* all time units other than months and
								 * years */
#endif
	long		month;			/* months and years, after time for
								 * alignment */
} interval;

extern interval *PGTYPESinterval_from_asc(char *, char **);
extern char *PGTYPESinterval_to_asc(interval *);
extern int	PGTYPESinterval_copy(interval *, interval *);

#endif   /* PGTYPES_INTERVAL */
