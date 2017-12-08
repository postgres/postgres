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

extern int	rdatestr(date, char *);
extern void rtoday(date *);
extern int	rjulmdy(date, short *);
extern int	rdefmtdate(date *, const char *, const char *);
extern int	rfmtdate(date, const char *, char *);
extern int	rmdyjul(short *, date *);
extern int	rstrdate(const char *, date *);
extern int	rdayofweek(date);

extern int	rfmtlong(long, const char *, char *);
extern int	rgetmsg(int, char *, int);
extern int	risnull(int, const char *);
extern int	rsetnull(int, char *);
extern int	rtypalign(int, int);
extern int	rtypmsize(int, int);
extern int	rtypwidth(int, int);
extern void rupshift(char *);

extern int	byleng(char *, int);
extern void ldchar(char *, int, char *);

extern void ECPG_informix_set_var(int, void *, int);
extern void *ECPG_informix_get_var(int);
extern void ECPG_informix_reset_sqlca(void);

/* Informix defines these in decimal.h */
int			decadd(decimal *, decimal *, decimal *);
int			deccmp(decimal *, decimal *);
void		deccopy(decimal *, decimal *);
int			deccvasc(const char *, int, decimal *);
int			deccvdbl(double, decimal *);
int			deccvint(int, decimal *);
int			deccvlong(long, decimal *);
int			decdiv(decimal *, decimal *, decimal *);
int			decmul(decimal *, decimal *, decimal *);
int			decsub(decimal *, decimal *, decimal *);
int			dectoasc(decimal *, char *, int, int);
int			dectodbl(decimal *, double *);
int			dectoint(decimal *, int *);
int			dectolong(decimal *, long *);

/* Informix defines these in datetime.h */
extern void dtcurrent(timestamp *);
extern int	dtcvasc(char *, timestamp *);
extern int	dtsub(timestamp *, timestamp *, interval *);
extern int	dttoasc(timestamp *, char *);
extern int	dttofmtasc(timestamp *, char *, int, char *);
extern int	intoasc(interval *, char *);
extern int	dtcvfmtasc(char *, char *, timestamp *);

#ifdef __cplusplus
}
#endif

#endif							/* ndef _ECPG_INFORMIX_H */
