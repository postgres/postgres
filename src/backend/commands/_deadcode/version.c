/*-------------------------------------------------------------------------
 *
 * version.c--
 *    This file contains all the rules that govern all version semantics.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *  The version stuff has not been tested under postgres95 and probably doesn't
 *  work! - jolly 8/19/95
 *  
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/_deadcode/Attic/version.c,v 1.4 1997/08/12 20:15:13 momjian Exp $
 *
 * NOTES
 *    At the point the version is defined, 2 physical relations are created
 *    <vname>_added and <vname>_deleted.
 *
 *    In addition, 4 rules are defined which govern the semantics of versions 
 *    w.r.t retrieves, appends, replaces and deletes.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include <postgres.h>

#include <access/heapam.h>
#include <utils/builtins.h>
#include <commands/version.h>
#include <access/xact.h>		/* for GetCurrentXactStartTime */
#include <tcop/tcopprot.h>

#define MAX_QUERY_LEN 1024

char rule_buf[MAX_QUERY_LEN];
static char attr_list[MAX_QUERY_LEN];

static void setAttrList(char *bname);

/*
 * problem: the version system assumes that the rules it declares will
 *          be fired in the order of declaration, it also assumes
 *          goh's silly instead semantics.  Unfortunately, it is a pain
 *          to make the version system work with the new semantics.
 *          However the whole problem can be solved, and some nice
 *          functionality can be achieved if we get multiple action rules
 *          to work.  So thats what I did                       -- glass
 *
 * Well, at least they've been working for about 20 minutes.
 * 
 * So any comments in this code about 1 rule per transction are false...:)
 *
 */

/*
 *  This is needed because the rule system only allows 
 *  *1* rule to be defined per transaction.
 *
 * NOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO
 * OOOOOOOOOOOOOOOOOOO!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *
 * DONT DO THAT!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *
 * If you commit the current Xact all the palloced memory GOES AWAY
 * and could be re-palloced in the new Xact and the whole hell breaks
 * loose and poor people like me spend 2 hours of their live chassing
 * a strange memory bug instead of watching the "Get Smart" marathon
 * in NICK !
 * DO NOT COMMIT THE XACT, just increase the Cid counter!
 *							_sp.
 */
static void
eval_as_new_xact(char *query)
{
    /* WARNING! do not uncomment the following lines WARNING!
     *  CommitTransactionCommand();
     * StartTransactionCommand();
     */
    CommandCounterIncrement();
    pg_eval(query, (char **) NULL, (Oid *) NULL, 0);
}

/*
 *  Define a version.
 */
void
DefineVersion(char *name, char *fromRelname, char *date)
{
    char *bname;
    static char saved_basename[512];
    static char saved_snapshot[512];

    if (date == NULL) {
	/* no time ranges */
	bname = fromRelname;
	strcpy(saved_basename, (char *) bname);
	*saved_snapshot = (char)NULL;
    } else {
	/* version is a snapshot */
	bname = fromRelname;
	strcpy(saved_basename, (char *) bname);
	sprintf(saved_snapshot, "['%s']", date);
    }
    
    
    /*
     * Calls the routine ``GetAttrList'' get the list of attributes
     * from the base relation. 
     * Code is put here so that we only need to look up the attribute once for
     * both appends and replaces.
     */
    setAttrList(bname);

    VersionCreate (name, saved_basename);
    VersionAppend (name, saved_basename);
    VersionDelete (name, saved_basename,saved_snapshot);
    VersionReplace (name, saved_basename,saved_snapshot);
    VersionRetrieve (name, saved_basename, saved_snapshot);
}


/*
 *  Creates the deltas.
 */
void
VersionCreate(char *vname, char *bname)
{
    static char query_buf [MAX_QUERY_LEN];
    
    /*
     *  Creating the dummy version relation for triggering rules.
     */
    sprintf(query_buf, "SELECT * INTO TABLE %s from %s where 1 =2",
	    vname, bname);
    
    pg_eval (query_buf, (char **) NULL, (Oid *) NULL, 0);  
    
    /* 
     * Creating the ``v_added'' relation 
     */
    sprintf (query_buf, "SELECT * INTO TABLE %s_added from %s where 1 = 2", 
	     vname, bname);
    eval_as_new_xact (query_buf); 
    
    /* 
     * Creating the ``v_deleted'' relation. 
     */
    sprintf (query_buf, "CREATE TABLE %s_del (DOID oid)", vname);
    eval_as_new_xact (query_buf); 
}


/*
 * Given the relation name, does a catalog lookup for that relation and
 * sets the global variable 'attr_list' with the list of attributes (names)
 * for that relation. 
 */
static void
setAttrList(char *bname)
{
    Relation rdesc;
    int i = 0;
    int maxattrs = 0;
    char *attrname;
    char temp_buf[512];
    int notfirst = 0;    

    rdesc = heap_openr(bname);
    if (rdesc == NULL ) {
	elog(WARN,"Unable to expand all -- amopenr failed ");
	return;
    }
    maxattrs = RelationGetNumberOfAttributes(rdesc);

    attr_list[0] = '\0';
    
    for ( i = maxattrs-1 ; i > -1 ; --i ) {
	attrname = (rdesc->rd_att->attrs[i]->attname).data;

	if (notfirst == 1) {
	    sprintf(temp_buf, ", %s = new.%s", attrname, attrname);
	} else {
	    sprintf(temp_buf, "%s = new.%s", attrname, attrname);
	    notfirst = 1;
	}
	strcat(attr_list, temp_buf);  
    }
    
    heap_close(rdesc);
    
    return;
}

