/*
 * This file contains stuff needed to be as compatible to Informix as possible.
 * src/interfaces/ecpg/include/ecpg_informix.h
 */
#ifndef _ECPG_INFORMIX_H
#define _ECPG_INFORMIX_H

#include <ecpglib.h>
#include <pgtypes_date.h>
#include <pgtypes_interval.h>
#include <pgtypes_numeric.h>
#include <pgtypes_timestamp.h>

#define SQLNOTFOUND 100

#define ECPG_INFORMIX_NUM_OVERFLOW	-1200
#define ECPG_INFORMIX_NUM_UNDERFLOW -1201
#define ECPG_INFORMIX_DIVIDE_ZERO	-1202
#define ECPG_INFORMIX_BAD_YEAR		-1204
#define ECPG_INFORMIX_BAD_MONTH		-1205
#define ECPG_INFORMIX_BAD_DAY		-1206
#define ECPG_INFORMIX_ENOSHORTDATE	-1209
#define ECPG_INFORMIX_DATE_CONVERT	-1210
#define ECPG_INFORMIX_OUT_OF_MEMORY -1211
#define ECPG_INFORMIX_ENOTDMY		-1212
#define ECPG_INFORMIX_BAD_NUMERIC	-1213
#define ECPG_INFORMIX_BAD_EXPONENT	-1216
#define ECPG_INFORMIX_BAD_DATE		-1218
#define ECPG_INFORMIX_EXTRA_CHARS	-1264

#ifdef __cplusplus
extern "C"
{
#endif

extern int	rdatestr(date d, char *str);
extern void rtoday(date * d);
extern int	rjulmdy(date d, short *mdy);
extern int	rdefmtdate(date * d, const char *fmt, const char *str);
extern int	rfmtdate(date d, const char *fmt, char *str);
extern int	rmdyjul(short *mdy, date * d);
extern int	rstrdate(const char *str, date * d);
extern int	rdayofweek(date d);

extern int	rfmtlong(long lng_val, const char *fmt, char *outbuf);
extern int	rgetmsg(int msgnum, char *s, int maxsize);
extern int	risnull(int t, const char *ptr);
extern int	rsetnull(int t, char *ptr);
extern int	rtypalign(int offset, int type);
extern int	rtypmsize(int type, int len);
extern int	rtypwidth(int sqltype, int sqllen);
extern void rupshift(char *str);

extern int	byleng(char *str, int len);
extern void ldchar(char *src, int len, char *dest);

extern void ECPG_informix_set_var(int number, void *pointer, int lineno);
extern void *ECPG_informix_get_var(int number);
extern void ECPG_informix_reset_sqlca(void);

/* Informix defines these in decimal.h */
int			decadd(decimal *arg1, decimal *arg2, decimal *sum);
int			deccmp(decimal *arg1, decimal *arg2);
void		deccopy(decimal *src, decimal *target);
int			deccvasc(const char *cp, int len, decimal *np);
int			deccvdbl(double dbl, decimal *np);
int			deccvint(int in, decimal *np);
int			deccvlong(long lng, decimal *np);
int			decdiv(decimal *n1, decimal *n2, decimal *result);
int			decmul(decimal *n1, decimal *n2, decimal *result);
int			decsub(decimal *n1, decimal *n2, decimal *result);
int			dectoasc(decimal *np, char *cp, int len, int right);
int			dectodbl(decimal *np, double *dblp);
int			dectoint(decimal *np, int *ip);
int			dectolong(decimal *np, long *lngp);

/* Informix defines these in datetime.h */
extern void dtcurrent(timestamp * ts);
extern int	dtcvasc(char *str, timestamp * ts);
extern int	dtsub(timestamp * ts1, timestamp * ts2, interval * iv);
extern int	dttoasc(timestamp * ts, char *output);
extern int	dttofmtasc(timestamp * ts, char *output, int str_len, char *fmtstr);
extern int	intoasc(interval * i, char *str);
extern int	dtcvfmtasc(char *inbuf, char *fmtstr, timestamp * dtvalue);

#ifdef __cplusplus
}
#endif

#endif							/* ndef _ECPG_INFORMIX_H */
