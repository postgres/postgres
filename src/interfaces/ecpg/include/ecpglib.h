/*
 * this is a small part of c.h since we don't want to leak all postgres
 * definitions into ecpg programs
 */

#ifndef _ECPGLIB_H
#define _ECPGLIB_H

#include "libpq-fe.h"
#include "ecpgtype.h"

#ifndef __BEOS__
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
#else							/* __BEOS__ */
#include <SupportDefs.h>
#endif   /* __BEOS__ */

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

struct sqlca_t;

void		ECPGinit_sqlca(struct sqlca_t * sqlca);
void		ECPGdebug(int, FILE *);
bool		ECPGstatus(int, const char *);
bool		ECPGsetcommit(int, const char *, const char *);
bool		ECPGsetconn(int, const char *);
bool		ECPGconnect(int, int, const char *, const char *, const char *, const char *, int);
bool		ECPGdo(int, int, int, const char *, char *,...);
bool		ECPGtrans(int, const char *, const char *);
bool		ECPGdisconnect(int, const char *);
bool		ECPGprepare(int, char *, char *);
bool		ECPGdeallocate(int, int, char *);
bool		ECPGdeallocate_one(int, char *);
bool		ECPGdeallocate_all(int);
char	   *ECPGprepared_statement(const char *);

void		ECPGlog(const char *format,...);
char	   *ECPGerrmsg(void);

 /* print an error message */
void		sqlprint(void);

/* define this for simplicity as well as compatibility */

#define		  SQLCODE	 sqlca.sqlcode

/* dynamic SQL */

bool ECPGdo_descriptor(int line, const char *connection,
				  const char *descriptor, const char *query);
bool		ECPGdeallocate_desc(int line, const char *name);
bool		ECPGallocate_desc(int line, const char *name);
void		ECPGraise(int line, int code, const char *sqlstate, const char *str);
void		ECPGraise_backend(int line, PGresult *result, PGconn *conn, int compat);
bool		ECPGget_desc_header(int, char *, int *);
bool		ECPGget_desc(int, char *, int,...);

void		ECPGset_informix_null(enum ECPGttype, void *);
bool		ECPGis_informix_null(enum ECPGttype, void *);
bool		ECPGdescribe(int, bool, const char *,...);

/* dynamic result allocation */
void		ECPGfree_auto_mem(void);

#ifdef __cplusplus
}
#endif

#endif   /* _ECPGLIB_H */
