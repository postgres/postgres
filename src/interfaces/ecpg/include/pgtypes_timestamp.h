#ifndef PGTYPES_TIMESTAMP
#define PGTYPES_TIMESTAMP

#include <pgtypes_interval.h>

#ifdef HAVE_INT64_TIMESTAMP
typedef int64 timestamp;
typedef int64 TimestampTz;

#else
typedef double timestamp;
typedef double TimestampTz;
#endif

extern timestamp PGTYPEStimestamp_from_asc(char *, char **);
extern char *PGTYPEStimestamp_to_asc(timestamp);
extern int	PGTYPEStimestamp_sub(timestamp *, timestamp *, interval *);
extern int	PGTYPEStimestamp_fmt_asc(timestamp *, char *, int, char *);
extern void PGTYPEStimestamp_current(timestamp *);
extern int	PGTYPEStimestamp_defmt_asc(char *, char *, timestamp *);

#endif   /* PGTYPES_TIMESTAMP */
