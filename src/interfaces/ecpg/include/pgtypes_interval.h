/* src/interfaces/ecpg/include/pgtypes_interval.h */

#ifndef PGTYPES_INTERVAL
#define PGTYPES_INTERVAL

#include <stdint.h>

#include <ecpg_config.h>
#include <pgtypes.h>

#ifndef C_H

typedef int64_t int64;

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
extern void PGTYPESinterval_free(interval * intvl);
extern interval * PGTYPESinterval_from_asc(char *str, char **endptr);
extern char *PGTYPESinterval_to_asc(interval * span);
extern int	PGTYPESinterval_copy(interval * intvlsrc, interval * intvldest);

#ifdef __cplusplus
}
#endif

#endif							/* PGTYPES_INTERVAL */
