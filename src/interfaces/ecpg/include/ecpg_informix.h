#ifndef _ECPG_INFORMIX_H
#define _ECPG_INFORMIX_H
/*
 * This file contains stuff needed to be as compatible to Informix as possible.
 */

#include <decimal.h>
#include <datetime.h>
#include <ecpglib.h>
#include <pgtypes_date.h>

#define SQLNOTFOUND 100

extern int	rdatestr(date, char *);
extern void 	rtoday(date *);
extern int	rjulmdy(date, short *);
extern int	rdefmtdate(date *, char *, char *);
extern int	rfmtdate(date, char *, char *);
extern int	rmdyjul(short *, date *);
extern int	rstrdate(char *, date *);
extern int	rdayofweek(date);

extern int	rfmtlong(long, char *, char *);
extern int	rgetmsg(int, char *, int);
extern int	risnull(int, char *);
extern int	rsetnull(int, char *);
extern int	rtypalign(int, int);
extern int	rtypmsize(int, int);
extern int	rtypwidth(int, int);
extern void 	rupshift(char *);

extern int	byleng(char *, int);
extern void ldchar(char *, int, char *);

extern void ECPG_informix_set_var(int, void *, int);
extern void *ECPG_informix_get_var(int);

#endif /* ndef _ECPG_INFORMIX_H */
