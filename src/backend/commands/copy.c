/*-------------------------------------------------------------------------
 *
 * copy.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/copy.c,v 1.2.2.1 1996/08/24 20:53:39 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <sys/types.h>	/* for mode_t */
#include <sys/stat.h>	/* for umask(2) prototype */

#include "postgres.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "catalog/pg_type.h"
#include "catalog/pg_index.h"
#include "catalog/index.h"

#include "storage/bufmgr.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/funcindex.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "executor/tuptable.h"
#include "executor/executor.h"
#include "utils/rel.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "fmgr.h"
#include "machine.h"

/*
 * New copy code.
 * 
 * This code "knows" the following about tuples:
 * 
 */

static bool reading_from_input = false;

/* non-export function prototypes */
static void CopyTo(Relation rel, bool binary, bool oids, FILE *fp, char *delim);
static void CopyFrom(Relation rel, bool binary, bool oids, FILE *fp, char *delim);
static Oid GetOutputFunction(Oid type);
static Oid GetTypeElement(Oid type);
static Oid GetInputFunction(Oid type);
static Oid IsTypeByVal(Oid type);
static void GetIndexRelations(Oid main_relation_oid,
			      int *n_indices,
			      Relation **index_rels);
static char *CopyReadAttribute(FILE *fp, bool *isnull, char *delim);
static void CopyAttributeOut(FILE *fp, char *string, char *delim);
static int CountTuples(Relation relation);

extern FILE *Pfout, *Pfin;

void
DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe, char *filename, 
       char *delim)
{
    FILE *fp;
    Relation rel;
    reading_from_input = pipe;
    
    rel = heap_openr(relname);
    if (rel == NULL) elog(WARN, "Copy: class %s does not exist.", relname);
    
    if (from) {
	if (pipe && IsUnderPostmaster) ReceiveCopyBegin();
	if (IsUnderPostmaster) {
	    fp = pipe ? Pfin : fopen(filename, "r");
	}else {
	    fp = pipe ? stdin : fopen(filename, "r");
	}
	if (fp == NULL) {
	    elog(WARN, "COPY: file %s could not be open for reading", filename);
	}
	CopyFrom(rel, binary, oids, fp, delim);
    }else {
	
	mode_t oumask = umask((mode_t) 0);
	
	if (pipe && IsUnderPostmaster) SendCopyBegin();
	if (IsUnderPostmaster) {
	    fp = pipe ? Pfout : fopen(filename, "w");
	    
	}else {
	    fp = pipe ? stdout : fopen(filename, "w");
	}
	(void) umask(oumask);
	if (fp == NULL)  {
	    elog(WARN, "COPY: file %s could not be open for writing", filename);
	}
	CopyTo(rel, binary, oids, fp, delim);
    }
    if (!pipe) {
	fclose(fp);
    }else if (!from && !binary) {
	fputs(".\n", fp);
	if (IsUnderPostmaster) fflush(Pfout);
    }
}

