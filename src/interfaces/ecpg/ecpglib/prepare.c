/* $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/prepare.c,v 1.25 2007/11/15 22:25:17 momjian Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <ctype.h>

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

struct prepared_statement
{
	char	   *name;
	bool		prepared;
	struct statement *stmt;
	struct prepared_statement *next;
};

#define STMTID_SIZE 32

typedef struct
{
	int			lineno;
	char		stmtID[STMTID_SIZE];
	char	   *ecpgQuery;
	long		execs;			/* # of executions		*/
	char	   *connection;		/* connection for the statement		*/
} stmtCacheEntry;

static int	nextStmtID = 1;
static const int stmtCacheNBuckets = 2039;		/* # buckets - a prime # */
static const int stmtCacheEntPerBucket = 8;		/* # entries/bucket		*/
static stmtCacheEntry stmtCacheEntries[16384] = {{0, {0}, 0, 0, 0}};

static struct prepared_statement *find_prepared_statement(const char *name,
				 struct connection * con, struct prepared_statement ** prev);
static bool deallocate_one(int lineno, enum COMPAT_MODE c, struct connection * con,
		 struct prepared_statement * prev, struct prepared_statement * this);

static bool
isvarchar(unsigned char c)
{
	if (isalnum(c))
		return true;

	if (c == '_' || c == '>' || c == '-' || c == '.')
		return true;

	if (c >= 128)
		return true;

	return (false);
}

static bool
replace_variables(char **text, int lineno, bool questionmarks)
{
	bool		string = false;
	int			counter = 1,
				ptr = 0;

	for (; (*text)[ptr] != '\0'; ptr++)
	{
		if ((*text)[ptr] == '\'')
			string = string ? false : true;

		if (string || (((*text)[ptr] != ':') && ((*text)[ptr] != '?')))
			continue;

		if (((*text)[ptr] == ':') && ((*text)[ptr + 1] == ':'))
			ptr += 2;			/* skip  '::' */
		else
		{
			int			len;
			int			buffersize = sizeof(int) * CHAR_BIT * 10 / 3;	/* a rough guess of the
																		 * size we need */
			char	   *buffer,
					   *newcopy;

			if (!(buffer = (char *) ecpg_alloc(buffersize, lineno)))
				return false;

			snprintf(buffer, buffersize, "$%d", counter++);

			for (len = 1; (*text)[ptr + len] && isvarchar((*text)[ptr + len]); len++);
			if (!(newcopy = (char *) ecpg_alloc(strlen(*text) -len + strlen(buffer) + 1, lineno)))
			{
				ecpg_free(buffer);
				return false;
			}

			strncpy(newcopy, *text, ptr);
			strcpy(newcopy + ptr, buffer);
			strcat(newcopy, (*text) +ptr + len);

			ecpg_free(*text);
			ecpg_free(buffer);

			*text = newcopy;

			if ((*text)[ptr] == '\0')	/* we reached the end */
				ptr--;			/* since we will (*text)[ptr]++ in the top
								 * level for loop */
		}
	}
	return true;
}

/* handle the EXEC SQL PREPARE statement */
bool
ECPGprepare(int lineno, const char *connection_name, const int questionmarks, const char *name, const char *variable)
{
	struct connection *con;
	struct statement *stmt;
	struct prepared_statement *this,
			   *prev;
	struct sqlca_t *sqlca = ECPGget_sqlca();
	PGresult   *query;

	ecpg_init_sqlca(sqlca);

	con = ecpg_get_connection(connection_name);

	/* check if we already have prepared this statement */
	this = find_prepared_statement(name, con, &prev);
	if (this && !deallocate_one(lineno, ECPG_COMPAT_PGSQL, con, prev, this))
		return false;

	/* allocate new statement */
	this = (struct prepared_statement *) ecpg_alloc(sizeof(struct prepared_statement), lineno);
	if (!this)
		return false;

	stmt = (struct statement *) ecpg_alloc(sizeof(struct statement), lineno);
	if (!stmt)
	{
		ecpg_free(this);
		return false;
	}

	/* create statement */
	stmt->lineno = lineno;
	stmt->connection = con;
	stmt->command = ecpg_strdup(variable, lineno);
	stmt->inlist = stmt->outlist = NULL;

	/* if we have C variables in our statment replace them with '?' */
	replace_variables(&(stmt->command), lineno, questionmarks);

	/* add prepared statement to our list */
	this->name = (char *) name;
	this->stmt = stmt;

	/* and finally really prepare the statement */
	query = PQprepare(stmt->connection->connection, name, stmt->command, 0, NULL);
	if (!ecpg_check_PQresult(query, stmt->lineno, stmt->connection->connection, stmt->compat))
	{
		ecpg_free(stmt->command);
		ecpg_free(this);
		ecpg_free(stmt);
		return false;
	}

	ecpg_log("ECPGprepare line %d: NAME: %s QUERY: %s\n", stmt->lineno, name, stmt->command);
	PQclear(query);
	this->prepared = true;

	if (con->prep_stmts == NULL)
		this->next = NULL;
	else
		this->next = con->prep_stmts;

	con->prep_stmts = this;
	return true;
}

