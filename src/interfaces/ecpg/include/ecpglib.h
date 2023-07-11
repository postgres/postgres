/*
 * Client-visible declarations for ecpglib
 *
 * src/interfaces/ecpg/include/ecpglib.h
 */

#ifndef _ECPGLIB_H
#define _ECPGLIB_H

#include <string.h>

#include "ecpg_config.h"
#include "ecpgtype.h"
#include "libpq-fe.h"
#include "sqlca.h"

/*
 * This is a small extract from c.h since we don't want to leak all postgres
 * definitions into ecpg programs; but we need to know what bool is.
 */
#ifndef __cplusplus

#ifdef PG_USE_STDBOOL
#include <stdbool.h>
#else

/*
 * We assume bool has been defined if true and false are.  This avoids
 * duplicate-typedef errors if this file is included after c.h.
 */
#if !(defined(true) && defined(false))
typedef unsigned char bool;
#endif

#ifndef true
#define true	((bool) 1)
#endif

#ifndef false
#define false	((bool) 0)
#endif

#endif							/* not PG_USE_STDBOOL */
#endif							/* not C++ */


#ifdef __cplusplus
extern "C"
{
#endif

void		ECPGdebug(int n, FILE *dbgs);
bool		ECPGstatus(int lineno, const char *connection_name);
bool		ECPGsetcommit(int lineno, const char *mode, const char *connection_name);
bool		ECPGsetconn(int lineno, const char *connection_name);
bool		ECPGconnect(int lineno, int c, const char *name, const char *user,
						const char *passwd, const char *connection_name, int autocommit);
bool		ECPGdo(const int lineno, const int compat, const int force_indicator,
				   const char *connection_name, const bool questionmarks,
				   const int st, const char *query,...);
bool		ECPGtrans(int lineno, const char *connection_name, const char *transaction);
bool		ECPGdisconnect(int lineno, const char *connection_name);
bool		ECPGprepare(int lineno, const char *connection_name, const bool questionmarks,
						const char *name, const char *variable);
bool		ECPGdeallocate(int lineno, int c, const char *connection_name, const char *name);
bool		ECPGdeallocate_all(int lineno, int compat, const char *connection_name);
char	   *ECPGprepared_statement(const char *connection_name, const char *name, int lineno);
PGconn	   *ECPGget_PGconn(const char *connection_name);
PGTransactionStatusType ECPGtransactionStatus(const char *connection_name);

 /* print an error message */
void		sqlprint(void);

/* define this for simplicity as well as compatibility */

#define		SQLCODE		sqlca.sqlcode
#define		SQLSTATE		sqlca.sqlstate

/* dynamic SQL */

bool		ECPGdo_descriptor(int line, const char *connection,
							  const char *descriptor, const char *query);
bool		ECPGdeallocate_desc(int line, const char *name);
bool		ECPGallocate_desc(int line, const char *name);
bool		ECPGget_desc_header(int lineno, const char *desc_name, int *count);
bool		ECPGget_desc(int lineno, const char *desc_name, int index,...);
bool		ECPGset_desc_header(int lineno, const char *desc_name, int count);
bool		ECPGset_desc(int lineno, const char *desc_name, int index,...);

void		ECPGset_noind_null(enum ECPGttype type, void *ptr);
bool		ECPGis_noind_null(enum ECPGttype type, const void *ptr);
bool		ECPGdescribe(int line, int compat, bool input,
						 const char *connection_name, const char *stmt_name,...);

void		ECPGset_var(int number, void *pointer, int lineno);
void	   *ECPGget_var(int number);

/* dynamic result allocation */
void		ECPGfree_auto_mem(void);

void		ecpg_pthreads_init(void);

#ifdef __cplusplus
}
#endif

#endif							/* _ECPGLIB_H */
