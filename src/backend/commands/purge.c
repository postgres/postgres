/*-------------------------------------------------------------------------
 *
 * purge.c--
 *    the POSTGRES purge command.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/Attic/purge.c,v 1.3 1996/11/06 08:21:36 scrappy Exp $
 *
 * Note:
 *	XXX There are many instances of int32 instead of ...Time.  These
 *	should be changed once it is decided the signed'ness will be.
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <access/heapam.h>
#include <access/xact.h>
#include <utils/tqual.h>	/* for NowTimeQual */
#include <catalog/catname.h>
#include <catalog/indexing.h>
#include <fmgr.h>
#include <commands/purge.h>
#include <utils/builtins.h>	/* for isreltime() */

static char	cmdname[] = "RelationPurge";

#define	RELATIVE	01
#define	ABSOLUTE	02

int32
RelationPurge(char *relationName,
	      char *absoluteTimeString,
	      char *relativeTimeString)
{
    register		i;
    AbsoluteTime		absoluteTime = INVALID_ABSTIME;
    RelativeTime		relativeTime = INVALID_RELTIME;
    bits8			dateTag;
    Relation		relation;
    HeapScanDesc		scan;
    static ScanKeyData	key[1] = {
	{ 0, Anum_pg_class_relname, F_NAMEEQ }
    };
    Buffer			buffer;
    HeapTuple		newTuple, oldTuple;
    AbsoluteTime		currentTime;
    char			*values[Natts_pg_class];
    char			nulls[Natts_pg_class];
    char			replace[Natts_pg_class];
    Relation		idescs[Num_pg_class_indices];
    
    /*
     * XXX for some reason getmyrelids (in inval.c) barfs when
     * you heap_replace tuples from these classes.  i thought
     * setheapoverride would fix it but it didn't.  for now,
     * just disallow purge on these classes.
     */
    if (strcmp(RelationRelationName, relationName) == 0 ||
	strcmp(AttributeRelationName, relationName)  == 0 ||
	strcmp(AccessMethodRelationName, relationName) == 0 ||
	strcmp(AccessMethodOperatorRelationName, relationName) == 0) {
	elog(WARN, "%s: cannot purge catalog \"%s\"",
	     cmdname, relationName);
    }
    
    if (PointerIsValid(absoluteTimeString)) {
	absoluteTime = (int32) nabstimein(absoluteTimeString);
	absoluteTimeString[0] = '\0';
	if (absoluteTime == INVALID_ABSTIME) {
	    elog(NOTICE, "%s: bad absolute time string \"%s\"",
		 cmdname, absoluteTimeString);
	    elog(WARN, "purge not executed");
	}
    }
    
#ifdef	PURGEDEBUG
    elog(DEBUG, "%s: absolute time `%s' is %d.",
	 cmdname, absoluteTimeString, absoluteTime);
#endif	/* defined(PURGEDEBUG) */
    
    if (PointerIsValid(relativeTimeString)) {
	if (isreltime(relativeTimeString, NULL, NULL, NULL) != 1) {
	    elog(WARN, "%s: bad relative time string \"%s\"",
		 cmdname, relativeTimeString);
	}
	relativeTime = reltimein(relativeTimeString);
	
#ifdef	PURGEDEBUG
	elog(DEBUG, "%s: relative time `%s' is %d.",
	     cmdname, relativeTimeString, relativeTime);
#endif	/* defined(PURGEDEBUG) */
    }
    
    /*
     * Find the RELATION relation tuple for the given relation.
     */
    relation = heap_openr(RelationRelationName);
    key[0].sk_argument = PointerGetDatum(relationName);
    fmgr_info(key[0].sk_procedure, &key[0].sk_func, &key[0].sk_nargs);
    
    scan = heap_beginscan(relation, 0, NowTimeQual, 1, key);
    oldTuple = heap_getnext(scan, 0, &buffer);
    if (!HeapTupleIsValid(oldTuple)) {
	heap_endscan(scan);
	heap_close(relation);
	elog(WARN, "%s: no such relation: %s", cmdname, relationName);
	return(0);
    }
    
    /*
     * Dig around in the tuple.
     */
    currentTime = GetCurrentTransactionStartTime();
    if (!RelativeTimeIsValid(relativeTime)) {
	dateTag = ABSOLUTE;
	if (!AbsoluteTimeIsValid(absoluteTime))
	    absoluteTime = currentTime;
    } else if (!AbsoluteTimeIsValid(absoluteTime))
	dateTag = RELATIVE;
    else
	dateTag = ABSOLUTE | RELATIVE;
    
    for (i = 0; i < Natts_pg_class; ++i) {
	nulls[i] = heap_attisnull(oldTuple, i+1) ? 'n' : ' ';
	values[i] = NULL;
	replace[i] = ' ';
    }
    if (dateTag & ABSOLUTE) {
	values[Anum_pg_class_relexpires-1] =
	    (char *) UInt32GetDatum(absoluteTime);
	replace[Anum_pg_class_relexpires-1] = 'r';
    }
    if (dateTag & RELATIVE) {
	values[Anum_pg_class_relpreserved-1] =
	    (char *) UInt32GetDatum(relativeTime);
	replace[Anum_pg_class_relpreserved-1] = 'r';
    }
    
    /*
     * Change the RELATION relation tuple for the given relation.
     */
    newTuple = heap_modifytuple(oldTuple, buffer, relation, (Datum*)values,
				nulls, replace);
    
    /* XXX How do you detect an insertion error?? */
    (void) heap_replace(relation, &newTuple->t_ctid, newTuple);
    
    /* keep the system catalog indices current */
    CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
    CatalogIndexInsert(idescs, Num_pg_class_indices, relation, newTuple);
    CatalogCloseIndices(Num_pg_class_indices, idescs);
    
    pfree(newTuple);
    
    heap_endscan(scan);
    heap_close(relation);
    return(1);
}

