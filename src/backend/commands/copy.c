/*-------------------------------------------------------------------------
 *
 * copy.c
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/copy.c,v 1.91 1999/11/22 17:56:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <sys/stat.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define VALUE(c) ((c) - '0')


/* non-export function prototypes */
static void CopyTo(Relation rel, bool binary, bool oids, FILE *fp, char *delim);
static void CopyFrom(Relation rel, bool binary, bool oids, FILE *fp, char *delim);
static Oid	GetOutputFunction(Oid type);
static Oid	GetTypeElement(Oid type);
static Oid	GetInputFunction(Oid type);
static Oid	IsTypeByVal(Oid type);
static void GetIndexRelations(Oid main_relation_oid,
				  int *n_indices,
				  Relation **index_rels);

static void CopyReadNewline(FILE *fp, int *newline);
static char *CopyReadAttribute(FILE *fp, bool *isnull, char *delim, int *newline);

static void CopyAttributeOut(FILE *fp, char *string, char *delim);
static int	CountTuples(Relation relation);

/*
 * Static communication variables ... pretty grotty, but COPY has
 * never been reentrant...
 */
static int	lineno;
static bool	fe_eof;

/*
 * Internal communications functions
 */
static void CopySendData(void *databuf, int datasize, FILE *fp);
static void CopySendString(char *str, FILE *fp);
static void CopySendChar(char c, FILE *fp);
static void CopyGetData(void *databuf, int datasize, FILE *fp);
static int	CopyGetChar(FILE *fp);
static int	CopyGetEof(FILE *fp);
static int	CopyPeekChar(FILE *fp);
static void CopyDonePeek(FILE *fp, int c, int pickup);

/*
 * CopySendData sends output data either to the file
 *	specified by fp or, if fp is NULL, using the standard
 *	backend->frontend functions
 *
 * CopySendString does the same for null-terminated strings
 * CopySendChar does the same for single characters
 *
 * NB: no data conversion is applied by these functions
 */
static void
CopySendData(void *databuf, int datasize, FILE *fp)
{
	if (!fp)
	{
		if (pq_putbytes((char *) databuf, datasize))
			fe_eof = true;
	}
	else
		fwrite(databuf, datasize, 1, fp);
}

static void
CopySendString(char *str, FILE *fp)
{
	CopySendData(str, strlen(str), fp);
}

static void
CopySendChar(char c, FILE *fp)
{
	CopySendData(&c, 1, fp);
}

/*
 * CopyGetData reads output data either from the file
 *	specified by fp or, if fp is NULL, using the standard
 *	backend->frontend functions
 *
 * CopyGetChar does the same for single characters
 * CopyGetEof checks if it's EOF on the input
 *
 * NB: no data conversion is applied by these functions
 */
static void
CopyGetData(void *databuf, int datasize, FILE *fp)
{
	if (!fp)
	{
		if (pq_getbytes((char *) databuf, datasize))
			fe_eof = true;
	}
	else
		fread(databuf, datasize, 1, fp);
}

static int
CopyGetChar(FILE *fp)
{
	if (!fp)
	{
		unsigned char ch;

		if (pq_getbytes((char *) &ch, 1))
		{
			fe_eof = true;
			return EOF;
		}
		return ch;
	}
	else
		return getc(fp);
}

static int
CopyGetEof(FILE *fp)
{
	if (!fp)
		return fe_eof;
	else
		return feof(fp);
}

/*
 * CopyPeekChar reads a byte in "peekable" mode.
 * after each call to CopyPeekChar, a call to CopyDonePeek _must_
 * follow, unless EOF was returned.
 * CopyDonePeek will either take the peeked char off the steam
 * (if pickup is != 0) or leave it on the stream (if pickup == 0)
 */
static int
CopyPeekChar(FILE *fp)
{
	if (!fp)
	{
		int ch = pq_peekbyte();
		if (ch == EOF)
			fe_eof = true;
		return ch;
	}
	else
		return getc(fp);
}

static void
CopyDonePeek(FILE *fp, int c, int pickup)
{
	if (!fp)
	{
		if (pickup)
		{

			/*
			 * We want to pick it up - just receive again into dummy
			 * buffer
			 */
			char		c;

			pq_getbytes(&c, 1);
		}
		/* If we didn't want to pick it up, just leave it where it sits */
	}
	else
	{
		if (!pickup)
		{
			/* We don't want to pick it up - so put it back in there */
			ungetc(c, fp);
		}
		/* If we wanted to pick it up, it's already there */
	}
}



