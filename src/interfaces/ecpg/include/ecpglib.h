/*
 * this is a small part of c.h since we don't want to leak all postgres
 * definitions into ecpg programs
 */

#ifndef __BEOS__
#ifndef __cplusplus
#ifndef bool
#define bool char
#endif   /* ndef bool */
#endif   /* not C++ */

#ifndef true
#define true    ((bool) 1)
#endif
#ifndef false    
#define bool char
#endif   /* ndef bool */
#endif /* __BEOS__ */

#ifndef TRUE
#define TRUE    1
#endif   /* TRUE */

#ifndef FALSE
#define FALSE   0
#endif   /* FALSE */

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
	char	   *ECPGprepared_statement(char *);

	void		ECPGlog(const char *format,...);

	/* print an error message */
	void		sqlprint(void);

/* define this for simplicity as well as compatibility */

#define		  SQLCODE	 sqlca.sqlcode

/* dynamic SQL */

	bool		ECPGdo_descriptor(int line, const char *connection,
							  const char *descriptor, const char *query);
	bool		ECPGdeallocate_desc(int line, const char *name);
	bool		ECPGallocate_desc(int line, const char *name);
	void		ECPGraise(int line, int code, const char *str);
	bool		ECPGget_desc_header(int, char *, int *);
	bool		ECPGget_desc(int, char *, int,...);


#ifdef __cplusplus
}

#endif
