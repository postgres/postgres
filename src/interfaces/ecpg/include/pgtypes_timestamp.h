#ifndef PGTYPES_TIMESTAMP
#define PGTYPES_TIMESTAMP

#include <pgtypes_interval.h>

#ifdef HAVE_INT64_TIMESTAMP
typedef int64 Timestamp;
typedef int64 TimestampTz;

#else
typedef double Timestamp;
typedef double TimestampTz;
#endif

extern Timestamp PGTYPEStimestamp_from_asc(char *, char **);
extern char *PGTYPEStimestamp_to_asc(Timestamp);
extern int PGTYPEStimestamp_sub (Timestamp *, Timestamp *, Interval *);
extern int PGTYPEStimestamp_fmt_asc (Timestamp *, char *, int, char *);
extern void PGTYPEStimestamp_current (Timestamp *);
extern int PGTYPEStimestamp_defmt_asc(char *, char *, Timestamp *);

#endif /* PGTYPES_TIMESTAMP */