/*
 *	 DoCopy executes the SQL COPY statement.
 */

void
DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe,
	   char *filename, char *delim, int fileumask)
{
/*----------------------------------------------------------------------------
  Either unload or reload contents of class <relname>, depending on <from>.

  If <pipe> is false, transfer is between the class and the file named
  <filename>.  Otherwise, transfer is between the class and our regular
  input/output stream.	The latter could be either stdin/stdout or a
  socket, depending on whether we're running under Postmaster control.

  Iff <binary>, unload or reload in the binary format, as opposed to the
  more wasteful but more robust and portable text format.

  If in the text format, delimit columns with delimiter <delim>.

  <fileumask> is the umask(2) setting to use while creating an output file.
  This should usually be more liberal than the backend's normal 077 umask,
  but not always (in particular, "pg_pwd" should be written with 077!).
  Up through version 6.5, <fileumask> was always 000, which was foolhardy.

  When loading in the text format from an input stream (as opposed to
  a file), recognize a "." on a line by itself as EOF.	Also recognize
  a stream EOF.  When unloading in the text format to an output stream,
  write a "." on a line by itself at the end of the data.

  Iff <oids>, unload or reload the format that includes OID information.

  Do not allow a Postgres user without superuser privilege to read from
  or write to a file.

  Do not allow the copy if user doesn't have proper permission to access
  the class.
----------------------------------------------------------------------------*/

	FILE	   *fp;
	Relation	rel;
	extern char *UserName;		/* defined in global.c */
	const AclMode required_access = from ? ACL_WR : ACL_RD;
	LOCKMODE	required_lock = from ? AccessExclusiveLock : AccessShareLock;
	/* Note: AccessExclusive is probably overkill for copying to a relation,
	 * but that's what the existing code grabs on the rel's indices.  If
	 * this is relaxed then I think the index locks need relaxed also.
	 */
	int			result;

	rel = heap_openr(relname, required_lock);

	result = pg_aclcheck(relname, UserName, required_access);
	if (result != ACLCHECK_OK)
		elog(ERROR, "%s: %s", relname, aclcheck_error_strings[result]);
	else if (!pipe && !superuser())
		elog(ERROR, "You must have Postgres superuser privilege to do a COPY "
			 "directly to or from a file.  Anyone can COPY to stdout or "
			 "from stdin.  Psql's \\copy command also works for anyone.");
	else
	{
		if (from)
		{						/* copy from file to database */
			if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				elog(ERROR, "You can't change sequence relation %s", relname);
			if (pipe)
			{
				if (IsUnderPostmaster)
				{
					ReceiveCopyBegin();
					fp = NULL;
				}
				else
					fp = stdin;
			}
			else
			{
#ifndef __CYGWIN32__
				fp = AllocateFile(filename, "r");
#else
				fp = AllocateFile(filename, "rb");
#endif
				if (fp == NULL)
					elog(ERROR, "COPY command, running in backend with "
						 "effective uid %d, could not open file '%s' for "
						 "reading.  Errno = %s (%d).",
						 geteuid(), filename, strerror(errno), errno);
			}
			CopyFrom(rel, binary, oids, fp, delim);
		}
		else
		{						/* copy from database to file */
			if (pipe)
			{
				if (IsUnderPostmaster)
				{
					SendCopyBegin();
					pq_startcopyout();
					fp = NULL;
				}
				else
					fp = stdout;
			}
			else
			{
				mode_t		oumask;		/* Pre-existing umask value */

				oumask = umask((mode_t) fileumask);
#ifndef __CYGWIN32__
				fp = AllocateFile(filename, "w");
#else
				fp = AllocateFile(filename, "wb");
#endif
				umask(oumask);
				if (fp == NULL)
					elog(ERROR, "COPY command, running in backend with "
						 "effective uid %d, could not open file '%s' for "
						 "writing.  Errno = %s (%d).",
						 geteuid(), filename, strerror(errno), errno);
			}
			CopyTo(rel, binary, oids, fp, delim);
		}
		if (!pipe)
		{
			FreeFile(fp);
		}
		else if (!from)
		{
			if (!binary)
				CopySendData("\\.\n", 3, fp);
			if (IsUnderPostmaster)
				pq_endcopyout(false);
		}
	}

	heap_close(rel, required_lock);
}



