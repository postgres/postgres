/*-------------------------------------------------------------------------
 *
 * common.c--
 *    common routines between pg_dump and pg4_dump
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/pg_dump/common.c,v 1.4 1996/08/24 20:49:17 scrappy Exp $
 *
 * Modifications - 6/12/96 - dave@bensoft.com - version 1.13.dhb.2
 *
 *   - Fixed dumpTable output to output lengths for char and varchar types!
 *   - Added single. quote to twin single quote expansion for 'insert' string
 *     mode.
 *
 *-------------------------------------------------------------------------
 */


#include <stdlib.h>
#include <stdio.h>
#include <sys/param.h>	/* for MAXHOSTNAMELEN on most */
#ifdef PORTNAME_sparc_solaris
#include <netdb.h>	/* for MAXHOSTNAMELEN on some */
#endif

#include "postgres.h"
#include "libpq-fe.h"

#include "pg_dump.h"

/* dupstr : copies a string, while allocating space for it. 
   the CALLER is responsible for freeing the space
   returns NULL if the argument is NULL*/
char* 
dupstr(char *s)
{
  char* result;

  if (s == NULL)
    return NULL;

  result = (char*)malloc(strlen(s)+1);
  strcpy(result, s);
  return result;
}

/*
 * findTypeByOid 
 *    given an oid of a type, return its typename
 *
 * if oid is "0", return "opaque" -- this is a special case
 *
 * NOTE:  should hash this, but just do linear search for now
 */

char*
findTypeByOid(TypeInfo* tinfo, int numTypes, char* oid)
{
    int i;

    if (strcmp(oid, "0") == 0) return g_opaque_type;

    for (i=0;i<numTypes;i++) {
	if (strcmp(tinfo[i].oid, oid) == 0)
	    return tinfo[i].typname;
    }

    /* should never get here */
    fprintf(stderr,"failed sanity check,  type with oid %s was not found\n",
	    oid);
    exit(2);
}

/*
 * findOprByOid
 *    given the oid of an operator, return the name of the operator
 *
 *
 * NOTE:  should hash this, but just do linear search for now
 * 
 */
char*
findOprByOid(OprInfo *oprinfo, int numOprs, char *oid)
{
    int i;
    for (i=0;i<numOprs;i++) {
	if (strcmp(oprinfo[i].oid, oid) == 0)
	    return oprinfo[i].oprname;
    }

    /* should never get here */
    fprintf(stderr,"failed sanity check,  opr with oid %s was not found\n",
	    oid);
    exit(2);
}


/*
 * findParentsByOid --
 *    given the oid of a class, return the names of its parent classes
 * and assign the number of parents to the last argument.
 *
 *
 * returns NULL if none
 */

char** 
findParentsByOid(TableInfo* tblinfo, int numTables,
		 InhInfo* inhinfo, int numInherits, char *oid,
		 int *numParentsPtr)
{
    int i,j;
    int parentInd;
    char** result;
    int numParents;

    numParents = 0;
    for (i=0;i<numInherits;i++) {
	if ( strcmp(inhinfo[i].inhrel, oid) == 0) {
	    numParents++;
	}
    }

    *numParentsPtr = numParents;

    if (numParents > 0) {
	result = (char**)malloc(sizeof(char*) * numParents);
	j = 0;
	for (i=0;i<numInherits;i++) {
	    if ( strcmp(inhinfo[i].inhrel, oid) == 0) {
		parentInd = findTableByOid(tblinfo, numTables, 
					   inhinfo[i].inhparent);
		result[j++] = tblinfo[parentInd].relname;
	    }
	}
	return result;
    }
    else 
	return NULL;
}

/*
 * parseArgTypes
 *    parse a string of eight numbers delimited by spaces
 * into a character array
 */

void 
parseArgTypes(char **argtypes, char* str)
{
    int j, argNum;
    char temp[100];
    char s;

    argNum = 0;
    j = 0;
    while ( (s = *str) != '\0') {
	if (s == ' ') {
	    temp[j] = '\0';
	    argtypes[argNum] = dupstr(temp);
	    argNum++;
	    j = 0;
	} else {
	    temp[j] = s;
	    j++;
	}
	str++;
    }
    if (j != 0)  {
	temp[j] = '\0';
        argtypes[argNum] = dupstr(temp);
    }
    
}


/*
 * strInArray:
 *    takes in a string and a string array and the number of elements in the 
 * string array.  
 *    returns the index if the string is somewhere in the array, -1 otherwise
 *
 */

int 
strInArray(char* pattern, char** arr, int arr_size)
{
    int i;
    for (i=0;i<arr_size;i++) {
	if (strcmp(pattern, arr[i]) == 0) 
	    return i;
    }
    return -1;
}

/*
 * dumpSchema:
 *    we have a valid connection, we are now going to dump the schema
 * into the file
 *
 */

