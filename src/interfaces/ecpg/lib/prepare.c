#include <ctype.h>

#include <ecpgtype.h>
#include <ecpglib.h>
#include <sqlca.h>

static struct prepared_statement
{
        char       *name;
        struct statement *stmt;
        struct prepared_statement *next;
}                  *prep_stmts = NULL;

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
			*ptr = '?';
			for (++ptr; *ptr && isvarchar(*ptr); ptr++)
				*ptr = ' ';
		}
	}
}
                        
/* handle the EXEC SQL PREPARE statement */
bool
ECPGprepare(int lineno, char *name, char *variable)
{
	struct statement *stmt;
	struct prepared_statement *this;

	/* check if we already have prepared this statement */
	for (this = prep_stmts; this != NULL && strcmp(this->name, name) != 0; this = this->next);
	if (this)
	{
		bool		b = ECPGdeallocate(lineno, name);

		if (!b)
			return false;
	}

	this = (struct prepared_statement *) ecpg_alloc(sizeof(struct prepared_statement), lineno);
	if (!this)
		return false;

	stmt = (struct statement *) ecpg_alloc(sizeof(struct statement), lineno);
	if (!stmt)
	{
		free(this);
		return false;
	}

	/* create statement */
	stmt->lineno = lineno;
	stmt->connection = NULL;
	stmt->command = ecpg_strdup(variable, lineno);
	stmt->inlist = stmt->outlist = NULL;

	/* if we have C variables in our statment replace them with '?' */
	replace_variables(stmt->command);

	/* add prepared statement to our list */
	this->name = ecpg_strdup(name, lineno);
	this->stmt = stmt;

	if (prep_stmts == NULL)
		this->next = NULL;
	else
		this->next = prep_stmts;

	prep_stmts = this;
	return true;
}

/* handle the EXEC SQL DEALLOCATE PREPARE statement */
bool
ECPGdeallocate(int lineno, char *name)
{
	struct prepared_statement *this,
			   *prev;

	/* check if we really have prepared this statement */
	for (this = prep_stmts, prev = NULL; this != NULL && strcmp(this->name, name) != 0; prev = this, this = this->next);
	if (this)
	{
		/* okay, free all the resources */
		free(this->name);
		free(this->stmt->command);
		free(this->stmt);
		if (prev != NULL)
			prev->next = this->next;
		else
			prep_stmts = this->next;
		
		free(this);
		return true;
	}
	ECPGlog("deallocate_prepare: invalid statement name %s\n", name);
	ECPGraise(lineno, ECPG_INVALID_STMT, name);
	return false;
}

bool
ECPGdeallocate_all(int lineno)
{
	/* deallocate all prepared statements */
	 while(prep_stmts != NULL)
	 {
		bool b = ECPGdeallocate(lineno, prep_stmts->name);
		
	        if (!b)
			return false;
         }
         
         return true;
}

/* return the prepared statement */
char *
ECPGprepared_statement(char *name)
{
	struct prepared_statement *this;

	for (this = prep_stmts; this != NULL && strcmp(this->name, name) != 0; this = this->next);
	return (this) ? this->stmt->command : NULL;
}
