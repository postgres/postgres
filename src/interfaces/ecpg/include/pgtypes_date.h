#ifndef PGTYPES_DATETIME
#define PGTYPES_DATETIME

#include <pgtypes_timestamp.h>

#define Date long

extern Date PGTYPESdate_atod(char *, char **);
extern char *PGTYPESdate_dtoa(Date);
extern Date PGTYPESdate_ttod(Timestamp);
extern void PGTYPESdate_julmdy(Date, int*);
extern void PGTYPESdate_mdyjul(int*, Date *);
extern int PGTYPESdate_dayofweek(Date);
extern void PGTYPESdate_today (Date *);
extern int PGTYPESdate_defmtdate(Date *, char *, char *);
extern int PGTYPESdate_fmtdate(Date, char *, char *);
#endif /* PGTYPES_DATETIME */
