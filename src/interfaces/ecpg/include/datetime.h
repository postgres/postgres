#include <pgtypes_timestamp.h>
#include <pgtypes_interval.h>

#ifndef dtime_t
#define dtime_t Timestamp
#endif   /* dtime_t */

#ifndef intrvl_t
#define intrvl_t Interval
#endif   /* intrvl_t */

extern void dtcurrent(dtime_t *);
extern int	dtcvasc(char *, dtime_t *);
extern int	dtsub(dtime_t *, dtime_t *, intrvl_t *);
extern int	dttoasc(dtime_t *, char *);
extern int	dttofmtasc(dtime_t *, char *, int, char *);
extern int	intoasc(intrvl_t *, char *);
extern int	dtcvfmtasc(char *, char *, dtime_t *);
