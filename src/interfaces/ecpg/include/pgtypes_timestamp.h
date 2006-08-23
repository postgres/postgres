/* $PostgreSQL: pgsql/src/interfaces/ecpg/include/pgtypes_timestamp.h,v 1.11 2006/08/23 12:01:52 meskes Exp $ */

#ifndef PGTYPES_TIMESTAMP
#define PGTYPES_TIMESTAMP

/* pgtypes_interval.h includes ecpg_config.h */
#include <pgtypes_interval.h>

#ifdef HAVE_INT64_TIMESTAMP
typedef int64 timestamp;
typedef int64 TimestampTz;
#else
typedef double timestamp;
typedef double TimestampTz;
#endif

#ifdef __cplusplus
extern		"C"
{
#endif

extern timestamp PGTYPEStimestamp_from_asc(char *, char **);
extern char *PGTYPEStimestamp_to_asc(timestamp);
extern int	PGTYPEStimestamp_sub(timestamp *, timestamp *, interval *);
extern int	PGTYPEStimestamp_fmt_asc(timestamp *, char *, int, char *);
extern void PGTYPEStimestamp_current(timestamp *);
extern int	PGTYPEStimestamp_defmt_asc(char *, char *, timestamp *);
extern int	PGTYPEStimestamp_add_interval(timestamp * tin, interval * span, timestamp * tout);
extern int	PGTYPEStimestamp_sub_interval(timestamp * tin, interval * span, timestamp * tout);

#ifdef __cplusplus
}
#endif

#endif   /* PGTYPES_TIMESTAMP */
