/*-------------------------------------------------------------------------
 *
 * common.c
 *	  common routines between pg_dump and pg4_dump
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/pg_dump/common.c,v 1.50 2001/01/24 19:43:18 momjian Exp $
 *
 * Modifications - 6/12/96 - dave@bensoft.com - version 1.13.dhb.2
 *
 *	 - Fixed dumpTable output to output lengths for char and varchar types!
 *	 - Added single. quote to twin single quote expansion for 'insert' string
 *	   mode.
 *
 * Modifications 14-Sep-2000 - pjw@rhyme.com.au 
 *	-	Added enum for findTypeByOid to specify how to handle OID and which 
 *		string to return - formatted type, or base type. If the base type
 *		is returned then fmtId is called on the string.
 *
 *		BEWARE: Since fmtId uses a static buffer, using 'useBaseTypeName' on more
 *				than one call in a line will cause problems.
 *
 *-------------------------------------------------------------------------
 */


#include <ctype.h>

#include "postgres.h"

#include "libpq-fe.h"
#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#include "pg_dump.h"

static char **findParentsByOid(TableInfo *tbinfo, int numTables,
				 InhInfo *inhinfo, int numInherits,
				 const char *oid,
				 int *numParents);
static int	findTableByOid(TableInfo *tbinfo, int numTables, const char *oid);
static void flagInhAttrs(TableInfo *tbinfo, int numTables,
			 InhInfo *inhinfo, int numInherits);
static int	strInArray(const char *pattern, char **arr, int arr_size);

/*
 * findTypeByOid
 *	  given an oid of a type, return its typename
 *
 * if oid is "0", return "opaque" -- this is a special case
 *
 * NOTE:  should hash this, but just do linear search for now
 */

char *
findTypeByOid(TypeInfo *tinfo, int numTypes, const char *oid, OidOptions opts)
{
	int			i;

	if (strcmp(oid, "0") == 0) {

		if ( (opts & zeroAsOpaque) != 0 ) {

			return g_opaque_type;

		} else if ( (opts & zeroAsAny) != 0 ) {

			return "'any'";

		}
	}

	for (i = 0; i < numTypes; i++)
	{
		if (strcmp(tinfo[i].oid, oid) == 0) {
			if ( (opts & useBaseTypeName) != 0 ) {
				return (char*) fmtId(tinfo[i].typname, false);
			} else {
				return tinfo[i].typedefn;
			}
		}
	}

	/* should never get here */
	fprintf(stderr, "failed sanity check, type with oid %s was not found\n",
			oid);
	exit(2);
}

/*
 * findOprByOid
 *	  given the oid of an operator, return the name of the operator
 *
 *
 * NOTE:  should hash this, but just do linear search for now
 *
 */
char *
findOprByOid(OprInfo *oprinfo, int numOprs, const char *oid)
{
	int			i;

	for (i = 0; i < numOprs; i++)
	{
		if (strcmp(oprinfo[i].oid, oid) == 0)
			return oprinfo[i].oprname;
	}

	/* should never get here */
	fprintf(stderr, "failed sanity check, opr with oid %s was not found\n",
			oid);
	exit(2);
}


/*
 * findParentsByOid
 *	  given the oid of a class, return the names of its parent classes
 * and assign the number of parents to the last argument.
 *
 *
 * returns NULL if none
 */

