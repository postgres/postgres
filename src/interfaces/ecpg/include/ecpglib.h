#include <c.h>

#ifdef __cplusplus
extern "C" {
#endif

void		ECPGdebug(int, FILE *);
bool		ECPGconnect(const char *dbname);
bool		ECPGdo(int, char *,...);
bool		ECPGtrans(int, const char *);
bool		ECPGfinish(void);
bool		ECPGstatus(void);

void		ECPGlog(const char *format,...);

/* These functions are only kept for compatibility reasons. */
/* Use ECPGtrans instead. */
bool		ECPGcommit(int);
bool		ECPGrollback(int);

#ifdef LIBPQ_FE_H
bool		ECPGsetdb(PGconn *);

#endif

/* Here are some methods used by the lib. */
/* Returns a pointer to a string containing a simple type name. */
const char *ECPGtype_name(enum ECPGttype);

/* A generic varchar type. */
struct ECPGgeneric_varchar
{
	int			len;
	char		arr[1];
};

/* print an error message */
void		sqlprint(void);

/* define this for simplicity as well as compatibility */

#define		  SQLCODE	 sqlca.sqlcode

#ifdef __cplusplus
}
#endif