static void
CopyTo(Relation rel, bool binary, bool oids, FILE *fp, char *delim)
{
	HeapTuple	tuple;
	HeapScanDesc scandesc;

	int32		attr_count,
				i;
	Form_pg_attribute *attr;
	FmgrInfo   *out_functions;
	Oid			out_func_oid;
	Oid		   *elements;
	int32	   *typmod;
	Datum		value;
	bool		isnull;			/* The attribute we are copying is null */
	char	   *nulls;

	/*
	 * <nulls> is a (dynamically allocated) array with one character per
	 * attribute in the instance being copied.	nulls[I-1] is 'n' if
	 * Attribute Number I is null, and ' ' otherwise.
	 *
	 * <nulls> is meaningful only if we are doing a binary copy.
	 */
	char	   *string;
	int32		ntuples;
	TupleDesc	tupDesc;

	scandesc = heap_beginscan(rel, 0, QuerySnapshot, 0, NULL);

	attr_count = rel->rd_att->natts;
	attr = rel->rd_att->attrs;
	tupDesc = rel->rd_att;

	if (!binary)
	{
		out_functions = (FmgrInfo *) palloc(attr_count * sizeof(FmgrInfo));
		elements = (Oid *) palloc(attr_count * sizeof(Oid));
		typmod = (int32 *) palloc(attr_count * sizeof(int32));
		for (i = 0; i < attr_count; i++)
		{
			out_func_oid = (Oid) GetOutputFunction(attr[i]->atttypid);
			fmgr_info(out_func_oid, &out_functions[i]);
			elements[i] = GetTypeElement(attr[i]->atttypid);
			typmod[i] = attr[i]->atttypmod;
		}
		nulls = NULL;			/* meaningless, but compiler doesn't know
								 * that */
	}
	else
	{
		elements = NULL;
		typmod = NULL;
		out_functions = NULL;
		nulls = (char *) palloc(attr_count);
		for (i = 0; i < attr_count; i++)
			nulls[i] = ' ';

		/* XXX expensive */

		ntuples = CountTuples(rel);
		CopySendData(&ntuples, sizeof(int32), fp);
	}

	while (HeapTupleIsValid(tuple = heap_getnext(scandesc, 0)))
	{
		if (QueryCancel)
			CancelQuery();

		if (oids && !binary)
		{
			CopySendString(oidout(tuple->t_data->t_oid), fp);
			CopySendChar(delim[0], fp);
		}

		for (i = 0; i < attr_count; i++)
		{
			value = heap_getattr(tuple, i + 1, tupDesc, &isnull);
			if (!binary)
			{
				if (!isnull)
				{
					string = (char *) (*fmgr_faddr(&out_functions[i]))
						(value, elements[i], typmod[i]);
					CopyAttributeOut(fp, string, delim);
					pfree(string);
				}
				else
					CopySendString("\\N", fp);	/* null indicator */

				if (i == attr_count - 1)
					CopySendChar('\n', fp);
				else
				{

					/*
					 * when copying out, only use the first char of the
					 * delim string
					 */
					CopySendChar(delim[0], fp);
				}
			}
			else
			{

				/*
				 * only interesting thing heap_getattr tells us in this
				 * case is if we have a null attribute or not.
				 */
				if (isnull)
					nulls[i] = 'n';
			}
		}

		if (binary)
		{
			int32		null_ct = 0,
						length;

			for (i = 0; i < attr_count; i++)
			{
				if (nulls[i] == 'n')
					null_ct++;
			}

			length = tuple->t_len - tuple->t_data->t_hoff;
			CopySendData(&length, sizeof(int32), fp);
			if (oids)
				CopySendData((char *) &tuple->t_data->t_oid, sizeof(int32), fp);

			CopySendData(&null_ct, sizeof(int32), fp);
			if (null_ct > 0)
			{
				for (i = 0; i < attr_count; i++)
				{
					if (nulls[i] == 'n')
					{
						CopySendData(&i, sizeof(int32), fp);
						nulls[i] = ' ';
					}
				}
			}
			CopySendData((char *) tuple->t_data + tuple->t_data->t_hoff,
						 length, fp);
		}
	}

	heap_endscan(scandesc);
	if (binary)
		pfree(nulls);
	else
	{
		pfree(out_functions);
		pfree(elements);
		pfree(typmod);
	}
}