static char **
findParentsByOid(TableInfo *tblinfo, int numTables,
				 InhInfo *inhinfo, int numInherits, const char *oid,
				 int *numParentsPtr)
{
	int			i,
				j;
	int			parentInd,
				selfInd;
	char	  **result;
	int			numParents;

	numParents = 0;
	for (i = 0; i < numInherits; i++)
	{
		if (strcmp(inhinfo[i].inhrelid, oid) == 0)
			numParents++;
	}

	*numParentsPtr = numParents;

	if (numParents > 0)
	{
		result = (char **) malloc(sizeof(char *) * numParents);
		j = 0;
		for (i = 0; i < numInherits; i++)
		{
			if (strcmp(inhinfo[i].inhrelid, oid) == 0)
			{
				parentInd = findTableByOid(tblinfo, numTables,
										   inhinfo[i].inhparent);
				if (parentInd < 0)
				{
					selfInd = findTableByOid(tblinfo, numTables, oid);
					fprintf(stderr,
							"failed sanity check, parent oid %s of table %s (oid %s) was not found\n",
							inhinfo[i].inhparent,
						  (selfInd >= 0) ? tblinfo[selfInd].relname : "",
							oid);
					exit(2);
				}
				result[j++] = tblinfo[parentInd].relname;
			}
		}
		return result;
	}
	else
		return NULL;
}

/*
 * parseNumericArray
 *	  parse a string of numbers delimited by spaces into a character array
 */

void
parseNumericArray(const char *str, char **array, int arraysize)
{
	int			j,
				argNum;
	char		temp[100];
	char		s;

	argNum = 0;
	j = 0;
	for (;;)
	{
		s = *str++;
		if (s == ' ' || s == '\0')
		{
			if (j > 0)
			{
				if (argNum >= arraysize)
				{
					fprintf(stderr, "parseNumericArray: too many numbers\n");
					exit(2);
				}
				temp[j] = '\0';
				array[argNum++] = strdup(temp);
				j = 0;
			}
			if (s == '\0')
				break;
		}
		else
		{
			if (!(isdigit((unsigned char) s) || s == '-') ||
				j >= sizeof(temp) - 1)
			{
				fprintf(stderr, "parseNumericArray: bogus number\n");
				exit(2);
			}
			temp[j++] = s;
		}
	}
	while (argNum < arraysize)
		array[argNum++] = strdup("0");
}


/*
 * strInArray:
 *	  takes in a string and a string array and the number of elements in the
 * string array.
 *	  returns the index if the string is somewhere in the array, -1 otherwise
 *
 */

static int
strInArray(const char *pattern, char **arr, int arr_size)
{
	int			i;

	for (i = 0; i < arr_size; i++)
	{
		if (strcmp(pattern, arr[i]) == 0)
			return i;
	}
	return -1;
}

/*
 * dumpSchema:
 *	  we have a valid connection, we are now going to dump the schema
 * into the file
 *
 */