static void
CopyTo(Relation rel, bool binary, bool oids, FILE *fp, char *delim)
{
    HeapTuple tuple;
    HeapScanDesc scandesc;
    
    int32 attr_count, i;
    AttributeTupleForm *attr;
    func_ptr *out_functions;
    int dummy;
    Oid out_func_oid;
    Oid *elements;
    Datum value;
    bool isnull = (bool) true;
    char *nulls;
    char *string;
    int32 ntuples;
    TupleDesc tupDesc;
    
    scandesc = heap_beginscan(rel, 0, NULL, 0, NULL);

    attr_count = rel->rd_att->natts;
    attr = rel->rd_att->attrs;
    tupDesc = rel->rd_att;

    if (!binary) {
	out_functions = (func_ptr *)
	    palloc(attr_count * sizeof(func_ptr));
	elements = (Oid *) palloc(attr_count * sizeof(Oid));
	for (i = 0; i < attr_count; i++) {
	    out_func_oid = (Oid) GetOutputFunction(attr[i]->atttypid);
	    fmgr_info(out_func_oid, &out_functions[i], &dummy);
	    elements[i] = GetTypeElement(attr[i]->atttypid);
	}
    }else {
	nulls = (char *) palloc(attr_count);
	for (i = 0; i < attr_count; i++) nulls[i] = ' ';
	
	/* XXX expensive */
	
	ntuples = CountTuples(rel);
	fwrite(&ntuples, sizeof(int32), 1, fp);
    }
    
    for (tuple = heap_getnext(scandesc, 0, NULL);
         tuple != NULL; 
         tuple = heap_getnext(scandesc, 0, NULL)) {

        if (oids && !binary) {
            	fputs(oidout(tuple->t_oid),fp);
		fputc(delim[0], fp);
	}
	
	for (i = 0; i < attr_count; i++) {
	    value = (Datum) 
		heap_getattr(tuple, InvalidBuffer, i+1, tupDesc, &isnull);
	    if (!binary) {
		if (!isnull) {
		    string = (char *) (out_functions[i]) (value, elements[i]);
		    CopyAttributeOut(fp, string, delim);
		    pfree(string);
		}
		if (i == attr_count - 1) {
		    fputc('\n', fp);
		}else {
		    /* when copying out, only use the first char of the delim
		       string */
		    fputc(delim[0], fp);
		}
	    }else {
		/*
		 * only interesting thing heap_getattr tells us in this case
		 * is if we have a null attribute or not.
		 */
		if (isnull) nulls[i] = 'n';
	    }
	}
	
	if (binary) {
	    int32 null_ct = 0, length;
	    
	    for (i = 0; i < attr_count; i++) {
		if (nulls[i] == 'n') null_ct++;
	    }
	    
	    length = tuple->t_len - tuple->t_hoff;
	    fwrite(&length, sizeof(int32), 1, fp);
            if (oids)
		fwrite((char *) &tuple->t_oid, sizeof(int32), 1, fp);

	    fwrite(&null_ct, sizeof(int32), 1, fp);
	    if (null_ct > 0) {
		for (i = 0; i < attr_count; i++) {
		    if (nulls[i] == 'n') {
			fwrite(&i, sizeof(int32), 1, fp);
			nulls[i] = ' ';
		    }
		}
	    }
	    fwrite((char *) tuple + tuple->t_hoff, length, 1, fp);
	}
    }
    
    heap_endscan(scandesc);
    if (binary) {
	pfree(nulls);
    }else {
	pfree(out_functions);
	pfree(elements);
    }
    
    heap_close(rel);
}

