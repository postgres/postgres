#include "postgres_fe.h"
#include "libpq-fe.h"

/* Here are some methods used by the lib. */
/* Returns a pointer to a string containing a simple type name. */
void		free_auto_mem(void);
bool get_data(PGresult *, int, int, int, enum ECPGttype type,
		 enum ECPGttype, void *, void *, long, long, bool);
struct connection *get_connection(const char *);
void		init_sqlca(void);
char	   *ecpg_alloc(long, int);
bool		ecpg_init(const struct connection *, const char *, const int);
char	   *ecpg_strdup(const char *, int);
const char *ECPGtype_name(enum ECPGttype);
unsigned int ECPGDynamicType(Oid);

/* A generic varchar type. */
struct ECPGgeneric_varchar
{
	int			len;
	char		arr[1];
};

/*
 * type information cache
 */
 
struct ECPGtype_information_cache
{
	struct ECPGtype_information_cache	*next;
	int		oid;
	bool	isarray;
};

/* structure to store one statement */
struct statement
{
	int			lineno;
	char	   *command;
	struct connection *connection;
	struct variable *inlist;
	struct variable *outlist;
};

/* structure to store connections */
struct connection
{
	char	   *name;
	PGconn	   *connection;
	bool	    committed;
	int	    autocommit;
	struct ECPGtype_information_cache        *cache_head;
	struct connection *next;
};
