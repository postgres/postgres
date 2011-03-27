/*
 * this is a small part of c.h since we don't want to leak all postgres
 * definitions into ecpg programs
 * src/interfaces/ecpg/include/ecpglib.h
 */

#ifndef _ECPGLIB_H
#define _ECPGLIB_H

#include "libpq-fe.h"
#include "ecpgtype.h"
#include "sqlca.h"
#include <string.h>

#ifdef ENABLE_NLS
extern char *
ecpg_gettext(const char *msgid)
__attribute__((format_arg(1)));
#else
#define ecpg_gettext(x) (x)
#endif

#ifndef __cplusplus
#ifndef bool
#define bool char
#endif   /* ndef bool */

#ifndef true
#define true	((bool) 1)
#endif   /* ndef true */
#ifndef false
#define false	((bool) 0)
#endif   /* ndef false */
#endif   /* not C++ */

#ifndef TRUE
#define TRUE	1
#endif   /* TRUE */

#ifndef FALSE
#define FALSE	0
#endif   /* FALSE */

#ifdef __cplusplus
extern		"C"
{
#endif

void		ECPGdebug(int, FILE *);
bool		ECPGstatus(int, const char *);
bool		ECPGsetcommit(int, const char *, const char *);
bool		ECPGsetconn(int, const char *);
bool		ECPGconnect(int, int, const char *, const char *, const char *, const char *, int);
bool		ECPGdo(const int, const int, const int, const char *, const bool, const int, const char *,...);
bool		ECPGtrans(int, const char *, const char *);
bool		ECPGdisconnect(int, const char *);
bool		ECPGprepare(int, const char *, const bool, const char *, const char *);
bool		ECPGdeallocate(int, int, const char *, const char *);
bool		ECPGdeallocate_all(int, int, const char *);
char	   *ECPGprepared_statement(const char *, const char *, int);
PGconn	   *ECPGget_PGconn(const char *);
PGTransactionStatusType ECPGtransactionStatus(const char *);

char	   *ECPGerrmsg(void);

 /* print an error message */
void		sqlprint(void);

/* define this for simplicity as well as compatibility */

#define		SQLCODE		sqlca.sqlcode
#define		SQLSTATE		sqlca.sqlstate

/* dynamic SQL */

bool		ECPGdo_descriptor(int, const char *, const char *, const char *);
bool		ECPGdeallocate_desc(int, const char *);
bool		ECPGallocate_desc(int, const char *);
bool		ECPGget_desc_header(int, const char *, int *);
bool		ECPGget_desc(int, const char *, int,...);
bool		ECPGset_desc_header(int, const char *, int);
bool		ECPGset_desc(int, const char *, int,...);

void		ECPGset_noind_null(enum ECPGttype, void *);
bool		ECPGis_noind_null(enum ECPGttype, void *);
bool		ECPGdescribe(int, int, bool, const char *, const char *,...);

void		ECPGset_var(int, void *, int);
void	   *ECPGget_var(int number);

/* dynamic result allocation */
void		ECPGfree_auto_mem(void);

#ifdef ENABLE_THREAD_SAFETY
void		ecpg_pthreads_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif   /* _ECPGLIB_H */
