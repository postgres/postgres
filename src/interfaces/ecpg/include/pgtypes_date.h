/* src/interfaces/ecpg/include/pgtypes_date.h */

#ifndef PGTYPES_DATETIME
#define PGTYPES_DATETIME

#include <pgtypes.h>
#include <pgtypes_timestamp.h>

typedef long date;

#ifdef __cplusplus
extern "C"
{
#endif

extern date * PGTYPESdate_new(void);
extern void PGTYPESdate_free(date * d);
extern date PGTYPESdate_from_asc(char *str, char **endptr);
extern char *PGTYPESdate_to_asc(date dDate);
extern date PGTYPESdate_from_timestamp(timestamp dt);
extern void PGTYPESdate_julmdy(date jd, int *mdy);
extern void PGTYPESdate_mdyjul(int *mdy, date * jdate);
extern int	PGTYPESdate_dayofweek(date dDate);
extern void PGTYPESdate_today(date * d);
extern int	PGTYPESdate_defmt_asc(date * d, const char *fmt, const char *str);
extern int	PGTYPESdate_fmt_asc(date dDate, const char *fmtstring, char *outbuf);

#ifdef __cplusplus
}
#endif

#endif							/* PGTYPES_DATETIME */
