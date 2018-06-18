/* src/interfaces/ecpg/include/pgtypes_interval.h */

#ifndef PGTYPES_INTERVAL
#define PGTYPES_INTERVAL

#include <ecpg_config.h>
#include <pgtypes.h>

#ifndef C_H

#ifdef HAVE_LONG_INT_64
#ifndef HAVE_INT64
typedef long int int64;
#endif
#elif defined(HAVE_LONG_LONG_INT_64)
#ifndef HAVE_INT64
typedef long long int int64;
#endif
#else
/* neither HAVE_LONG_INT_64 nor HAVE_LONG_LONG_INT_64 */
#error must have a working 64-bit integer datatype
#endif

#define HAVE_INT64_TIMESTAMP
#endif							/* C_H */

typedef struct
{
	int64		time;			/* all time units other than months and years */
	long		month;			/* months and years, after time for alignment */
}			interval;

#ifdef __cplusplus
extern "C"
{
#endif

extern interval * PGTYPESinterval_new(void);
extern void PGTYPESinterval_free(interval *);
extern interval * PGTYPESinterval_from_asc(char *, char **);
extern char *PGTYPESinterval_to_asc(interval *);
extern int	PGTYPESinterval_copy(interval *, interval *);

#ifdef __cplusplus
}
#endif

#endif							/* PGTYPES_INTERVAL */