static void
CopyFrom(Relation rel, bool binary, bool oids, FILE *fp, char *delim)
{
	HeapTuple	tuple;
	AttrNumber	attr_count;
	Form_pg_attribute *attr;
	FmgrInfo   *in_functions;
	int			i;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls,
			   *index_nulls;
	bool	   *byval;
	bool		isnull;
	bool		has_index;
	int			done = 0;
	char	   *string = NULL,
			   *ptr;
	Relation   *index_rels;
	int32		len,
				null_ct,
				null_id;
	int32		ntuples,
				tuples_read = 0;
	bool		reading_to_eof = true;
	Oid		   *elements;
	int32	   *typmod;
	FuncIndexInfo *finfo,
			  **finfoP = NULL;
	TupleDesc  *itupdescArr;
	HeapTuple	pgIndexTup;
	Form_pg_index *pgIndexP = NULL;
	int		   *indexNatts = NULL;
	char	   *predString;
	Node	  **indexPred = NULL;
	TupleDesc	rtupdesc;
	ExprContext *econtext = NULL;
	EState	   *estate = makeNode(EState);		/* for ExecConstraints() */

#ifndef OMIT_PARTIAL_INDEX
	TupleTable	tupleTable;
	TupleTableSlot *slot = NULL;

#endif
	int			natts;
	AttrNumber *attnumP;
	Datum	   *idatum;
	int			n_indices;
	InsertIndexResult indexRes;
	TupleDesc	tupDesc;
	Oid			loaded_oid;
	bool		skip_tuple = false;

	tupDesc = RelationGetDescr(rel);
	attr = tupDesc->attrs;
	attr_count = tupDesc->natts;

	has_index = false;

	/*
	 * This may be a scalar or a functional index.	We initialize all
	 * kinds of arrays here to avoid doing extra work at every tuple copy.
	 */

	if (rel->rd_rel->relhasindex)
	{
		GetIndexRelations(RelationGetRelid(rel), &n_indices, &index_rels);
		if (n_indices > 0)
		{
			has_index = true;
			itupdescArr = (TupleDesc *) palloc(n_indices * sizeof(TupleDesc));
			pgIndexP = (Form_pg_index *) palloc(n_indices * sizeof(Form_pg_index));
			indexNatts = (int *) palloc(n_indices * sizeof(int));
			finfo = (FuncIndexInfo *) palloc(n_indices * sizeof(FuncIndexInfo));
			finfoP = (FuncIndexInfo **) palloc(n_indices * sizeof(FuncIndexInfo *));
			indexPred = (Node **) palloc(n_indices * sizeof(Node *));
			econtext = NULL;
			for (i = 0; i < n_indices; i++)
			{
				itupdescArr[i] = RelationGetDescr(index_rels[i]);
				pgIndexTup = SearchSysCacheTuple(INDEXRELID,
					   ObjectIdGetDatum(RelationGetRelid(index_rels[i])),
												 0, 0, 0);
				Assert(pgIndexTup);
				pgIndexP[i] = (Form_pg_index) GETSTRUCT(pgIndexTup);
				for (attnumP = &(pgIndexP[i]->indkey[0]), natts = 0;
				 natts < INDEX_MAX_KEYS && *attnumP != InvalidAttrNumber;
					 attnumP++, natts++);
				if (pgIndexP[i]->indproc != InvalidOid)
				{
					FIgetnArgs(&finfo[i]) = natts;
					natts = 1;
					FIgetProcOid(&finfo[i]) = pgIndexP[i]->indproc;
					*(FIgetname(&finfo[i])) = '\0';
					finfoP[i] = &finfo[i];
				}
				else
					finfoP[i] = (FuncIndexInfo *) NULL;
				indexNatts[i] = natts;
				if (VARSIZE(&pgIndexP[i]->indpred) != 0)
				{
					predString = fmgr(F_TEXTOUT, &pgIndexP[i]->indpred);
					indexPred[i] = stringToNode(predString);
					pfree(predString);
					/* make dummy ExprContext for use by ExecQual */
					if (econtext == NULL)
					{
#ifndef OMIT_PARTIAL_INDEX
						tupleTable = ExecCreateTupleTable(1);
						slot = ExecAllocTableSlot(tupleTable);
						econtext = makeNode(ExprContext);
						econtext->ecxt_scantuple = slot;
						rtupdesc = RelationGetDescr(rel);
						slot->ttc_tupleDescriptor = rtupdesc;

						/*
						 * There's no buffer associated with heap tuples
						 * here, so I set the slot's buffer to NULL.
						 * Currently, it appears that the only way a
						 * buffer could be needed would be if the partial
						 * index predicate referred to the "lock" system
						 * attribute.  If it did, then heap_getattr would
						 * call HeapTupleGetRuleLock, which uses the
						 * buffer's descriptor to get the relation id.
						 * Rather than try to fix this, I'll just disallow
						 * partial indexes on "lock", which wouldn't be
						 * useful anyway. --Nels, Nov '92
						 */
						/* SetSlotBuffer(slot, (Buffer) NULL); */
						/* SetSlotShouldFree(slot, false); */
						slot->ttc_buffer = (Buffer) NULL;
						slot->ttc_shouldFree = false;
#endif	 /* OMIT_PARTIAL_INDEX */
					}
				}
				else
					indexPred[i] = NULL;
			}
		}
	}

	if (!binary)
	{
		in_functions = (FmgrInfo *) palloc(attr_count * sizeof(FmgrInfo));
		elements = (Oid *) palloc(attr_count * sizeof(Oid));
		typmod = (int32 *) palloc(attr_count * sizeof(int32));
		for (i = 0; i < attr_count; i++)
		{
			in_func_oid = (Oid) GetInputFunction(attr[i]->atttypid);
			fmgr_info(in_func_oid, &in_functions[i]);
			elements[i] = GetTypeElement(attr[i]->atttypid);
			typmod[i] = attr[i]->atttypmod;
		}
	}
	else
	{
		in_functions = NULL;
		elements = NULL;
		typmod = NULL;
		CopyGetData(&ntuples, sizeof(int32), fp);
		if (ntuples != 0)
			reading_to_eof = false;
	}

	values = (Datum *) palloc(sizeof(Datum) * attr_count);
	nulls = (char *) palloc(attr_count);
	index_nulls = (char *) palloc(attr_count);
	idatum = (Datum *) palloc(sizeof(Datum) * attr_count);
	byval = (bool *) palloc(attr_count * sizeof(bool));

	for (i = 0; i < attr_count; i++)
	{
		nulls[i] = ' ';
		index_nulls[i] = ' ';
		byval[i] = (bool) IsTypeByVal(attr[i]->atttypid);
	}

	lineno = 0;
	fe_eof = false;

	while (!done)
	{
		if (QueryCancel)
			CancelQuery();

		if (!binary)
		{
			int			newline = 0;

			lineno++;
			if (oids)
			{
				string = CopyReadAttribute(fp, &isnull, delim, &newline);
				if (string == NULL)
					done = 1;
				else
				{
					loaded_oid = oidin(string);
					if (loaded_oid < BootstrapObjectIdData)
						elog(ERROR, "COPY TEXT: Invalid Oid. line: %d", lineno);
					pfree(string);
				}
			}
			for (i = 0; i < attr_count && !done; i++)
			{
				string = CopyReadAttribute(fp, &isnull, delim, &newline);
				if (isnull)
				{
					values[i] = PointerGetDatum(NULL);
					nulls[i] = 'n';
					if (string)
						pfree(string);
				}
				else if (string == NULL)
					done = 1;
				else
				{
					values[i] = (Datum) (*fmgr_faddr(&in_functions[i])) (string,
															 elements[i],
															  typmod[i]);

					/*
					 * Sanity check - by reference attributes cannot
					 * return NULL
					 */
					if (!PointerIsValid(values[i]) &&
						!(rel->rd_att->attrs[i]->attbyval))
						elog(ERROR, "copy from line %d: Bad file format", lineno);
					pfree(string);
				}
			}
			if (!done)
				CopyReadNewline(fp, &newline);
		}
		else
		{						/* binary */
			CopyGetData(&len, sizeof(int32), fp);
			if (CopyGetEof(fp))
				done = 1;
			else
			{
				if (oids)
				{
					CopyGetData(&loaded_oid, sizeof(int32), fp);
					if (loaded_oid < BootstrapObjectIdData)
						elog(ERROR, "COPY BINARY: Invalid Oid line: %d", lineno);
				}
				CopyGetData(&null_ct, sizeof(int32), fp);
				if (null_ct > 0)
				{
					for (i = 0; i < null_ct; i++)
					{
						CopyGetData(&null_id, sizeof(int32), fp);
						nulls[null_id] = 'n';
					}
				}

				string = (char *) palloc(len);
				CopyGetData(string, len, fp);

				ptr = string;

				for (i = 0; i < attr_count; i++)
				{
					if (byval[i] && nulls[i] != 'n')
					{

						switch (attr[i]->attlen)
						{
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
								elog(ERROR, "COPY BINARY: impossible size! line: %d", lineno);
								break;
						}
					}
					else if (nulls[i] != 'n')
					{
						ptr = (char *) att_align(ptr, attr[i]->attlen, attr[i]->attalign);
						values[i] = (Datum) ptr;
						ptr = att_addlength(ptr, attr[i]->attlen, ptr);
					}
				}
			}
		}
		if (done)
			continue;

		tuple = heap_formtuple(tupDesc, values, nulls);
		if (oids)
			tuple->t_data->t_oid = loaded_oid;

		skip_tuple = false;
		/* BEFORE ROW INSERT Triggers */
		if (rel->trigdesc &&
			rel->trigdesc->n_before_row[TRIGGER_EVENT_INSERT] > 0)
		{
			HeapTuple	newtuple;

			newtuple = ExecBRInsertTriggers(rel, tuple);

			if (newtuple == NULL)		/* "do nothing" */
				skip_tuple = true;
			else if (newtuple != tuple) /* modified by Trigger(s) */
			{
				pfree(tuple);
				tuple = newtuple;
			}
		}

		if (!skip_tuple)
		{
			/* ----------------
			 * Check the constraints of a tuple
			 * ----------------
			 */

			if (rel->rd_att->constr)
				ExecConstraints("CopyFrom", rel, tuple, estate);

			heap_insert(rel, tuple);

			if (has_index)
			{
				for (i = 0; i < n_indices; i++)
				{
					if (indexPred[i] != NULL)
					{
#ifndef OMIT_PARTIAL_INDEX

						/*
						 * if tuple doesn't satisfy predicate, don't
						 * update index
						 */
						slot->val = tuple;
						/* SetSlotContents(slot, tuple); */
						if (ExecQual((List *) indexPred[i], econtext) == false)
							continue;
#endif	 /* OMIT_PARTIAL_INDEX */
					}
					FormIndexDatum(indexNatts[i],
								(AttrNumber *) &(pgIndexP[i]->indkey[0]),
								   tuple,
								   tupDesc,
								   idatum,
								   index_nulls,
								   finfoP[i]);
					indexRes = index_insert(index_rels[i], idatum, index_nulls,
											&(tuple->t_self), rel);
					if (indexRes)
						pfree(indexRes);
				}
			}
			/* AFTER ROW INSERT Triggers */
			if (rel->trigdesc &&
				rel->trigdesc->n_after_row[TRIGGER_EVENT_INSERT] > 0)
				ExecARInsertTriggers(rel, tuple);
		}

		if (binary)
			pfree(string);

		for (i = 0; i < attr_count; i++)
		{
			if (!byval[i] && nulls[i] != 'n')
			{
				if (!binary)
					pfree((void *) values[i]);
			}
			else if (nulls[i] == 'n')
				nulls[i] = ' ';
		}

		pfree(tuple);
		tuples_read++;

		if (!reading_to_eof && ntuples == tuples_read)
			done = true;
	}
	pfree(values);
	pfree(nulls);
	pfree(index_nulls);
	pfree(idatum);
	pfree(byval);

	if (!binary)
	{
		pfree(in_functions);
		pfree(elements);
		pfree(typmod);
	}

	if (has_index)
	{
		for (i = 0; i < n_indices; i++)
		{
			if (index_rels[i] == NULL)
				continue;
			/* see comments in ExecOpenIndices() in execUtils.c */
			if ((index_rels[i])->rd_rel->relam != BTREE_AM_OID &&
				(index_rels[i])->rd_rel->relam != HASH_AM_OID)
				UnlockRelation(index_rels[i], AccessExclusiveLock);
			index_close(index_rels[i]);
		}
	}
}



