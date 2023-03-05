/* src/interfaces/ecpg/include/pgtypes_timestamp.h */

#ifndef PGTYPES_TIMESTAMP
#define PGTYPES_TIMESTAMP

#include <pgtypes.h>
/* pgtypes_interval.h includes ecpg_config.h */
#include <pgtypes_interval.h>

typedef int64 timestamp;
typedef int64 TimestampTz;

#ifdef __cplusplus
extern "C"
{
#endif

extern timestamp PGTYPEStimestamp_from_asc(char *str, char **endptr);
extern char *PGTYPEStimestamp_to_asc(timestamp tstamp);
extern int	PGTYPEStimestamp_sub(timestamp * ts1, timestamp * ts2, interval * iv);
extern int	PGTYPEStimestamp_fmt_asc(timestamp * ts, char *output, int str_len, const char *fmtstr);
extern void PGTYPEStimestamp_current(timestamp * ts);
extern int	PGTYPEStimestamp_defmt_asc(const char *str, const char *fmt, timestamp * d);
extern int	PGTYPEStimestamp_add_interval(timestamp * tin, interval * span, timestamp * tout);
extern int	PGTYPEStimestamp_sub_interval(timestamp * tin, interval * span, timestamp * tout);

#ifdef __cplusplus
}
#endif

#endif							/* PGTYPES_TIMESTAMP */
