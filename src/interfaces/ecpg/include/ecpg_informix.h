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

#define ECPG_INFORMIX_NUM_OVERFLOW	-1200
#define ECPG_INFORMIX_NUM_UNDERFLOW	-1201
#define ECPG_INFORMIX_DIVIDE_ZERO	-1202
#define ECPG_INFORMIX_BAD_YEAR		-1204
#define ECPG_INFORMIX_BAD_MONTH		-1205
#define ECPG_INFORMIX_BAD_DAY		-1206
#define ECPG_INFORMIX_ENOSHORTDATE	-1209
#define ECPG_INFORMIX_DATE_CONVERT	-1210
#define ECPG_INFORMIX_OUT_OF_MEMORY	-1211
#define ECPG_INFORMIX_ENOTDMY		-1212
#define ECPG_INFORMIX_BAD_NUMERIC	-1213
#define ECPG_INFORMIX_BAD_EXPONENT	-1216
#define ECPG_INFORMIX_BAD_DATE		-1218
#define ECPG_INFORMIX_EXTRA_CHARS	-1264

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
