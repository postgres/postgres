#ifndef PGTYPES_DATETIME
#define PGTYPES_DATETIME

#include <pgtypes_timestamp.h>

typedef long date;

#ifdef __cplusplus
extern		"C"
{
#endif

extern date PGTYPESdate_from_asc(char *, char **);
extern char *PGTYPESdate_to_asc(date);
extern date PGTYPESdate_from_timestamp(timestamp);
extern void PGTYPESdate_julmdy(date, int *);
extern void PGTYPESdate_mdyjul(int *, date *);
extern int	PGTYPESdate_dayofweek(date);
extern void PGTYPESdate_today(date *);
extern int	PGTYPESdate_defmt_asc(date *, char *, char *);
extern int	PGTYPESdate_fmt_asc(date, char *, char *);

#ifdef __cplusplus
}
#endif

#endif   /* PGTYPES_DATETIME */