static Oid
GetOutputFunction(Oid type)
{
	HeapTuple	typeTuple;

	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(type),
									0, 0, 0);

	if (HeapTupleIsValid(typeTuple))
		return (int) ((Form_pg_type) GETSTRUCT(typeTuple))->typoutput;

	elog(ERROR, "GetOutputFunction: Cache lookup of type %u failed", type);
	return InvalidOid;
}

static Oid
GetTypeElement(Oid type)
{
	HeapTuple	typeTuple;

	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(type),
									0, 0, 0);

	if (HeapTupleIsValid(typeTuple))
		return (int) ((Form_pg_type) GETSTRUCT(typeTuple))->typelem;

	elog(ERROR, "GetOutputFunction: Cache lookup of type %d failed", type);
	return InvalidOid;
}

static Oid
GetInputFunction(Oid type)
{
	HeapTuple	typeTuple;

	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(type),
									0, 0, 0);

	if (HeapTupleIsValid(typeTuple))
		return (int) ((Form_pg_type) GETSTRUCT(typeTuple))->typinput;

	elog(ERROR, "GetInputFunction: Cache lookup of type %u failed", type);
	return InvalidOid;
}

static Oid
IsTypeByVal(Oid type)
{
	HeapTuple	typeTuple;

	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(type),
									0, 0, 0);

	if (HeapTupleIsValid(typeTuple))
		return (int) ((Form_pg_type) GETSTRUCT(typeTuple))->typbyval;

	elog(ERROR, "GetInputFunction: Cache lookup of type %u failed", type);

	return InvalidOid;
}