static struct prepared_statement *
find_prepared_statement(const char *name,
				 struct connection * con, struct prepared_statement ** prev_)
{
	struct prepared_statement *this,
			   *prev;

	for (this = con->prep_stmts, prev = NULL; this != NULL; prev = this, this = this->next)
	{
		if (strcmp(this->name, name) == 0)
		{
			if (prev_)
				*prev_ = prev;
			return this;
		}
	}
	return NULL;
}

static bool
deallocate_one(int lineno, enum COMPAT_MODE c, struct connection * con, struct prepared_statement * prev, struct prepared_statement * this)
{
	bool		r = false;

	ecpg_log("ECPGdeallocate line %d: NAME: %s\n", lineno, this->name);

	/* first deallocate the statement in the backend */
	if (this->prepared)
	{
		char	   *text;
		PGresult   *query;

		text = (char *) ecpg_alloc(strlen("deallocate \"\" ") + strlen(this->name), this->stmt->lineno);

		if (text)
		{
			sprintf(text, "deallocate \"%s\"", this->name);
			query = PQexec(this->stmt->connection->connection, text);
			ecpg_free(text);
			if (ecpg_check_PQresult(query, lineno, this->stmt->connection->connection, this->stmt->compat))
			{
				PQclear(query);
				r = true;
			}
		}
	}

	/*
	 * Just ignore all errors since we do not know the list of cursors we are
	 * allowed to free. We have to trust the software.
	 */
	if (!r && !INFORMIX_MODE(c))
	{
		ecpg_raise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, this->name);
		return false;
	}

	/* okay, free all the resources */
	ecpg_free(this->stmt->command);
	ecpg_free(this->stmt);
	if (prev != NULL)
		prev->next = this->next;
	else
		con->prep_stmts = this->next;

	ecpg_free(this);
	return true;
}

/* handle the EXEC SQL DEALLOCATE PREPARE statement */
bool
ECPGdeallocate(int lineno, int c, const char *connection_name, const char *name)
{
	struct connection *con;
	struct prepared_statement *this,
			   *prev;

	con = ecpg_get_connection(connection_name);

	this = find_prepared_statement(name, con, &prev);
	if (this)
		return deallocate_one(lineno, c, con, prev, this);

	/* prepared statement is not found */
	if (INFORMIX_MODE(c))
		return true;
	ecpg_raise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, name);
	return false;
}

bool
ecpg_deallocate_all_conn(int lineno, enum COMPAT_MODE c, struct connection * con)
{
	/* deallocate all prepared statements */
	while (con->prep_stmts)
	{
		if (!deallocate_one(lineno, c, con, NULL, con->prep_stmts))
			return false;
	}

	return true;
}

bool
ECPGdeallocate_all(int lineno, int compat, const char *connection_name)
{
	return ecpg_deallocate_all_conn(lineno, compat, ecpg_get_connection(connection_name));
}

char *
ecpg_prepared(const char *name, struct connection * con, int lineno)
{
	struct prepared_statement *this;

	this = find_prepared_statement(name, con, NULL);
	return this ? this->stmt->command : NULL;
}

/* return the prepared statement */
char *
ECPGprepared_statement(const char *connection_name, const char *name, int lineno)
{
	return ecpg_prepared(name, ecpg_get_connection(connection_name), lineno);
}

/*
 * hash a SQL statement -  returns entry # of first entry in the bucket
 */
static int
HashStmt(const char *ecpgQuery)
{
	int			stmtIx,
				bucketNo,
				hashLeng,
				stmtLeng;
	long long	hashVal,
				rotVal;

	stmtLeng = strlen(ecpgQuery);
	hashLeng = 50;				/* use 1st 50 characters of statement		*/
	if (hashLeng > stmtLeng)	/* if the statement isn't that long         */
		hashLeng = stmtLeng;	/* use its actual length			   */

	hashVal = 0;
	for (stmtIx = 0; stmtIx < hashLeng; ++stmtIx)
	{
		hashVal = hashVal + (int) ecpgQuery[stmtIx];
		hashVal = hashVal << 13;
		rotVal = (hashVal & 0x1fff00000000LL) >> 32;
		hashVal = (hashVal & 0xffffffffLL) | rotVal;
	}

	bucketNo = hashVal % stmtCacheNBuckets;
	bucketNo += 1;				/* don't use bucket # 0         */

	return (bucketNo * stmtCacheEntPerBucket);
}

/*
 * search the statement cache - search for entry with matching ECPG-format query
 * Returns entry # in cache if found
 *	 OR  zero if not present (zero'th entry isn't used)
 */