static void
CopyFrom(Relation rel, bool binary, bool oids, FILE *fp, char *delim)
{
    HeapTuple tuple;
    IndexTuple ituple;
    AttrNumber attr_count;
    AttributeTupleForm *attr;
    func_ptr *in_functions;
    int i, dummy;
    Oid in_func_oid;
    Datum *values;
    char *nulls, *index_nulls;
    bool *byval;
    bool isnull;
    bool has_index;
    int done = 0;
    char *string, *ptr;
    Relation *index_rels;
    int32 len, null_ct, null_id;
    int32 ntuples, tuples_read = 0;
    bool reading_to_eof = true;
    Oid *elements;
    FuncIndexInfo *finfo, **finfoP;
    TupleDesc *itupdescArr;
    HeapTuple pgIndexTup;
    IndexTupleForm *pgIndexP;
    int *indexNatts;
    char *predString;
    Node **indexPred;
    TupleDesc rtupdesc;
    ExprContext *econtext;
    TupleTable tupleTable;
    TupleTableSlot *slot;
    int natts;
    AttrNumber *attnumP;
    Datum idatum;
    int n_indices;
    InsertIndexResult indexRes;
    TupleDesc tupDesc;
    Oid loaded_oid;
    
    tupDesc = RelationGetTupleDescriptor(rel);
    attr = tupDesc->attrs;
    attr_count = tupDesc->natts;

    has_index = false;
    
    /*
     *  This may be a scalar or a functional index.  We initialize all
     *  kinds of arrays here to avoid doing extra work at every tuple
     *  copy.
     */
    
    if (rel->rd_rel->relhasindex) {
        GetIndexRelations(rel->rd_id, &n_indices, &index_rels);
	if (n_indices > 0) {
	    has_index = true;
	    itupdescArr = 
		(TupleDesc *)palloc(n_indices * sizeof(TupleDesc));
	    pgIndexP =
		(IndexTupleForm *)palloc(n_indices * sizeof(IndexTupleForm));
	    indexNatts = (int *) palloc(n_indices * sizeof(int));
	    finfo = (FuncIndexInfo *) palloc(n_indices * sizeof(FuncIndexInfo));
	    finfoP = (FuncIndexInfo **) palloc(n_indices * sizeof(FuncIndexInfo *));
	    indexPred = (Node **) palloc(n_indices * sizeof(Node*));
	    econtext = NULL;
	    for (i = 0; i < n_indices; i++) {
		itupdescArr[i] = RelationGetTupleDescriptor(index_rels[i]);
		pgIndexTup =
		    SearchSysCacheTuple(INDEXRELID,
					ObjectIdGetDatum(index_rels[i]->rd_id),
					0,0,0);
		Assert(pgIndexTup);
		pgIndexP[i] = (IndexTupleForm)GETSTRUCT(pgIndexTup);
		for (attnumP = &(pgIndexP[i]->indkey[0]), natts = 0;
		     *attnumP != InvalidAttrNumber;
		     attnumP++, natts++);
		if (pgIndexP[i]->indproc != InvalidOid) {
		    FIgetnArgs(&finfo[i]) = natts;
		    natts = 1;
		    FIgetProcOid(&finfo[i]) = pgIndexP[i]->indproc;
		    *(FIgetname(&finfo[i])) = '\0';
		    finfoP[i] = &finfo[i];
		} else
		    finfoP[i] = (FuncIndexInfo *) NULL;
		indexNatts[i] = natts;
		if (VARSIZE(&pgIndexP[i]->indpred) != 0) {
		    predString = fmgr(F_TEXTOUT, &pgIndexP[i]->indpred);
		    indexPred[i] = stringToNode(predString);
		    pfree(predString);
		    /* make dummy ExprContext for use by ExecQual */
		    if (econtext == NULL) {
#ifndef OMIT_PARTIAL_INDEX
			tupleTable = ExecCreateTupleTable(1); 
			slot =   ExecAllocTableSlot(tupleTable);
			econtext = makeNode(ExprContext);
			econtext->ecxt_scantuple = slot;
			rtupdesc = RelationGetTupleDescriptor(rel);
			slot->ttc_tupleDescriptor = rtupdesc;
			/*
			 * There's no buffer associated with heap tuples here,
			 * so I set the slot's buffer to NULL.  Currently, it
			 * appears that the only way a buffer could be needed
			 * would be if the partial index predicate referred to
			 * the "lock" system attribute.  If it did, then
			 * heap_getattr would call HeapTupleGetRuleLock, which
			 * uses the buffer's descriptor to get the relation id.
			 * Rather than try to fix this, I'll just disallow
			 * partial indexes on "lock", which wouldn't be useful
			 * anyway. --Nels, Nov '92
			 */
			/* SetSlotBuffer(slot, (Buffer) NULL); */
			/* SetSlotShouldFree(slot, false); */
			slot->ttc_buffer = (Buffer)NULL;
			slot->ttc_shouldFree = false;
#endif /* OMIT_PARTIAL_INDEX */    
		    }
		} else {
		    indexPred[i] = NULL;
		}
	    }
	}
    }
    
    if (!binary)
	{
	    in_functions = (func_ptr *) palloc(attr_count * sizeof(func_ptr));
	    elements = (Oid *) palloc(attr_count * sizeof(Oid));
	    for (i = 0; i < attr_count; i++)
		{
		    in_func_oid = (Oid) GetInputFunction(attr[i]->atttypid);
		    fmgr_info(in_func_oid, &in_functions[i], &dummy);
		    elements[i] = GetTypeElement(attr[i]->atttypid);
		}
	}
    else
	{
	    fread(&ntuples, sizeof(int32), 1, fp);
	    if (ntuples != 0) reading_to_eof = false;
	}
    
    values       = (Datum *) palloc(sizeof(Datum) * attr_count);
    nulls        = (char *) palloc(attr_count);
    index_nulls  = (char *) palloc(attr_count);
    byval        = (bool *) palloc(attr_count * sizeof(bool));
    
    for (i = 0; i < attr_count; i++) {
	nulls[i] = ' ';
	index_nulls[i] = ' ';
	byval[i] = (bool) IsTypeByVal(attr[i]->atttypid);
    }
    
    while (!done) {
	if (!binary) {
	    if (oids) {
		string = CopyReadAttribute(fp, &isnull, delim);
		if (string == NULL)
		    done = 1;
		else {
		    loaded_oid = oidin(string);
		    if (loaded_oid < BootstrapObjectIdData)
			elog(WARN, "COPY TEXT: Invalid Oid");
		}
	    }
	    for (i = 0; i < attr_count && !done; i++) {
		string = CopyReadAttribute(fp, &isnull, delim);
		if (isnull) {
		    values[i] = PointerGetDatum(NULL);
		    nulls[i] = 'n';
		}else if (string == NULL) {
		    done = 1;
		}else {
		    values[i] =
			(Datum)(in_functions[i])(string,
						 elements[i],
						 attr[i]->attlen);
		    /*
		     * Sanity check - by reference attributes cannot return
		     * NULL
		     */
		    if (!PointerIsValid(values[i]) &&
			!(rel->rd_att->attrs[i]->attbyval)) {
			elog(WARN, "copy from: Bad file format");
		    }
		}
	    }
	}else { /* binary */
	    fread(&len, sizeof(int32), 1, fp);
	    if (feof(fp)) {
		done = 1;
	    }else {
		if (oids) {
		    fread(&loaded_oid, sizeof(int32), 1, fp);
		    if (loaded_oid < BootstrapObjectIdData)
			elog(WARN, "COPY BINARY: Invalid Oid");
		}
		fread(&null_ct, sizeof(int32), 1, fp);
		if (null_ct > 0) {
		    for (i = 0; i < null_ct; i++) {
			fread(&null_id, sizeof(int32), 1, fp);
			nulls[null_id] = 'n';
		    }
		}
		
		string = (char *) palloc(len);
		fread(string, len, 1, fp);
		
		ptr = string;
		
		for (i = 0; i < attr_count; i++) {
		    if (byval[i] && nulls[i] != 'n') {
			
			switch(attr[i]->attlen) {
			case sizeof(char):
			    values[i] = (Datum) *(unsigned char *) ptr;
			    ptr += sizeof(char);
			    break;
			case sizeof(short):
			    ptr = (char *) SHORTALIGN(ptr);
			    values[i] = (Datum) *(unsigned short *) ptr; 
			    ptr += sizeof(short);
			    break;
			case sizeof(int32):
			    ptr = (char *) INTALIGN(ptr);
			    values[i] = (Datum) *(uint32 *) ptr;
			    ptr += sizeof(int32);
			    break;
			default:
			    elog(WARN, "COPY BINARY: impossible size!");
			    break;
			}
		    }else if (nulls[i] != 'n') {
			switch (attr[i]->attlen) {
			case -1:
			    if (attr[i]->attalign == 'd') 
				ptr = (char *)DOUBLEALIGN(ptr);
			    else
				ptr = (char *)INTALIGN(ptr);
			    values[i] = (Datum) ptr;
			    ptr += * (uint32 *) ptr;
			    break;
			case sizeof(char):
			    values[i] = (Datum)ptr;
			    ptr += attr[i]->attlen;
			    break;
			case sizeof(short):
			    ptr = (char*)SHORTALIGN(ptr);
			    values[i] = (Datum)ptr;
			    ptr += attr[i]->attlen;
			    break;
			case sizeof(int32):
			    ptr = (char*)INTALIGN(ptr);
			    values[i] = (Datum)ptr;
			    ptr += attr[i]->attlen;
			    break;
			default:
			    if (attr[i]->attalign == 'd')
				ptr = (char *)DOUBLEALIGN(ptr);
			    else 
				ptr = (char *)LONGALIGN(ptr);
			    values[i] = (Datum) ptr;
			    ptr += attr[i]->attlen;
			}
		    }
		}
	    }
	}
	if (done) continue;
	    
	tupDesc = CreateTupleDesc(attr_count, attr);
	tuple = heap_formtuple(tupDesc, values, nulls);
	if (oids)
	    tuple->t_oid = loaded_oid;
	heap_insert(rel, tuple);
	    
	if (has_index) {
	    for (i = 0; i < n_indices; i++) {
		if (indexPred[i] != NULL) {
#ifndef OMIT_PARTIAL_INDEX
		    /* if tuple doesn't satisfy predicate,
		     * don't update index
		     */
		    slot->val = tuple;
		    /*SetSlotContents(slot, tuple); */
		    if (ExecQual((List*)indexPred[i], econtext) == false)
			continue;
#endif /* OMIT_PARTIAL_INDEX */    	
		}
		FormIndexDatum(indexNatts[i],
			       (AttrNumber *)&(pgIndexP[i]->indkey[0]),
			       tuple,
			       tupDesc,
			       InvalidBuffer,
			       &idatum,
			       index_nulls,
			       finfoP[i]);
		ituple = index_formtuple(itupdescArr[i], &idatum, index_nulls);
		ituple->t_tid = tuple->t_ctid;
		indexRes = index_insert(index_rels[i], ituple);
		if (indexRes) pfree(indexRes);
		pfree(ituple);
	    }
	}
	    
	if (binary) pfree(string);
	    
	for (i = 0; i < attr_count; i++) {
	    if (!byval[i] && nulls[i] != 'n') {
		if (!binary) pfree((void*)values[i]);
	    }else if (nulls[i] == 'n') {
		nulls[i] = ' ';
	    }
	}
	    
	pfree(tuple);
	tuples_read++;
	    
	if (!reading_to_eof && ntuples == tuples_read) done = true;
    }
    pfree(values);
    if (!binary) pfree(in_functions);
    pfree(nulls);
    pfree(byval);
    heap_close(rel);
}