TableInfo *
dumpSchema(FILE *fout, int *numTablesPtr, char *tablename)
{
    int numTypes;
    int numFuncs;
    int numTables;
    int numInherits;
    int numAggregates;
    int numOperators;
    TypeInfo *tinfo;
    FuncInfo *finfo;
    AggInfo *agginfo;
    TableInfo *tblinfo;
    InhInfo *inhinfo;
    OprInfo *oprinfo;

if (g_verbose) fprintf(stderr,"%s reading user-defined types %s\n",
		       g_comment_start, g_comment_end);
    tinfo = getTypes(&numTypes);

if (g_verbose) fprintf(stderr,"%s reading user-defined functions %s\n",
		       g_comment_start, g_comment_end);
    finfo = getFuncs(&numFuncs);

if (g_verbose) fprintf(stderr,"%s reading user-defined aggregates %s\n",
		       g_comment_start, g_comment_end);
    agginfo = getAggregates(&numAggregates);

if (g_verbose) fprintf(stderr,"%s reading user-defined operators %s\n",
		       g_comment_start, g_comment_end);
    oprinfo = getOperators(&numOperators);

if (g_verbose) fprintf(stderr,"%s reading user-defined tables %s\n",
		       g_comment_start, g_comment_end);
    tblinfo = getTables(&numTables);

if (g_verbose) fprintf(stderr,"%s reading table inheritance information %s\n",
		       g_comment_start, g_comment_end);
    inhinfo = getInherits(&numInherits);

if (g_verbose) fprintf(stderr, "%s finding the attribute names and types for each table %s\n",
		       g_comment_start, g_comment_end);
    getTableAttrs(tblinfo, numTables);

if (g_verbose) fprintf(stderr, "%s flagging inherited attributes in subtables %s\n",
		       g_comment_start, g_comment_end);
    flagInhAttrs(tblinfo, numTables, inhinfo, numInherits);

if (!tablename && fout) {
  if (g_verbose) fprintf(stderr,"%s dumping out user-defined types %s\n",
  		       g_comment_start, g_comment_end);
      dumpTypes(fout, finfo, numFuncs, tinfo, numTypes);
}    

if (fout) {
  if (g_verbose) fprintf(stderr,"%s dumping out tables %s\n",
		       g_comment_start, g_comment_end);
    dumpTables(fout, tblinfo, numTables, inhinfo, numInherits,
	       tinfo, numTypes, tablename);
}	       

if (!tablename && fout) {
  if (g_verbose) fprintf(stderr,"%s dumping out user-defined functions %s\n",
  		       g_comment_start, g_comment_end);
      dumpFuncs(fout, finfo, numFuncs, tinfo, numTypes);
}

if (!tablename && fout) {
  if (g_verbose) fprintf(stderr,"%s dumping out user-defined functions %s\n",
  		       g_comment_start, g_comment_end);
      dumpAggs(fout, agginfo, numAggregates, tinfo, numTypes);
}      

if (!tablename && fout) {
  if (g_verbose) fprintf(stderr,"%s dumping out user-defined operators %s\n",
  		       g_comment_start, g_comment_end);
      dumpOprs(fout, oprinfo, numOperators, tinfo, numTypes);
}

    *numTablesPtr = numTables;
    return tblinfo;
}


/*
 * dumpSchemaIdx:
 *    dump indexes at the end for performance
 *
 */

extern void
dumpSchemaIdx(FILE *fout, int *numTablesPtr, char *tablename,
					TableInfo* tblinfo, int numTables)
{
    int numIndices;
    IndInfo *indinfo;
    
    if (g_verbose) fprintf(stderr,"%s reading indices information %s\n",
		       g_comment_start, g_comment_end);
    indinfo = getIndices(&numIndices);

    if (fout) {
  	if (g_verbose) fprintf(stderr,"%s dumping out indices %s\n",
  		g_comment_start, g_comment_end);
	dumpIndices(fout, indinfo, numIndices, tblinfo, numTables, tablename);
    }
}

/* flagInhAttrs -
 *   for each table in tblinfo, flag its inherited attributes
 * so when we dump the table out, we don't dump out the inherited attributes
 *   
 * initializes the parentRels field of each table
 *
 * modifies tblinfo
 *
 */
void
flagInhAttrs(TableInfo* tblinfo, int numTables,
	     InhInfo* inhinfo, int numInherits)
{
    int i,j,k;
    int parentInd;

    /* we go backwards because the tables in tblinfo are in OID
       order, meaning the subtables are after the parent tables
       we flag inherited attributes from child tables first */
    for (i = numTables-1; i >= 0; i--) {
	tblinfo[i].parentRels = findParentsByOid(tblinfo, numTables,
						inhinfo, numInherits,
						tblinfo[i].oid,
						&tblinfo[i].numParents);
	for (k=0;k<tblinfo[i].numParents;k++) {
	    parentInd = findTableByName(tblinfo, numTables, 
					tblinfo[i].parentRels[k]);
	    for (j=0;j<tblinfo[i].numatts;j++) {
		if (strInArray(tblinfo[i].attnames[j],
			       tblinfo[parentInd].attnames,
			       tblinfo[parentInd].numatts) != -1) {
		    tblinfo[i].inhAttrs[j] = 1;
		}
	    }
	}
    }
}


/*
 * findTableByName
 *    finds the index (in tblinfo) of the table with the given relname
 *  returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */

int
findTableByName(TableInfo* tblinfo, int numTables, char* relname)
{
    int i;
    for (i=0;i<numTables;i++) {
	if  (strcmp(tblinfo[i].relname, relname) == 0)
	    return i;
    }
    return -1;
}

/*
 * findTableByOid
 *    finds the index (in tblinfo) of the table with the given oid
 *  returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */

int
findTableByOid(TableInfo* tblinfo, int numTables, char* oid)
{
    int i;
    for (i=0;i<numTables;i++) {
	if  (strcmp(tblinfo[i].oid, oid) == 0)
	    return i;
    }
    return -1;
}


/*
 * findFuncByName
 *    finds the index (in finfo) of the function with the given name
 *  returns -1 if not found
 *
 * NOTE:  should hash this, but just do linear search for now
 */

int
findFuncByName(FuncInfo* finfo, int numFuncs, char* name)
{
    int i;
    for (i=0;i<numFuncs;i++) {
	if  (strcmp(finfo[i].proname, name) == 0)
	    return i;
    }
    return -1;
}

/*
 * isArchiveName
 *
 *   returns true if the relation name is an archive name, false otherwise
 */ 
int
isArchiveName(char* relname)
{
    return (strlen(relname) > 1 && relname[1] == ',');
}






