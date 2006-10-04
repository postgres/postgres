/* $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/prepare.c,v 1.18 2006/10/04 00:30:11 momjian Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <ctype.h>

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

static struct prepared_statement
{
	char	   *name;
	struct statement *stmt;
	struct prepared_statement *next;
}	*prep_stmts = NULL;

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

static void
replace_variables(char *text)
{
	char	   *ptr = text;
	bool		string = false;

	for (; *ptr != '\0'; ptr++)
	{
		if (*ptr == '\'')
			string = string ? false : true;

		if (!string && *ptr == ':')
		{
			if (ptr[1] == ':')
				ptr += 2;		/* skip  '::' */
			else
			{
				*ptr = '?';
				for (++ptr; *ptr && isvarchar(*ptr); ptr++)
					*ptr = ' ';
				if (*ptr == '\0')		/* we reached the end */
					ptr--;		/* since we will ptr++ in the top level for
								 * loop */
			}
		}
	}
}

/* handle the EXEC SQL PREPARE statement */
bool
ECPGprepare(int lineno, const char *name, const char *variable)
{
	struct statement *stmt;
	struct prepared_statement *this;
	struct sqlca_t *sqlca = ECPGget_sqlca();

	ECPGinit_sqlca(sqlca);
	/* check if we already have prepared this statement */
	for (this = prep_stmts; this != NULL && strcmp(this->name, name) != 0; this = this->next);
	if (this)
	{
		bool		b = ECPGdeallocate(lineno, ECPG_COMPAT_PGSQL, name);

		if (!b)
			return false;
	}

	this = (struct prepared_statement *) ECPGalloc(sizeof(struct prepared_statement), lineno);
	if (!this)
		return false;

	stmt = (struct statement *) ECPGalloc(sizeof(struct statement), lineno);
	if (!stmt)
	{
		ECPGfree(this);
		return false;
	}

	/* create statement */
	stmt->lineno = lineno;
	stmt->connection = NULL;
	stmt->command = ECPGstrdup(variable, lineno);
	stmt->inlist = stmt->outlist = NULL;

	/* if we have C variables in our statment replace them with '?' */
	replace_variables(stmt->command);

	/* add prepared statement to our list */
	this->name = ECPGstrdup(name, lineno);
	this->stmt = stmt;
	ECPGlog("ECPGprepare line %d: QUERY: %s\n", stmt->lineno, stmt->command);


	if (prep_stmts == NULL)
		this->next = NULL;
	else
		this->next = prep_stmts;

	prep_stmts = this;
	return true;
}

/* handle the EXEC SQL DEALLOCATE PREPARE statement */
bool
ECPGdeallocate(int lineno, int c, const char *name)
{
	bool		ret = ECPGdeallocate_one(lineno, name);
	enum COMPAT_MODE compat = c;

	if (INFORMIX_MODE(compat))
	{
		/*
		 * Just ignore all errors since we do not know the list of cursors we
		 * are allowed to free. We have to trust the software.
		 */
		return true;
	}

	if (!ret)
		ECPGraise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, name);

	return ret;
}

bool
ECPGdeallocate_one(int lineno, const char *name)
{
	struct prepared_statement *this,
			   *prev;

	/* check if we really have prepared this statement */
	for (this = prep_stmts, prev = NULL; this != NULL && strcmp(this->name, name) != 0; prev = this, this = this->next);
	if (this)
	{
		/* okay, free all the resources */
		ECPGfree(this->name);
		ECPGfree(this->stmt->command);
		ECPGfree(this->stmt);
		if (prev != NULL)
			prev->next = this->next;
		else
			prep_stmts = this->next;

		ECPGfree(this);
		return true;
	}
	return false;
}

bool
ECPGdeallocate_all(int lineno)
{
	/* deallocate all prepared statements */
	while (prep_stmts != NULL)
	{
		bool		b = ECPGdeallocate(lineno, ECPG_COMPAT_PGSQL, prep_stmts->name);

		if (!b)
			return false;
	}

	return true;
}

/* return the prepared statement */
char *
ECPGprepared_statement(const char *name)
{
	struct prepared_statement *this;

	for (this = prep_stmts; this != NULL && strcmp(this->name, name) != 0; this = this->next);
	return (this) ? this->stmt->command : NULL;
}