TableInfo  *
dumpSchema(Archive  *fout,
		    int *numTablesPtr,
		    const char *tablename,
		    const bool aclsSkip,
		    const bool oids,
		    const bool schemaOnly,
		    const bool dataOnly)
{
	int			numTypes;
	int			numFuncs;
	int			numTables;
	int			numInherits;
	int			numAggregates;
	int			numOperators;
	int			numIndices;
	TypeInfo   *tinfo = NULL;
	FuncInfo   *finfo = NULL;
	AggInfo    *agginfo = NULL;
	TableInfo  *tblinfo = NULL;
	InhInfo    *inhinfo = NULL;
	OprInfo    *oprinfo = NULL;
	IndInfo    *indinfo = NULL;

	if (g_verbose)
		fprintf(stderr, "%s reading user-defined types %s\n",
				g_comment_start, g_comment_end);
	tinfo = getTypes(&numTypes);

	if (g_verbose)
		fprintf(stderr, "%s reading user-defined functions %s\n",
				g_comment_start, g_comment_end);
	finfo = getFuncs(&numFuncs);

	if (g_verbose)
		fprintf(stderr, "%s reading user-defined aggregates %s\n",
				g_comment_start, g_comment_end);
	agginfo = getAggregates(&numAggregates);

	if (g_verbose)
		fprintf(stderr, "%s reading user-defined operators %s\n",
				g_comment_start, g_comment_end);
	oprinfo = getOperators(&numOperators);

	if (g_verbose)
		fprintf(stderr, "%s reading user-defined tables %s\n",
				g_comment_start, g_comment_end);
	tblinfo = getTables(&numTables, finfo, numFuncs);

	if (g_verbose)
		fprintf(stderr, "%s reading indices information %s\n",
				g_comment_start, g_comment_end);
	indinfo = getIndices(&numIndices);

	if (g_verbose)
		fprintf(stderr, "%s reading table inheritance information %s\n",
				g_comment_start, g_comment_end);
	inhinfo = getInherits(&numInherits);

	if (g_verbose)
		fprintf(stderr, "%s finding the attribute names and types for each table %s\n",
				g_comment_start, g_comment_end);
	getTableAttrs(tblinfo, numTables);

	if (g_verbose)
		fprintf(stderr, "%s flagging inherited attributes in subtables %s\n",
				g_comment_start, g_comment_end);
	flagInhAttrs(tblinfo, numTables, inhinfo, numInherits);

	if (!tablename && !dataOnly)
	{
		if (g_verbose)
			fprintf(stderr, "%s dumping out database comment %s\n",
					g_comment_start, g_comment_end);
		dumpDBComment(fout);
	}

	if (!tablename && fout)
	{
		if (g_verbose)
			fprintf(stderr, "%s dumping out user-defined types %s\n",
					g_comment_start, g_comment_end);
		dumpTypes(fout, finfo, numFuncs, tinfo, numTypes);
	}

	if (g_verbose)
		fprintf(stderr, "%s dumping out tables %s\n",
				g_comment_start, g_comment_end);

	dumpTables(fout, tblinfo, numTables, indinfo, numIndices, inhinfo, numInherits,
			   tinfo, numTypes, tablename, aclsSkip, oids, schemaOnly, dataOnly);

	if (fout && !dataOnly)
	{
		if (g_verbose)
			fprintf(stderr, "%s dumping out indices %s\n",
					g_comment_start, g_comment_end);
		dumpIndices(fout, indinfo, numIndices, tblinfo, numTables, tablename);
	}

	if (!tablename && !dataOnly)
	{
		if (g_verbose)
			fprintf(stderr, "%s dumping out user-defined procedural languages %s\n",
					g_comment_start, g_comment_end);
		dumpProcLangs(fout, finfo, numFuncs, tinfo, numTypes);
	}

	if (!tablename && !dataOnly)
	{
		if (g_verbose)
			fprintf(stderr, "%s dumping out user-defined functions %s\n",
					g_comment_start, g_comment_end);
		dumpFuncs(fout, finfo, numFuncs, tinfo, numTypes);
	}

	if (!tablename && !dataOnly)
	{
		if (g_verbose)
			fprintf(stderr, "%s dumping out user-defined aggregates %s\n",
					g_comment_start, g_comment_end);
		dumpAggs(fout, agginfo, numAggregates, tinfo, numTypes);
	}

	if (!tablename && !dataOnly)
	{
		if (g_verbose)
			fprintf(stderr, "%s dumping out user-defined operators %s\n",
					g_comment_start, g_comment_end);
		dumpOprs(fout, oprinfo, numOperators, tinfo, numTypes);
	}

	*numTablesPtr = numTables;
	clearAggInfo(agginfo, numAggregates);
	clearOprInfo(oprinfo, numOperators);
	clearTypeInfo(tinfo, numTypes);
	clearFuncInfo(finfo, numFuncs);
	clearInhInfo(inhinfo, numInherits);
	clearIndInfo(indinfo, numIndices);
	return tblinfo;
}

/* flagInhAttrs -
 *	 for each table in tblinfo, flag its inherited attributes
 * so when we dump the table out, we don't dump out the inherited attributes
 *
 * initializes the parentRels field of each table
 *
 * modifies tblinfo
 *
 */
