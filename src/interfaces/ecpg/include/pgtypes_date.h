#ifndef PGTYPES_DATETIME
#define PGTYPES_DATETIME

#define Date long

extern Date PGTYPESdate_atod(char *, char **);
extern char *PGTYPESdate_dtoa(Date);
extern int PGTYPESdate_julmdy(Date, int*);
extern int PGTYPESdate_mdyjul(int*, Date *);
extern int PGTYPESdate_day(Date);

#endif /* PGTYPES_DATETIME */