static Oid
GetOutputFunction(Oid type)
{
    HeapTuple    typeTuple;
    
    typeTuple = SearchSysCacheTuple(TYPOID,
				    ObjectIdGetDatum(type),
				    0,0,0);
    
    if (HeapTupleIsValid(typeTuple))
	return((int) ((TypeTupleForm) GETSTRUCT(typeTuple))->typoutput);
    
    elog(WARN, "GetOutputFunction: Cache lookup of type %d failed", type);
    return(InvalidOid);
}

static Oid
GetTypeElement(Oid type)
{
    HeapTuple    typeTuple;
    
    typeTuple = SearchSysCacheTuple(TYPOID,
				    ObjectIdGetDatum(type),
				    0,0,0);

    
    if (HeapTupleIsValid(typeTuple))
	return((int) ((TypeTupleForm) GETSTRUCT(typeTuple))->typelem);
    
    elog(WARN, "GetOutputFunction: Cache lookup of type %d failed", type);
    return(InvalidOid);
}

static Oid
GetInputFunction(Oid type)
{
    HeapTuple    typeTuple;
    
    typeTuple = SearchSysCacheTuple(TYPOID,
				    ObjectIdGetDatum(type),
				    0,0,0);
    
    if (HeapTupleIsValid(typeTuple))
	return((int) ((TypeTupleForm) GETSTRUCT(typeTuple))->typinput);
    
    elog(WARN, "GetInputFunction: Cache lookup of type %d failed", type);
    return(InvalidOid);
}