static void
flagInhAttrs(TableInfo *tblinfo, int numTables,
			 InhInfo *inhinfo, int numInherits)
{
	int			i,
				j,
				k;
	int			parentInd;

	/*
	 * we go backwards because the tables in tblinfo are in OID order,
	 * meaning the subtables are after the parent tables we flag inherited
	 * attributes from child tables first
	 */
	for (i = numTables - 1; i >= 0; i--)
	{
		tblinfo[i].parentRels = findParentsByOid(tblinfo, numTables,
												 inhinfo, numInherits,
												 tblinfo[i].oid,
												 &tblinfo[i].numParents);
		for (k = 0; k < tblinfo[i].numParents; k++)
		{
			parentInd = findTableByName(tblinfo, numTables,
										tblinfo[i].parentRels[k]);
			if (parentInd < 0)
			{
				/* shouldn't happen unless findParentsByOid is broken */
				fprintf(stderr, "failed sanity check, table %s not found by flagInhAttrs\n",
						tblinfo[i].parentRels[k]);
				exit(2);
			}
			for (j = 0; j < tblinfo[i].numatts; j++)
			{
				if (strInArray(tblinfo[i].attnames[j],
							   tblinfo[parentInd].attnames,
							   tblinfo[parentInd].numatts) != -1)
					tblinfo[i].inhAttrs[j] = 1;
			}
		}
	}
}


/*
 * findTableByName
 *	  finds the index (in tblinfo) of the table with the given relname
 *	returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */

int
findTableByName(TableInfo *tblinfo, int numTables, const char *relname)
{
	int			i;

	for (i = 0; i < numTables; i++)
	{
		if (strcmp(tblinfo[i].relname, relname) == 0)
			return i;
	}
	return -1;
}

/*
 * findTableByOid
 *	  finds the index (in tblinfo) of the table with the given oid
 *	returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */

static int
findTableByOid(TableInfo *tblinfo, int numTables, const char *oid)
{
	int			i;

	for (i = 0; i < numTables; i++)
	{
		if (strcmp(tblinfo[i].oid, oid) == 0)
			return i;
	}
	return -1;
}


/*
 * findFuncByName
 *	  finds the index (in finfo) of the function with the given name
 *	returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */

int
findFuncByName(FuncInfo *finfo, int numFuncs, const char *name)
{
	int			i;

	for (i = 0; i < numFuncs; i++)
	{
		if (strcmp(finfo[i].proname, name) == 0)
			return i;
	}
	return -1;
}

/*
 * fmtId
 *
 *	checks input string for non-lowercase characters
 *	returns pointer to input string or string surrounded by double quotes
 *
 *	Note that the returned string should be used immediately since it
 *	uses a static buffer to hold the string. Non-reentrant but faster?
 */
const char *
fmtId(const char *rawid, bool force_quotes)
{
	static PQExpBuffer id_return = NULL;
	const char *cp;

	if (!force_quotes)
	{
		/* do a quick check on the first character... */
		if (!islower((unsigned char) *rawid))
			force_quotes = true;
		/* otherwise check the entire string */
		else
			for (cp = rawid; *cp; cp++)
			{
				if (!(islower((unsigned char) *cp) ||
					  isdigit((unsigned char) *cp) ||
					  (*cp == '_')))
				{
					force_quotes = true;
					break;
				}
			}
	}

	if (!force_quotes)
		return rawid;			/* no quoting needed */

	if (id_return)
		resetPQExpBuffer(id_return);
	else
		id_return = createPQExpBuffer();

	appendPQExpBufferChar(id_return, '\"');
	for (cp = rawid; *cp; cp++)
	{
		/* Did we find a double-quote in the string?
		 * Then make this a double double-quote per SQL99.
		 * Before, we put in a backslash/double-quote pair.
		 * - thomas 2000-08-05 */
		if (*cp == '\"')
		{
			appendPQExpBufferChar(id_return, '\"');
			appendPQExpBufferChar(id_return, '\"');
		}
		appendPQExpBufferChar(id_return, *cp);
	}
	appendPQExpBufferChar(id_return, '\"');

	return id_return->data;
}	/* fmtId() */
