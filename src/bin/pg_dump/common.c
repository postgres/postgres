/*-------------------------------------------------------------------------
 *
 * common.c
 *	  common routines between pg_dump and pg4_dump
 *
 * Since pg4_dump is long-dead code, there is no longer any useful distinction
 * between this file and pg_dump.c.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/pg_dump/common.c,v 1.75 2003/08/04 02:40:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include "pg_dump.h"
#include "pg_backup_archiver.h"
#include "postgres.h"
#include "catalog/pg_class.h"

#include <ctype.h>

#include "libpq-fe.h"
#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

static void findParentsByOid(TableInfo *tblinfo, int numTables,
				 InhInfo *inhinfo, int numInherits,
				 const char *oid,
				 int *numParentsPtr, int **parentIndexes);
static void flagInhTables(TableInfo *tbinfo, int numTables,
			  InhInfo *inhinfo, int numInherits);
static void flagInhAttrs(TableInfo *tbinfo, int numTables,
			 InhInfo *inhinfo, int numInherits);
static int	strInArray(const char *pattern, char **arr, int arr_size);


/*
 * dumpSchema:
 *	  we have a valid connection, we are now going to dump the schema
 * into the file
 */

TableInfo *
dumpSchema(Archive *fout,
		   int *numTablesPtr,
		   const bool aclsSkip,
		   const bool schemaOnly,
		   const bool dataOnly)
{
	int			numNamespaces;
	int			numTypes;
	int			numFuncs;
	int			numTables;
	int			numInherits;
	int			numAggregates;
	int			numOperators;
	int			numOpclasses;
	NamespaceInfo *nsinfo;
	TypeInfo   *tinfo;
	FuncInfo   *finfo;
	AggInfo    *agginfo;
	TableInfo  *tblinfo;
	InhInfo    *inhinfo;
	OprInfo    *oprinfo;
	OpclassInfo *opcinfo;

	if (g_verbose)
		write_msg(NULL, "reading schemas\n");
	nsinfo = getNamespaces(&numNamespaces);

	if (g_verbose)
		write_msg(NULL, "reading user-defined types\n");
	tinfo = getTypes(&numTypes);

	if (g_verbose)
		write_msg(NULL, "reading user-defined functions\n");
	finfo = getFuncs(&numFuncs);

	if (g_verbose)
		write_msg(NULL, "reading user-defined aggregate functions\n");
	agginfo = getAggregates(&numAggregates);

	if (g_verbose)
		write_msg(NULL, "reading user-defined operators\n");
	oprinfo = getOperators(&numOperators);

	if (g_verbose)
		write_msg(NULL, "reading user-defined operator classes\n");
	opcinfo = getOpclasses(&numOpclasses);

	if (g_verbose)
		write_msg(NULL, "reading user-defined tables\n");
	tblinfo = getTables(&numTables);

	if (g_verbose)
		write_msg(NULL, "reading table inheritance information\n");
	inhinfo = getInherits(&numInherits);

	/* Link tables to parents, mark parents of target tables interesting */
	if (g_verbose)
		write_msg(NULL, "finding inheritance relationships\n");
	flagInhTables(tblinfo, numTables, inhinfo, numInherits);

	if (g_verbose)
		write_msg(NULL, "reading column info for interesting tables\n");
	getTableAttrs(tblinfo, numTables);

	if (g_verbose)
		write_msg(NULL, "flagging inherited columns in subtables\n");
	flagInhAttrs(tblinfo, numTables, inhinfo, numInherits);

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out database comment\n");
		dumpDBComment(fout);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined schemas\n");
		dumpNamespaces(fout, nsinfo, numNamespaces);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined types\n");
		dumpTypes(fout, finfo, numFuncs, tinfo, numTypes);
	}

	if (g_verbose)
		write_msg(NULL, "dumping out tables\n");
	dumpTables(fout, tblinfo, numTables,
			   aclsSkip, schemaOnly, dataOnly);

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out indexes\n");
		dumpIndexes(fout, tblinfo, numTables);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined procedural languages\n");
		dumpProcLangs(fout, finfo, numFuncs);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined functions\n");
		dumpFuncs(fout, finfo, numFuncs);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined aggregate functions\n");
		dumpAggs(fout, agginfo, numAggregates);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined operators\n");
		dumpOprs(fout, oprinfo, numOperators);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined operator classes\n");
		dumpOpclasses(fout, opcinfo, numOpclasses);
	}

	if (!dataOnly)
	{
		if (g_verbose)
			write_msg(NULL, "dumping out user-defined casts\n");
		dumpCasts(fout, finfo, numFuncs, tinfo, numTypes);
	}

	*numTablesPtr = numTables;
	return tblinfo;
}