static Oid
IsTypeByVal(Oid type)
{
    HeapTuple    typeTuple;
    
    typeTuple = SearchSysCacheTuple(TYPOID,
				    ObjectIdGetDatum(type),
				    0,0,0);
    
    if (HeapTupleIsValid(typeTuple))
        return((int) ((TypeTupleForm) GETSTRUCT(typeTuple))->typbyval);
    
    elog(WARN, "GetInputFunction: Cache lookup of type %d failed", type);
    
    return(InvalidOid);
}

/* 
 * Given the OID of a relation, return an array of index relation descriptors
 * and the number of index relations.  These relation descriptors are open
 * using heap_open().
 *
 * Space for the array itself is palloc'ed.
 */

typedef struct rel_list {
    Oid index_rel_oid;
    struct rel_list *next;
} RelationList;

static void
GetIndexRelations(Oid main_relation_oid,
		  int *n_indices,
		  Relation **index_rels)
{
    RelationList *head, *scan;
    Relation pg_index_rel;
    HeapScanDesc scandesc;
    Oid index_relation_oid;
    HeapTuple tuple;
    TupleDesc tupDesc;
    int i;
    bool isnull;
    
    pg_index_rel = heap_openr(IndexRelationName);
    scandesc = heap_beginscan(pg_index_rel, 0, NULL, 0, NULL);
    tupDesc = RelationGetTupleDescriptor(pg_index_rel);
    
    *n_indices = 0;
    
    head = (RelationList *) palloc(sizeof(RelationList));
    scan = head;
    head->next = NULL;
    
    for (tuple = heap_getnext(scandesc, 0, NULL);
         tuple != NULL; 
         tuple = heap_getnext(scandesc, 0, NULL)) {
	
	index_relation_oid =
	    (Oid) DatumGetInt32(heap_getattr(tuple, InvalidBuffer, 2,
					     tupDesc, &isnull));
	if (index_relation_oid == main_relation_oid) {
	    scan->index_rel_oid =
		(Oid) DatumGetInt32(heap_getattr(tuple, InvalidBuffer,
						 Anum_pg_index_indexrelid,
						 tupDesc, &isnull));
	    (*n_indices)++;
	    scan->next = (RelationList *) palloc(sizeof(RelationList));
	    scan = scan->next;
	}
    }
    
    heap_endscan(scandesc);
    heap_close(pg_index_rel);
    
    /* We cannot trust to relhasindex of the main_relation now, so... */
    if ( *n_indices == 0 )
      return;

    *index_rels = (Relation *) palloc(*n_indices * sizeof(Relation));
    
    for (i = 0, scan = head; i < *n_indices; i++, scan = scan->next) {
	(*index_rels)[i] = index_open(scan->index_rel_oid);
    }
    
    for (i = 0, scan = head; i < *n_indices + 1; i++) {
	scan = head->next;
	pfree(head);
	head = scan;
    }
}