static int
SearchStmtCache(const char *ecpgQuery)
{
	int			entNo,
				entIx;

/* hash the statement			*/
	entNo = HashStmt(ecpgQuery);

/* search the cache		*/
	for (entIx = 0; entIx < stmtCacheEntPerBucket; ++entIx)
	{
		if (stmtCacheEntries[entNo].stmtID[0])	/* check if entry is in use		*/
		{
			if (!strcmp(ecpgQuery, stmtCacheEntries[entNo].ecpgQuery))
				break;			/* found it		*/
		}
		++entNo;				/* incr entry #		*/
	}

/* if entry wasn't found - set entry # to zero  */
	if (entIx >= stmtCacheEntPerBucket)
		entNo = 0;

	return (entNo);
}

/*
 * free an entry in the statement cache
 * Returns entry # in cache used
 *	 OR  negative error code
 */
static int
ecpg_freeStmtCacheEntry(int entNo)		/* entry # to free			*/
{
	stmtCacheEntry *entry;
	PGresult   *results;
	char		deallocText[100];
	struct connection *con;

	entry = &stmtCacheEntries[entNo];
	if (!entry->stmtID[0])		/* return if the entry isn't in use     */
		return (0);

	con = ecpg_get_connection(entry->connection);
/* free the server resources for the statement											*/
	ecpg_log("ecpg_freeStmtCacheEntry line %d: deallocate %s, cache entry #%d\n", entry->lineno, entry->stmtID, entNo);
	sprintf(deallocText, "DEALLOCATE PREPARE %s", entry->stmtID);
	results = PQexec(con->connection, deallocText);

	if (!ecpg_check_PQresult(results, entry->lineno, con->connection, ECPG_COMPAT_PGSQL))
		return (-1);
	PQclear(results);

	entry->stmtID[0] = '\0';

/* free the memory used by the cache entry		*/
	if (entry->ecpgQuery)
	{
		ecpg_free(entry->ecpgQuery);
		entry->ecpgQuery = 0;
	}

	return (entNo);
}

/*
 * add an entry to the statement cache
 * returns entry # in cache used  OR  negative error code
 */
static int
AddStmtToCache(int lineno,		/* line # of statement		*/
			   char *stmtID,	/* statement ID				*/
			   const char *connection,	/* connection				*/
			   const char *ecpgQuery)	/* query					*/
{
	int			ix,
				initEntNo,
				luEntNo,
				entNo;
	stmtCacheEntry *entry;

/* hash the statement																	*/
	initEntNo = HashStmt(ecpgQuery);

/* search for an unused entry															*/
	entNo = initEntNo;			/* start with the initial entry # for the
								 * bucket	 */
	luEntNo = initEntNo;		/* use it as the initial 'least used' entry			*/
	for (ix = 0; ix < stmtCacheEntPerBucket; ++ix)
	{
		entry = &stmtCacheEntries[entNo];
		if (!entry->stmtID[0])	/* unused entry  -	use it			*/
			break;
		if (entry->execs < stmtCacheEntries[luEntNo].execs)
			luEntNo = entNo;	/* save new 'least used' entry		*/
		++entNo;				/* increment entry #				*/
	}

/* if no unused entries were found - use the 'least used' entry found in the bucket		*/
	if (ix >= stmtCacheEntPerBucket)	/* if no unused entries were found	*/
		entNo = luEntNo;		/* re-use the 'least used' entry	*/

/* 'entNo' is the entry to use - make sure its free										*/
	if (ecpg_freeStmtCacheEntry(entNo) < 0)
		return (-1);

/* add the query to the entry															*/
	entry = &stmtCacheEntries[entNo];
	entry->lineno = lineno;
	entry->ecpgQuery = ecpg_strdup(ecpgQuery, lineno);
	entry->connection = (char *) connection;
	entry->execs = 0;
	memcpy(entry->stmtID, stmtID, sizeof(entry->stmtID));

	return (entNo);
}

/* handle cache and preparation of statments in auto-prepare mode */
bool
ecpg_auto_prepare(int lineno, const char *connection_name, const int questionmarks, char **name, const char *query)
{
	int			entNo;

	/* search the statement cache for this statement	*/
	entNo = SearchStmtCache(query);

	/* if not found - add the statement to the cache	*/
	if (entNo)
	{
		ecpg_log("ecpg_auto_prepare line %d: stmt found in cache, entry %d\n", lineno, entNo);
		*name = ecpg_strdup(stmtCacheEntries[entNo].stmtID, lineno);
	}
	else
	{
		ecpg_log("ecpg_auto_prepare line %d: stmt not in cache; inserting\n", lineno);

		/* generate a statement ID */
		*name = (char *) ecpg_alloc(STMTID_SIZE, lineno);
		sprintf(*name, "ecpg%d", nextStmtID++);

		if (!ECPGprepare(lineno, connection_name, questionmarks, ecpg_strdup(*name, lineno), query))
			return (false);
		if (AddStmtToCache(lineno, *name, connection_name, query) < 0)
			return (false);
	}

	/* increase usage counter */
	stmtCacheEntries[entNo].execs++;

	return (true);
}
