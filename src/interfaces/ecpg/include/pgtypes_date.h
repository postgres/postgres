#ifndef PGTYPES_DATETIME
#define PGTYPES_DATETIME

#include <pgtypes_timestamp.h>

#define Date long

extern Date PGTYPESdate_from_asc(char *, char **);
extern char *PGTYPESdate_to_asc(Date);
extern Date PGTYPESdate_from_timestamp(Timestamp);
extern void PGTYPESdate_julmdy(Date, int *);
extern void PGTYPESdate_mdyjul(int *, Date *);
extern int	PGTYPESdate_dayofweek(Date);
extern void PGTYPESdate_today(Date *);
extern int	PGTYPESdate_defmt_asc(Date *, char *, char *);
extern int	PGTYPESdate_fmt_asc(Date, char *, char *);

#endif   /* PGTYPES_DATETIME */