/*
 * Given the OID of a relation, return an array of index relation descriptors
 * and the number of index relations.  These relation descriptors are open
 * using index_open().
 *
 * Space for the array itself is palloc'ed.
 */

typedef struct rel_list
{
	Oid			index_rel_oid;
	struct rel_list *next;
} RelationList;

static void
GetIndexRelations(Oid main_relation_oid,
				  int *n_indices,
				  Relation **index_rels)
{
	RelationList *head,
			   *scan;
	Relation	pg_index_rel;
	HeapScanDesc scandesc;
	Oid			index_relation_oid;
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	int			i;
	bool		isnull;

	pg_index_rel = heap_openr(IndexRelationName, AccessShareLock);
	scandesc = heap_beginscan(pg_index_rel, 0, SnapshotNow, 0, NULL);
	tupDesc = RelationGetDescr(pg_index_rel);

	*n_indices = 0;

	head = (RelationList *) palloc(sizeof(RelationList));
	scan = head;
	head->next = NULL;

	while (HeapTupleIsValid(tuple = heap_getnext(scandesc, 0)))
	{

		index_relation_oid = (Oid) DatumGetInt32(heap_getattr(tuple, 2,
													  tupDesc, &isnull));
		if (index_relation_oid == main_relation_oid)
		{
			scan->index_rel_oid = (Oid) DatumGetInt32(heap_getattr(tuple,
												Anum_pg_index_indexrelid,
													  tupDesc, &isnull));
			(*n_indices)++;
			scan->next = (RelationList *) palloc(sizeof(RelationList));
			scan = scan->next;
		}
	}

	heap_endscan(scandesc);
	heap_close(pg_index_rel, AccessShareLock);

	/* We cannot trust to relhasindex of the main_relation now, so... */
	if (*n_indices == 0)
		return;

	*index_rels = (Relation *) palloc(*n_indices * sizeof(Relation));

	for (i = 0, scan = head; i < *n_indices; i++, scan = scan->next)
	{
		(*index_rels)[i] = index_open(scan->index_rel_oid);
		/* see comments in ExecOpenIndices() in execUtils.c */
		if ((*index_rels)[i] != NULL &&
			((*index_rels)[i])->rd_rel->relam != BTREE_AM_OID &&
			((*index_rels)[i])->rd_rel->relam != HASH_AM_OID)
			LockRelation((*index_rels)[i], AccessExclusiveLock);
	}

	for (i = 0, scan = head; i < *n_indices + 1; i++)
	{
		scan = head->next;
		pfree(head);
		head = scan;
	}
}