/* flagInhTables -
 *	 Fill in parentIndexes fields of every target table, and mark
 *	 parents of target tables as interesting
 *
 * Note that only direct ancestors of targets are marked interesting.
 * This is sufficient; we don't much care whether they inherited their
 * attributes or not.
 *
 * modifies tblinfo
 */
static void
flagInhTables(TableInfo *tblinfo, int numTables,
			  InhInfo *inhinfo, int numInherits)
{
	int			i,
				j;
	int			numParents;
	int		   *parentIndexes;

	for (i = 0; i < numTables; i++)
	{
		/* Sequences and views never have parents */
		if (tblinfo[i].relkind == RELKIND_SEQUENCE ||
			tblinfo[i].relkind == RELKIND_VIEW)
			continue;

		/* Don't bother computing anything for non-target tables, either */
		if (!tblinfo[i].dump)
			continue;

		/* Find all the immediate parent tables */
		findParentsByOid(tblinfo, numTables,
						 inhinfo, numInherits,
						 tblinfo[i].oid,
						 &tblinfo[i].numParents,
						 &tblinfo[i].parentIndexes);
		numParents = tblinfo[i].numParents;
		parentIndexes = tblinfo[i].parentIndexes;

		/* Mark the parents as interesting for getTableAttrs */
		for (j = 0; j < numParents; j++)
		{
			int			parentInd = parentIndexes[j];

			tblinfo[parentInd].interesting = true;
		}
	}
}

/* flagInhAttrs -
 *	 for each dumpable table in tblinfo, flag its inherited attributes
 * so when we dump the table out, we don't dump out the inherited attributes
 *
 * modifies tblinfo
 */
static void
flagInhAttrs(TableInfo *tblinfo, int numTables,
			 InhInfo *inhinfo, int numInherits)
{
	int			i,
				j,
				k;
	int			parentInd;
	int			inhAttrInd;
	int			numParents;
	int		   *parentIndexes;
	bool		foundAttr;		/* Attr was found in a parent */
	bool		foundNotNull;	/* Attr was NOT NULL in a parent */
	bool		defaultsMatch;	/* All non-empty defaults match */
	bool		defaultsFound;	/* Found a default in a parent */
	char	   *attrDef;
	char	   *inhDef;

	for (i = 0; i < numTables; i++)
	{
		/* Sequences and views never have parents */
		if (tblinfo[i].relkind == RELKIND_SEQUENCE ||
			tblinfo[i].relkind == RELKIND_VIEW)
			continue;

		/* Don't bother computing anything for non-target tables, either */
		if (!tblinfo[i].dump)
			continue;

		numParents = tblinfo[i].numParents;
		parentIndexes = tblinfo[i].parentIndexes;

		if (numParents == 0)
			continue;			/* nothing to see here, move along */

		/*----------------------------------------------------------------
		 * For each attr, check the parent info: if no parent has an attr
		 * with the same name, then it's not inherited. If there *is* an
		 * attr with the same name, then only dump it if:
		 *
		 * - it is NOT NULL and zero parents are NOT NULL
		 *	 OR
		 * - it has a default value AND the default value does not match
		 *	 all parent default values, or no parents specify a default.
		 *
		 * See discussion on -hackers around 2-Apr-2001.
		 *----------------------------------------------------------------
		 */
		for (j = 0; j < tblinfo[i].numatts; j++)
		{
			foundAttr = false;
			foundNotNull = false;
			defaultsMatch = true;
			defaultsFound = false;

			attrDef = tblinfo[i].adef_expr[j];

			for (k = 0; k < numParents; k++)
			{
				parentInd = parentIndexes[k];
				inhAttrInd = strInArray(tblinfo[i].attnames[j],
										tblinfo[parentInd].attnames,
										tblinfo[parentInd].numatts);

				if (inhAttrInd != -1)
				{
					foundAttr = true;
					foundNotNull |= tblinfo[parentInd].notnull[inhAttrInd];
					if (attrDef != NULL)		/* If we have a default,
												 * check parent */
					{
						inhDef = tblinfo[parentInd].adef_expr[inhAttrInd];

						if (inhDef != NULL)
						{
							defaultsFound = true;
							defaultsMatch &= (strcmp(attrDef, inhDef) == 0);
						}
					}
				}
			}

			/*
			 * Based on the scan of the parents, decide if we can rely on
			 * the inherited attr
			 */
			if (foundAttr)		/* Attr was inherited */
			{
				/* Set inherited flag by default */
				tblinfo[i].inhAttrs[j] = true;
				tblinfo[i].inhAttrDef[j] = true;
				tblinfo[i].inhNotNull[j] = true;

				/*
				 * Clear it if attr had a default, but parents did not, or
				 * mismatch
				 */
				if ((attrDef != NULL) && (!defaultsFound || !defaultsMatch))
				{
					tblinfo[i].inhAttrs[j] = false;
					tblinfo[i].inhAttrDef[j] = false;
				}

				/*
				 * Clear it if NOT NULL and none of the parents were NOT
				 * NULL
				 */
				if (tblinfo[i].notnull[j] && !foundNotNull)
				{
					tblinfo[i].inhAttrs[j] = false;
					tblinfo[i].inhNotNull[j] = false;
				}

				/* Clear it if attr has local definition */
				if (g_fout->remoteVersion >= 70300 && tblinfo[i].attislocal[j])
					tblinfo[i].inhAttrs[j] = false;
			}
		}
	}
}