#define EXT_ATTLEN 5*8192

/*
   returns 1 is c is in s
*/
static bool
inString(char c, char* s)
{
    int i;

    if (s) {
	i = 0;
	while (s[i] != '\0') {
	    if (s[i] == c)
		return 1;
	    i++;
	}
    }
    return 0;
}

/*
 * Reads input from fp until eof is seen.  If we are reading from standard
 * input, AND we see a dot on a line by itself (a dot followed immediately
 * by a newline), we exit as if we saw eof.  This is so that copy pipelines
 * can be used as standard input.
 */

static char *
CopyReadAttribute(FILE *fp, bool *isnull, char *delim)
{
    static char attribute[EXT_ATTLEN];
    char c;
    int done = 0;
    int i = 0;
    
    if (feof(fp)) {
	*isnull = (bool) false;
	return(NULL);
    }
    
    while (!done) {
	c = getc(fp);
	    
	if (feof(fp)) {
	    *isnull = (bool) false;
	    return(NULL);
	}else if (reading_from_input && attno == 0 && i == 0 && c == '.') {
	    attribute[0] = c;
	    c = getc(fp);
	    if (c == '\n') {
		*isnull = (bool) false;
		return(NULL);
	    }else if (inString(c,delim)) {
		attribute[1] = 0;
		*isnull = (bool) false;
		return(&attribute[0]);
	    }else {
		attribute[1] = c;
		i = 2;
	    }
	}else if (c == '\\') {
	    c = getc(fp);
#ifdef ESCAPE_PATCH
#define ISOCTAL(c)    (((c) >= '0') && ((c) <= '7'))
#define       VALUE(c)        ((c) - '0')
            if (feof(fp)) {
                *isnull = (bool) false;
                return(NULL);
            }
            switch (c) {
              case '0':
              case '1':
              case '2':
              case '3':
              case '4':
              case '5':
              case '6':
              case '7': {
                int val;
                val = VALUE(c);
                c = getc(fp);
                if (ISOCTAL(c)) {
                    val = (val<<3) + VALUE(c);
                    c = getc(fp);
                    if (ISOCTAL(c)) {
                        val = (val<<3) + VALUE(c);
                    } else {
                        if (feof(fp)) {
                            *isnull = (bool) false;
                            return(NULL);
                        }
                        ungetc(c, fp);
                    }
                } else {
                    if (feof(fp)) {
                        *isnull = (bool) false;
                        return(NULL);
                    }
                    ungetc(c, fp);
                }
                c = val & 0377;
            }
                break;
              case 'b':
                c = '\b';
                break;
              case 'f':
                c = '\f';
                break;
              case 'n':
                c = '\n';
                break;
              case 'r':
                c = '\r';
                break;
              case 't':
                c = '\t';
                break;
              case 'v':
                c = '\v';
                break;
            }
#endif
	}else if (inString(c,delim) || c == '\n') {
	    done = 1;
	}
	if (!done) attribute[i++] = c;
	if (i == EXT_ATTLEN - 1)
	    elog(WARN, "CopyReadAttribute - attribute length too long");
    }
    attribute[i] = '\0';
    if (i == 0) {
	*isnull = (bool) true;
	return(NULL);
    }else {
	*isnull = (bool) false;
	return(&attribute[0]);
    }
}