/*
   returns 1 if c is in s
*/
static bool
inString(char c, char *s)
{
	if (s && c)
		return strchr(s, c) != NULL;
	return 0;
}

/*
 * Reads input from fp until an end of line is seen.
 */

static void
CopyReadNewline(FILE *fp, int *newline)
{
	if (!*newline)
	{
		elog(NOTICE, "CopyReadNewline: line %d - extra fields ignored", lineno);
		while (!CopyGetEof(fp) && (CopyGetChar(fp) != '\n'));
	}
	*newline = 0;
}

/*
 * Read the value of a single attribute.
 *
 * Result is either a palloc'd string, or NULL (if EOF or a null attribute).
 * *isnull is set true if a null attribute, else false.
 *
 * delim is the string of acceptable delimiter characters(s).
 * *newline remembers whether we've seen a newline ending this tuple.
 */

static char *
CopyReadAttribute(FILE *fp, bool *isnull, char *delim, int *newline)
{
	StringInfoData	attribute_buf;
	char		c;
#ifdef MULTIBYTE
	int			mblen;
	int			encoding;
	unsigned char s[2];
	char	   *cvt;
	int			j;

	encoding = pg_get_client_encoding();
	s[1] = 0;
#endif

	/* if last delimiter was a newline return a NULL attribute */
	if (*newline)
	{
		*isnull = (bool) true;
		return NULL;
	}

	*isnull = (bool) false;		/* set default */

	initStringInfo(&attribute_buf);

	if (CopyGetEof(fp))
		goto endOfFile;

	for (;;)
	{
		c = CopyGetChar(fp);
		if (CopyGetEof(fp))
			goto endOfFile;

		if (c == '\\')
		{
			c = CopyGetChar(fp);
			if (CopyGetEof(fp))
				goto endOfFile;
			switch (c)
			{
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
					{
						int			val;

						val = VALUE(c);
						c = CopyPeekChar(fp);
						if (ISOCTAL(c))
						{
							val = (val << 3) + VALUE(c);
							CopyDonePeek(fp, c, 1);		/* Pick up the
														 * character! */
							c = CopyPeekChar(fp);
							if (ISOCTAL(c))
							{
								CopyDonePeek(fp, c, 1); /* pick up! */
								val = (val << 3) + VALUE(c);
							}
							else
							{
								if (CopyGetEof(fp))
									goto endOfFile;
								CopyDonePeek(fp, c, 0); /* Return to stream! */
							}
						}
						else
						{
							if (CopyGetEof(fp))
								goto endOfFile;
							CopyDonePeek(fp, c, 0);		/* Return to stream! */
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
				case 'N':
					*isnull = (bool) true;
					break;
				case '.':
					c = CopyGetChar(fp);
					if (c != '\n')
						elog(ERROR, "CopyReadAttribute - end of record marker corrupted. line: %d", lineno);
					goto endOfFile;
					break;
			}
		}
		else if (c == '\n' || inString(c, delim))
		{
			if (c == '\n')
				*newline = 1;
			break;
		}
		appendStringInfoChar(&attribute_buf, c);
#ifdef MULTIBYTE
		/* get additional bytes of the char, if any */
		s[0] = c;
		mblen = pg_encoding_mblen(encoding, s);
		for (j = 1; j < mblen; j++)
		{
			c = CopyGetChar(fp);
			if (CopyGetEof(fp))
				goto endOfFile;
			appendStringInfoChar(&attribute_buf, c);
		}
#endif
	}

#ifdef MULTIBYTE
	cvt = (char *) pg_client_to_server((unsigned char *) attribute_buf.data,
									   attribute_buf.len);
	if (cvt != attribute_buf.data)
	{
		pfree(attribute_buf.data);
		return cvt;
	}
#endif
	return attribute_buf.data;

endOfFile:
	pfree(attribute_buf.data);
	return NULL;
}

static void
CopyAttributeOut(FILE *fp, char *server_string, char *delim)
{
	char	   *string;
	char		c;
#ifdef MULTIBYTE
	char	   *string_start;
	int			encoding;
	int			mblen;
	int			i;
#endif

#ifdef MULTIBYTE
	encoding = pg_get_client_encoding();
	string = (char *) pg_server_to_client((unsigned char *) server_string,
										  strlen(server_string));
	string_start = string;
#else
	string = server_string;
#endif

#ifdef MULTIBYTE
	for (; (mblen = pg_encoding_mblen(encoding, string)) &&
		 ((c = *string) != '\0'); string += mblen)
#else
	for (; (c = *string) != '\0'; string++)
#endif
	{
		if (c == delim[0] || c == '\n' || c == '\\')
			CopySendChar('\\', fp);
#ifdef MULTIBYTE
		for (i = 0; i < mblen; i++)
			CopySendChar(*(string + i), fp);
#else
		CopySendChar(c, fp);
#endif
	}

#ifdef MULTIBYTE
	if (string_start != server_string)
		pfree(string_start);	/* pfree pg_server_to_client result */
#endif
}

/*
 * Returns the number of tuples in a relation.	Unfortunately, currently
 * must do a scan of the entire relation to determine this.
 *
 * relation is expected to be an open relation descriptor.
 */
static int
CountTuples(Relation relation)
{
	HeapScanDesc scandesc;
	HeapTuple	tuple;

	int			i;

	scandesc = heap_beginscan(relation, 0, QuerySnapshot, 0, NULL);

	i = 0;
	while (HeapTupleIsValid(tuple = heap_getnext(scandesc, 0)))
		i++;
	heap_endscan(scandesc);
	return i;
}