/*
 * findTableByOid
 *	  finds the index (in tblinfo) of the table with the given oid
 *	returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */
int
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
 * findFuncByOid
 *	  finds the index (in finfo) of the function with the given OID
 *	returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */
int
findFuncByOid(FuncInfo *finfo, int numFuncs, const char *oid)
{
	int			i;

	for (i = 0; i < numFuncs; i++)
	{
		if (strcmp(finfo[i].oid, oid) == 0)
			return i;
	}
	return -1;
}

/*
 * Finds the index (in tinfo) of the type with the given OID.  Returns
 * -1 if not found.
 */
int
findTypeByOid(TypeInfo *tinfo, int numTypes, const char *oid)
{
	int			i;

	for (i = 0; i < numTypes; i++)
	{
		if (strcmp(tinfo[i].oid, oid) == 0)
			return i;
	}
	return -1;
}

/*
 * findOprByOid
 *	  given the oid of an operator, return the name of the operator
 *
 * NOTE:  should hash this, but just do linear search for now
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
	write_msg(NULL, "failed sanity check, operator with OID %s not found\n", oid);

	/* no suitable operator name was found */
	return (NULL);
}


/*
 * findParentsByOid
 *	  given the oid of a class, find its parent classes in tblinfo[]
 *
 * Returns the number of parents and their array indexes into the
 * last two arguments.
 */

static void
findParentsByOid(TableInfo *tblinfo, int numTables,
				 InhInfo *inhinfo, int numInherits,
				 const char *oid,
				 int *numParentsPtr, int **parentIndexes)
{
	int			i,
				j;
	int			parentInd,
				selfInd;
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
		*parentIndexes = (int *) malloc(sizeof(int) * numParents);
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
					if (selfInd >= 0)
						write_msg(NULL, "failed sanity check, parent OID %s of table \"%s\" (OID %s) not found\n",
								  inhinfo[i].inhparent,
								  tblinfo[selfInd].relname,
								  oid);
					else
						write_msg(NULL, "failed sanity check, parent OID %s of table (OID %s) not found\n",
								  inhinfo[i].inhparent,
								  oid);

					exit_nicely();
				}
				(*parentIndexes)[j++] = parentInd;
			}
		}
	}
	else
		*parentIndexes = NULL;
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
					write_msg(NULL, "could not parse numeric array: too many numbers\n");
					exit_nicely();
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
				write_msg(NULL, "could not parse numeric array: invalid character in number\n");
				exit_nicely();
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
