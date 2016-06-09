/* src/interfaces/ecpg/ecpglib/extern.h */

#ifndef _ECPG_LIB_EXTERN_H
#define _ECPG_LIB_EXTERN_H

#include "postgres_fe.h"
#include "libpq-fe.h"
#include "sqlca.h"
#include "sqlda-native.h"
#include "sqlda-compat.h"
#include "ecpg_config.h"
#ifndef CHAR_BIT
#include <limits.h>
#endif

enum COMPAT_MODE
{
	ECPG_COMPAT_PGSQL = 0, ECPG_COMPAT_INFORMIX, ECPG_COMPAT_INFORMIX_SE
};

extern bool ecpg_internal_regression_mode;

#define INFORMIX_MODE(X) ((X) == ECPG_COMPAT_INFORMIX || (X) == ECPG_COMPAT_INFORMIX_SE)

enum ARRAY_TYPE
{
	ECPG_ARRAY_ERROR, ECPG_ARRAY_NOT_SET, ECPG_ARRAY_ARRAY, ECPG_ARRAY_VECTOR, ECPG_ARRAY_NONE
};

#define ECPG_IS_ARRAY(X) ((X) == ECPG_ARRAY_ARRAY || (X) == ECPG_ARRAY_VECTOR)

/* A generic varchar type. */
struct ECPGgeneric_varchar
{
	int			len;
	char		arr[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * type information cache
 */

struct ECPGtype_information_cache
{
	struct ECPGtype_information_cache *next;
	int			oid;
	enum ARRAY_TYPE isarray;
};

/* structure to store one statement */
struct statement
{
	int			lineno;
	char	   *command;
	char	   *name;
	struct connection *connection;
	enum COMPAT_MODE compat;
	bool		force_indicator;
	enum ECPG_statement_type statement_type;
	bool		questionmarks;
	struct variable *inlist;
	struct variable *outlist;
	char	   *oldlocale;
	int			nparams;
	char	  **paramvalues;
	PGresult   *results;
};

/* structure to store prepared statements for a connection */
struct prepared_statement
{
	char	   *name;
	bool		prepared;
	struct statement *stmt;
	struct prepared_statement *next;
};

/* structure to store connections */
struct connection
{
	char	   *name;
	PGconn	   *connection;
	bool		autocommit;
	struct ECPGtype_information_cache *cache_head;
	struct prepared_statement *prep_stmts;
	struct connection *next;
};

/* structure to store descriptors */
struct descriptor
{
	char	   *name;
	PGresult   *result;
	struct descriptor *next;
	int			count;
	struct descriptor_item *items;
};

struct descriptor_item
{
	int			num;
	char	   *data;
	int			indicator;
	int			length;
	int			precision;
	int			scale;
	int			type;
	struct descriptor_item *next;
};

struct variable
{
	enum ECPGttype type;
	void	   *value;
	void	   *pointer;
	long		varcharsize;
	long		arrsize;
	long		offset;
	enum ECPGttype ind_type;
	void	   *ind_value;
	void	   *ind_pointer;
	long		ind_varcharsize;
	long		ind_arrsize;
	long		ind_offset;
	struct variable *next;
};

struct var_list
{
	int			number;
	void	   *pointer;
	struct var_list *next;
};

extern struct var_list *ivlist;

/* Here are some methods used by the lib. */

bool		ecpg_add_mem(void *ptr, int lineno);

bool ecpg_get_data(const PGresult *, int, int, int, enum ECPGttype type,
			  enum ECPGttype, char *, char *, long, long, long,
			  enum ARRAY_TYPE, enum COMPAT_MODE, bool);

#ifdef ENABLE_THREAD_SAFETY
void		ecpg_pthreads_init(void);
#endif
struct connection *ecpg_get_connection(const char *);
char	   *ecpg_alloc(long, int);
char	   *ecpg_auto_alloc(long, int);
char	   *ecpg_realloc(void *, long, int);
void		ecpg_free(void *);
bool		ecpg_init(const struct connection *, const char *, const int);
char	   *ecpg_strdup(const char *, int);
const char *ecpg_type_name(enum ECPGttype);
int			ecpg_dynamic_type(Oid);
int			sqlda_dynamic_type(Oid, enum COMPAT_MODE);
void		ecpg_free_auto_mem(void);
void		ecpg_clear_auto_mem(void);

struct descriptor *ecpggetdescp(int, char *);

struct descriptor *ecpg_find_desc(int line, const char *name);

struct prepared_statement *ecpg_find_prepared_statement(const char *,
						  struct connection *, struct prepared_statement **);

bool ecpg_store_result(const PGresult *results, int act_field,
				  const struct statement * stmt, struct variable * var);
bool		ecpg_store_input(const int, const bool, const struct variable *, char **, bool);
void		ecpg_free_params(struct statement * stmt, bool print);
bool ecpg_do_prologue(int, const int, const int, const char *, const bool,
				 enum ECPG_statement_type, const char *, va_list,
				 struct statement **);
bool		ecpg_build_params(struct statement *);
bool		ecpg_autostart_transaction(struct statement * stmt);
bool		ecpg_execute(struct statement * stmt);
bool		ecpg_process_output(struct statement *, bool);
void		ecpg_do_epilogue(struct statement *);
bool ecpg_do(const int, const int, const int, const char *, const bool,
		const int, const char *, va_list);

bool		ecpg_check_PQresult(PGresult *, int, PGconn *, enum COMPAT_MODE);
void		ecpg_raise(int line, int code, const char *sqlstate, const char *str);
void		ecpg_raise_backend(int line, PGresult *result, PGconn *conn, int compat);
char	   *ecpg_prepared(const char *, struct connection *);
bool		ecpg_deallocate_all_conn(int lineno, enum COMPAT_MODE c, struct connection * conn);
void		ecpg_log(const char *format,...) pg_attribute_printf(1, 2);
bool		ecpg_auto_prepare(int, const char *, const int, char **, const char *);
void		ecpg_init_sqlca(struct sqlca_t * sqlca);

struct sqlda_compat *ecpg_build_compat_sqlda(int, PGresult *, int, enum COMPAT_MODE);
void		ecpg_set_compat_sqlda(int, struct sqlda_compat **, const PGresult *, int, enum COMPAT_MODE);
struct sqlda_struct *ecpg_build_native_sqlda(int, PGresult *, int, enum COMPAT_MODE);
void		ecpg_set_native_sqlda(int, struct sqlda_struct **, const PGresult *, int, enum COMPAT_MODE);

/* SQLSTATE values generated or processed by ecpglib (intentionally
 * not exported -- users should refer to the codes directly) */

#define ECPG_SQLSTATE_NO_DATA				"02000"
#define ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_PARAMETERS	"07001"
#define ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_TARGETS		"07002"
#define ECPG_SQLSTATE_RESTRICTED_DATA_TYPE_ATTRIBUTE_VIOLATION	"07006"
#define ECPG_SQLSTATE_INVALID_DESCRIPTOR_INDEX		"07009"
#define ECPG_SQLSTATE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION	"08001"
#define ECPG_SQLSTATE_CONNECTION_DOES_NOT_EXIST		"08003"
#define ECPG_SQLSTATE_TRANSACTION_RESOLUTION_UNKNOWN	"08007"
#define ECPG_SQLSTATE_CARDINALITY_VIOLATION "21000"
#define ECPG_SQLSTATE_NULL_VALUE_NO_INDICATOR_PARAMETER "22002"
#define ECPG_SQLSTATE_ACTIVE_SQL_TRANSACTION		"25001"
#define ECPG_SQLSTATE_NO_ACTIVE_SQL_TRANSACTION		"25P01"
#define ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME	"26000"
#define ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME	"33000"
#define ECPG_SQLSTATE_INVALID_CURSOR_NAME	"34000"
#define ECPG_SQLSTATE_SYNTAX_ERROR			"42601"
#define ECPG_SQLSTATE_DATATYPE_MISMATCH		"42804"
#define ECPG_SQLSTATE_DUPLICATE_CURSOR		"42P03"

/* implementation-defined internal errors of ecpg */
#define ECPG_SQLSTATE_ECPG_INTERNAL_ERROR	"YE000"
#define ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY	"YE001"

#endif   /* _ECPG_LIB_EXTERN_H */
