#include <postgres.h>
#include <libpq-fe.h>

#ifdef __cplusplus
extern		"C"
{
#endif

	void		ECPGdebug(int, FILE *);
	bool		ECPGstatus(int, const char *);
	bool		ECPGsetcommit(int, const char *, const char *);
	bool		ECPGsetconn(int, const char *);
	bool		ECPGconnect(int, const char *, const char *, const char *, const char *, int);
	bool		ECPGdo(int, const char *, char *,...);
	bool		ECPGtrans(int, const char *, const char *);
	bool		ECPGdisconnect(int, const char *);
	bool		ECPGprepare(int, char *, char *);
	bool		ECPGdeallocate(int, char *);
	bool		ECPGdeallocate_all(int);
	char		*ECPGprepared_statement(char *);
	
	void		ECPGlog(const char *format,...);
	
	/* print an error message */
	void		sqlprint(void);
	
#ifdef LIBPQ_FE_H
	bool		ECPGsetdb(PGconn *);
#endif

/* Here are some methods used by the lib. */
/* Returns a pointer to a string containing a simple type name. */
	const char *ECPGtype_name(enum ECPGttype);
	bool get_data(PGresult *, int, int, int, enum ECPGttype type,
			enum ECPGttype, void *, void *, long, long);
	char *ecpg_alloc(long, int);
	char *ecpg_strdup(const char *, int);

/* and some vars */
	extern struct auto_mem *auto_allocs;

/* define this for simplicity as well as compatibility */

#define		  SQLCODE	 sqlca.sqlcode

/* dynamic SQL */

	bool		ECPGdo_descriptor(int line,const char *connection,
							const char *descriptor,const char *query);
	bool		ECPGdeallocate_desc(int line,const char *name);
	bool		ECPGallocate_desc(int line,const char *name);
	void		ECPGraise(int line, int code, const char *str);
	bool		ECPGget_desc_header(int, char *, int *);
	bool		ECPGget_desc(int, char *, int, ...);
	

#ifdef __cplusplus
}

#endif

#include <ecpgerrno.h>