#ifdef ESCAPE_PATCH
static void
CopyAttributeOut(FILE *fp, char *string, char *delim)
{
    char c;
    int is_array = false;
    int len = strlen(string);

    /* XXX - This is a kludge, we should check the data type */
    if (len && (string[0] == '{') && (string[len-1] == '}')) {
      is_array = true;
    }

    for ( ; c = *string; string++) {
      if ((c == delim[0]) || (c == '\n')) {
          fputc('\\', fp);
      } else if ((c == '\\') && is_array) {
          if (*(string+1) == '\\') {
              /* translate \\ to \\\\ */
              fputc('\\', fp);
              fputc('\\', fp);
              fputc('\\', fp);
              string++;
          } else if (*(string+1) == '"') {
              /* translate \" to \\\" */
              fputc('\\', fp);
              fputc('\\', fp);
          }
      }
      fputc(*string, fp);
    }
}
#else
static void
CopyAttributeOut(FILE *fp, char *string, char *delim)
{
    int i;
    int len = strlen(string);
    
    for (i = 0; i < len; i++) {
	if (string[i] == delim[0] || string[i] == '\n' || string[i] == '\\') {
	    fputc('\\', fp);
	}
	fputc(string[i], fp);
    }
}
#endif

/*
 * Returns the number of tuples in a relation.  Unfortunately, currently
 * must do a scan of the entire relation to determine this.
 *
 * relation is expected to be an open relation descriptor.
 */
static int
CountTuples(Relation relation)
{
    HeapScanDesc scandesc;
    HeapTuple tuple;
    
    int i;
    
    scandesc = heap_beginscan(relation, 0, NULL, 0, NULL);
    
    for (tuple = heap_getnext(scandesc, 0, NULL), i = 0;
         tuple != NULL; 
         tuple = heap_getnext(scandesc, 0, NULL), i++)
	;
    heap_endscan(scandesc);
    return(i);
}