/*
 * This routine defines the rule governing the append semantics of
 * versions.  All tuples appended to a version gets appended to the 
 * <vname>_added relation.
 */
void
VersionAppend(char *vname, char *bname)
{
    sprintf(rule_buf,
	    "define rewrite rule %s_append is on INSERT to %s do instead append %s_added(%s)",
	    vname, vname, vname, attr_list);
    
    eval_as_new_xact(rule_buf); 
}


/*
 * This routine defines the rule governing the retrieval semantics of
 * versions.  To retrieve tuples from a version , we need to:
 *
 *      1. Retrieve all tuples in the <vname>_added relation.
 *      2. Retrieve all tuples in the base relation which are not in 
 *         the <vname>_del relation.
 */
void
VersionRetrieve(char *vname, char *bname, char *snapshot)
{
    
    sprintf(rule_buf, 
	    "define rewrite rule %s_retrieve is on SELECT to %s do instead\n\
SELECT %s_1.oid, %s_1.* from _%s in %s%s, %s_1 in (%s_added | _%s) \
where _%s.oid !!= '%s_del.DOID'",
	    vname, vname, vname, vname, bname,
	    bname, snapshot,
	    vname, vname, bname, bname, vname);
    
    eval_as_new_xact(rule_buf); 
    
    /*  printf("%s\n",rule_buf); */
    
}

/*
 * This routine defines the rules that govern the delete semantics of 
 * versions. Two things happens when we delete a tuple from a version:
 *
 *     1. If the tuple to be deleted was added to the version *after*
 *        the version was created, then we simply delete the tuple 
 *        from the <vname>_added relation.
 *     2. If the tuple to be deleted is actually in the base relation,
 *        then we have to mark that tuple as being deleted by adding
 *        it to the <vname>_del relation.
 */
void
VersionDelete(char *vname, char *bname, char *snapshot)
{
    
    sprintf(rule_buf,
	    "define rewrite rule %s_delete1 is on delete to %s do instead\n \
[delete %s_added where current.oid = %s_added.oid\n \
 append %s_del(DOID = current.oid) from _%s in %s%s \
 where current.oid = _%s.oid] \n",
	  vname,vname,vname,vname,vname,
bname,bname,snapshot,bname); 

  eval_as_new_xact(rule_buf); 
#ifdef OLD_REWRITE
   sprintf(rule_buf,
         "define rewrite rule %s_delete2 is on delete to %s do instead \n \
    append %s_del(DOID = current.oid) from _%s in %s%s \
    where current.oid = _%s.oid \n",
         vname,vname,vname,bname,bname,snapshot,bname);

   eval_as_new_xact(rule_buf);
#endif /*  OLD_REWRITE */
}

/*
 *  This routine defines the rules that govern the update semantics
 *  of versions. To update a tuple in a version:
 *
 *      1. If the tuple is in <vname>_added, we simply ``replace''
 *         the tuple (as per postgres style).
 *      2. if the tuple is in the base relation, then two things have to
 *         happen:
 *         2.1  The tuple is marked ``deleted'' from the base relation by 
 *              adding the tuple to the <vname>_del relation. 
 *         2.2  A copy of the tuple is appended to the <vname>_added relation
 */
void
VersionReplace(char *vname, char *bname, char *snapshot)
{
    sprintf(rule_buf,
	    "define rewrite rule %s_replace1 is on replace to %s do instead \n\
[replace %s_added(%s) where current.oid = %s_added.oid \n\
 append %s_del(DOID = current.oid) from _%s in %s%s \
 where current.oid = _%s.oid\n\
 append %s_added(%s) from _%s in %s%s \
 where current.oid !!= '%s_added.oid' and current.oid = _%s.oid]\n",
	  vname,vname,vname,attr_list,vname,
          vname,bname,bname,snapshot,bname,
vname,attr_list,bname,bname,snapshot,vname,bname);

  eval_as_new_xact(rule_buf); 

/*  printf("%s\n",rule_buf); */
#ifdef OLD_REWRITE
  sprintf(rule_buf,
	  "define rewrite rule %s_replace2 is on replace to %s do \n\
    append %s_del(DOID = current.oid) from _%s in %s%s \
    where current.oid = _%s.oid\n",
	  vname,vname,vname,bname,bname,snapshot,bname);

  eval_as_new_xact(rule_buf); 

  sprintf(rule_buf,
	  "define rewrite rule %s_replace3 is on replace to %s do instead\n\
    append %s_added(%s) from _%s in %s%s \
    where current.oid !!= '%s_added.oid' and current.oid = \
    _%s.oid\n",
	  vname,vname, vname,attr_list,bname,bname,snapshot,vname,bname);

  eval_as_new_xact(rule_buf); 
#endif /* OLD_REWRITE */
/*  printf("%s\n",rule_buf); */

}

