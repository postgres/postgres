#ifndef PGTYPES_TIMESTAMP
#define PGTYPES_TIMESTAMP

#ifdef HAVE_INT64_TIMESTAMP
typedef int64 Timestamp;
typedef int64 TimestampTz;

#else
typedef double Timestamp;
typedef double TimestampTz;
#endif

extern Timestamp PGTYPEStimestamp_atot(char *, char **);
extern char *PGTYPEStimestamp_ttoa(Timestamp);

#endif /* PGTYPES_TIMESTAMP */
