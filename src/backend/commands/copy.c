/*-------------------------------------------------------------------------
 *
 * copy.c
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/copy.c,v 1.161 2002/07/30 16:55:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "rewrite/rewriteHandler.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

typedef enum CopyReadResult
{
	NORMAL_ATTR,
	END_OF_LINE,
	END_OF_FILE
} CopyReadResult;

/* non-export function prototypes */
static void CopyTo(Relation rel, List *attlist, bool binary, bool oids, FILE *fp, char *delim, char *null_print);
static void CopyFrom(Relation rel, List *attlist, bool binary, bool oids, FILE *fp, char *delim, char *null_print);
static Oid	GetInputFunction(Oid type);
static Oid	GetTypeElement(Oid type);
static char *CopyReadAttribute(FILE *fp, const char *delim, CopyReadResult *result);
static void CopyAttributeOut(FILE *fp, char *string, char *delim);
static void CopyCheckAttlist(Relation rel, List *attlist);

static const char BinarySignature[12] = "PGBCOPY\n\377\r\n\0";

/*
 * Static communication variables ... pretty grotty, but COPY has
 * never been reentrant...
 */
int			copy_lineno = 0;	/* exported for use by elog() -- dz */
static bool fe_eof;

/*
 * These static variables are used to avoid incurring overhead for each
 * attribute processed.  attribute_buf is reused on each CopyReadAttribute
 * call to hold the string being read in.  Under normal use it will soon
 * grow to a suitable size, and then we will avoid palloc/pfree overhead
 * for subsequent attributes.  Note that CopyReadAttribute returns a pointer
 * to attribute_buf's data buffer!
 * encoding, if needed, can be set once at the start of the copy operation.
 */
static StringInfoData attribute_buf;

#ifdef MULTIBYTE
static int	client_encoding;
static int	server_encoding;
#endif


/*
 * Internal communications functions
 */
static void CopySendData(void *databuf, int datasize, FILE *fp);
static void CopySendString(const char *str, FILE *fp);
static void CopySendChar(char c, FILE *fp);
static void CopyGetData(void *databuf, int datasize, FILE *fp);
static int	CopyGetChar(FILE *fp);
static int	CopyGetEof(FILE *fp);
static int	CopyPeekChar(FILE *fp);
static void CopyDonePeek(FILE *fp, int c, bool pickup);

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
	{
		fwrite(databuf, datasize, 1, fp);
		if (ferror(fp))
			elog(ERROR, "CopySendData: %m");
	}
}

static void
CopySendString(const char *str, FILE *fp)
{
	CopySendData((void *) str, strlen(str), fp);
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
 * CopyGetEof checks if it's EOF on the input (or, check for EOF result
 *		from CopyGetChar)
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
		int			ch = pq_getbyte();

		if (ch == EOF)
			fe_eof = true;
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
 *
 * after each call to CopyPeekChar, a call to CopyDonePeek _must_
 * follow, unless EOF was returned.
 *
 * CopyDonePeek will either take the peeked char off the stream
 * (if pickup is true) or leave it on the stream (if pickup is false).
 */
static int
CopyPeekChar(FILE *fp)
{
	if (!fp)
	{
		int			ch = pq_peekbyte();

		if (ch == EOF)
			fe_eof = true;
		return ch;
	}
	else
		return getc(fp);
}

static void
CopyDonePeek(FILE *fp, int c, bool pickup)
{
	if (!fp)
	{
		if (pickup)
		{
			/* We want to pick it up */
			(void) pq_getbyte();
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
		/* If we wanted to pick it up, it's already done */
	}
}



/*
 *	 DoCopy executes the SQL COPY statement.
 *
 * Either unload or reload contents of table <relation>, depending on <from>.
 * (<from> = TRUE means we are inserting into the table.)
 *
 * If <pipe> is false, transfer is between the table and the file named
 * <filename>.	Otherwise, transfer is between the table and our regular
 * input/output stream. The latter could be either stdin/stdout or a
 * socket, depending on whether we're running under Postmaster control.
 *
 * Iff <binary>, unload or reload in the binary format, as opposed to the
 * more wasteful but more robust and portable text format.
 *
 * Iff <oids>, unload or reload the format that includes OID information.
 * On input, we accept OIDs whether or not the table has an OID column,
 * but silently drop them if it does not.  On output, we report an error
 * if the user asks for OIDs in a table that has none (not providing an
 * OID column might seem friendlier, but could seriously confuse programs).
 *
 * If in the text format, delimit columns with delimiter <delim> and print
 * NULL values as <null_print>.
 *
 * When loading in the text format from an input stream (as opposed to
 * a file), recognize a "." on a line by itself as EOF. Also recognize
 * a stream EOF.  When unloading in the text format to an output stream,
 * write a "." on a line by itself at the end of the data.
 *
 * Do not allow a Postgres user without superuser privilege to read from
 * or write to a file.
 *
 * Do not allow the copy if user doesn't have proper permission to access
 * the table.
 */
void
DoCopy(const CopyStmt *stmt)
{
	RangeVar *relation = stmt->relation;
	char *filename = stmt->filename;
	bool is_from = stmt->is_from;
	bool pipe = (stmt->filename == NULL);
	List *option;
	List *attlist = stmt->attlist;
	bool binary = false;
	bool oids = false;
	char *delim = NULL;
	char *null_print = NULL;
	FILE	   *fp;
	Relation	rel;
	AclMode		required_access = (is_from ? ACL_INSERT : ACL_SELECT);
	AclResult	aclresult;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		/* XXX: Should we bother checking for doubled options? */

		if (strcmp(defel->defname, "binary") == 0)
		{
			if (binary)
				elog(ERROR, "COPY: BINARY option appears more than once");

			binary = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "oids") == 0)
		{
			if (oids)
				elog(ERROR, "COPY: OIDS option appears more than once");

			oids = intVal(defel->arg);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (delim)
				elog(ERROR, "COPY: DELIMITER string may only be defined once in query");

			delim = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (null_print)
				elog(ERROR, "COPY: NULL representation may only be defined once in query");

			null_print = strVal(defel->arg);
		}
		else
			elog(ERROR, "COPY: option \"%s\" not recognized",
				 defel->defname);
	}

	if (binary && delim)
		elog(ERROR, "You can not specify the DELIMITER in BINARY mode.");

	if (binary && null_print)
		elog(ERROR, "You can not specify NULL in BINARY mode.");

	/* Set defaults */
	if (!delim)
		delim = "\t";

	if (!null_print)
		null_print = "\\N";
	
	/*
	 * Open and lock the relation, using the appropriate lock type.
	 */
	rel = heap_openrv(relation, (is_from ? RowExclusiveLock : AccessShareLock));

	/* Check permissions. */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  required_access);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, RelationGetRelationName(rel));
	if (!pipe && !superuser())
		elog(ERROR, "You must have Postgres superuser privilege to do a COPY "
			 "directly to or from a file.  Anyone can COPY to stdout or "
			 "from stdin.  Psql's \\copy command also works for anyone.");

	/*
	 * This restriction is unfortunate, but necessary until the frontend
	 * COPY protocol is redesigned to be binary-safe...
	 */
	if (pipe && binary)
		elog(ERROR, "COPY BINARY is not supported to stdout or from stdin");

	/*
	 * Presently, only single-character delimiter strings are supported.
	 */
	if (strlen(delim) != 1)
		elog(ERROR, "COPY delimiter must be a single character");

	/*
	 * Don't allow COPY w/ OIDs to or from a table without them
	 */
	if (oids && !rel->rd_rel->relhasoids)
		elog(ERROR, "COPY: table \"%s\" does not have OIDs",
			 RelationGetRelationName(rel));

	/*
	 * Set up variables to avoid per-attribute overhead.
	 */
	initStringInfo(&attribute_buf);
#ifdef MULTIBYTE
	client_encoding = pg_get_client_encoding();
	server_encoding = GetDatabaseEncoding();
#endif

	if (attlist == NIL)
	{
		/* get list of attributes in the relation */
 		TupleDesc desc = RelationGetDescr(rel);
 		int i;
 		for (i = 0; i < desc->natts; ++i)
		{
 			Ident *id = makeNode(Ident);
 			id->name = NameStr(desc->attrs[i]->attname);
 			attlist = lappend(attlist,id);
 		}
	}
	else
	{
		if (binary)
			elog(ERROR,"COPY: BINARY format cannot be used with specific column list");

		CopyCheckAttlist(rel, attlist);
	}
	
	if (is_from)
	{							/* copy from file to database */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				elog(ERROR, "You cannot copy view %s",
					 RelationGetRelationName(rel));
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				elog(ERROR, "You cannot change sequence relation %s",
					 RelationGetRelationName(rel));
			else
				elog(ERROR, "You cannot copy object %s",
					 RelationGetRelationName(rel));
		}
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
			struct stat st;

			fp = AllocateFile(filename, PG_BINARY_R);

			if (fp == NULL)
				elog(ERROR, "COPY command, running in backend with "
					 "effective uid %d, could not open file '%s' for "
					 "reading.  Errno = %s (%d).",
					 (int) geteuid(), filename, strerror(errno), errno);

			fstat(fileno(fp), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(fp);
				elog(ERROR, "COPY: %s is a directory.", filename);
			}
		}
		CopyFrom(rel, attlist, binary, oids, fp, delim, null_print);
	}
	else
	{							/* copy from database to file */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				elog(ERROR, "You cannot copy view %s",
					 RelationGetRelationName(rel));
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				elog(ERROR, "You cannot copy sequence %s",
					 RelationGetRelationName(rel));
			else
				elog(ERROR, "You cannot copy object %s",
					 RelationGetRelationName(rel));
		}
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
			mode_t		oumask; /* Pre-existing umask value */
			struct stat st;

			/*
			 * Prevent write to relative path ... too easy to shoot
			 * oneself in the foot by overwriting a database file ...
			 */
			if (filename[0] != '/')
				elog(ERROR, "Relative path not allowed for server side"
					 " COPY command.");

			oumask = umask((mode_t) 022);
			fp = AllocateFile(filename, PG_BINARY_W);
			umask(oumask);

			if (fp == NULL)
				elog(ERROR, "COPY command, running in backend with "
					 "effective uid %d, could not open file '%s' for "
					 "writing.  Errno = %s (%d).",
					 (int) geteuid(), filename, strerror(errno), errno);
			fstat(fileno(fp), &st);
			if (S_ISDIR(st.st_mode))
			{
				FreeFile(fp);
				elog(ERROR, "COPY: %s is a directory.", filename);
			}
		}
		CopyTo(rel, attlist, binary, oids, fp, delim, null_print);
	}

	if (!pipe)
		FreeFile(fp);
	else if (!is_from)
	{
		if (!binary)
			CopySendData("\\.\n", 3, fp);
		if (IsUnderPostmaster)
			pq_endcopyout(false);
	}
	pfree(attribute_buf.data);

	/*
	 * Close the relation.	If reading, we can release the AccessShareLock
	 * we got; if writing, we should hold the lock until end of
	 * transaction to ensure that updates will be committed before lock is
	 * released.
	 */
	heap_close(rel, (is_from ? NoLock : AccessShareLock));
}


/*
 * Copy from relation TO file.
 */
static void
CopyTo(Relation rel, List *attlist, bool binary, bool oids, 
		 FILE *fp, char *delim, char *null_print)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	HeapScanDesc scandesc;
	int			attr_count,
				i;
	Form_pg_attribute *attr;
	FmgrInfo   *out_functions;
	Oid		   *elements;
	bool	   *isvarlena;
	int16		fld_size;
	char	   *string;
	Snapshot	mySnapshot;
	int			copy_attr_count;
	int		   *attmap;
	int			p = 0;
	List	   *cur;

	tupDesc = rel->rd_att;
	attr_count = rel->rd_att->natts;
	attr = rel->rd_att->attrs;

	copy_attr_count = length(attlist);
	attmap = (int *) palloc(copy_attr_count * sizeof(int));

	foreach(cur, attlist)
	{
		const char *currAtt = strVal(lfirst(cur));

		for (i = 0; i < attr_count; i++)
		{
			if (namestrcmp(&attr[i]->attname, currAtt) == 0)
			{
				attmap[p++] = i;
				continue;
			}
		}
	}

	/*
	 * For binary copy we really only need isvarlena, but compute it
	 * all...
	 */
	out_functions = (FmgrInfo *) palloc(attr_count * sizeof(FmgrInfo));
	elements = (Oid *) palloc(attr_count * sizeof(Oid));
	isvarlena = (bool *) palloc(attr_count * sizeof(bool));
	for (i = 0; i < attr_count; i++)
	{
		Oid			out_func_oid;

		if (!getTypeOutputInfo(attr[i]->atttypid,
							 &out_func_oid, &elements[i], &isvarlena[i]))
			elog(ERROR, "COPY: couldn't lookup info for type %u",
				 attr[i]->atttypid);
		fmgr_info(out_func_oid, &out_functions[i]);
	}

	if (binary)
	{
		/* Generate header for a binary copy */
		int32		tmp;

		/* Signature */
		CopySendData((char *) BinarySignature, 12, fp);
		/* Integer layout field */
		tmp = 0x01020304;
		CopySendData(&tmp, sizeof(int32), fp);
		/* Flags field */
		tmp = 0;
		if (oids)
			tmp |= (1 << 16);
		CopySendData(&tmp, sizeof(int32), fp);
		/* No header extension */
		tmp = 0;
		CopySendData(&tmp, sizeof(int32), fp);
	}

	mySnapshot = CopyQuerySnapshot();

	scandesc = heap_beginscan(rel, mySnapshot, 0, NULL);

	while ((tuple = heap_getnext(scandesc, ForwardScanDirection)) != NULL)
	{
		bool		need_delim = false;

		CHECK_FOR_INTERRUPTS();

		if (binary)
		{
			/* Binary per-tuple header */
			int16		fld_count = attr_count;

			CopySendData(&fld_count, sizeof(int16), fp);
			/* Send OID if wanted --- note fld_count doesn't include it */
			if (oids)
			{
				Oid oid;
				
				AssertTupleDescHasOid(tupDesc);
				oid = HeapTupleGetOid(tuple);
				fld_size = sizeof(Oid);
				CopySendData(&fld_size, sizeof(int16), fp);
				CopySendData(&oid, sizeof(Oid), fp);
			}
		}
		else
		{
			/* Text format has no per-tuple header, but send OID if wanted */
			if (oids)
			{
				AssertTupleDescHasOid(tupDesc);
				string = DatumGetCString(DirectFunctionCall1(oidout,
								ObjectIdGetDatum(HeapTupleGetOid(tuple))));
				CopySendString(string, fp);
				pfree(string);
				need_delim = true;
			}
		}

		for (i = 0; i < copy_attr_count; i++)
		{
			Datum		origvalue,
						value;
			bool		isnull;
			int			mi = attmap[i];

			origvalue = heap_getattr(tuple, mi + 1, tupDesc, &isnull);

			if (!binary)
			{
				if (need_delim)
					CopySendChar(delim[0], fp);
				need_delim = true;
			}

			if (isnull)
			{
				if (!binary)
				{
					CopySendString(null_print, fp);		/* null indicator */
				}
				else
				{
					fld_size = 0;		/* null marker */
					CopySendData(&fld_size, sizeof(int16), fp);
				}
			}
			else
			{
				/*
				 * If we have a toasted datum, forcibly detoast it to
				 * avoid memory leakage inside the type's output routine
				 * (or for binary case, becase we must output untoasted
				 * value).
				 */
				if (isvarlena[mi])
					value = PointerGetDatum(PG_DETOAST_DATUM(origvalue));
				else
					value = origvalue;

				if (!binary)
				{
					string = DatumGetCString(FunctionCall3(&out_functions[mi],
														   value,
										   ObjectIdGetDatum(elements[mi]),
									 Int32GetDatum(attr[mi]->atttypmod)));
					CopyAttributeOut(fp, string, delim);
					pfree(string);
				}
				else
				{
					fld_size = attr[mi]->attlen;
					CopySendData(&fld_size, sizeof(int16), fp);
					if (isvarlena[mi])
					{
						/* varlena */
						Assert(fld_size == -1);
						CopySendData(DatumGetPointer(value),
									 VARSIZE(value),
									 fp);
					}
					else if (!attr[mi]->attbyval)
					{
						/* fixed-length pass-by-reference */
						Assert(fld_size > 0);
						CopySendData(DatumGetPointer(value),
									 fld_size,
									 fp);
					}
					else
					{
						/* pass-by-value */
						Datum		datumBuf;

						/*
						 * We need this horsing around because we don't
						 * know how shorter data values are aligned within
						 * a Datum.
						 */
						store_att_byval(&datumBuf, value, fld_size);
						CopySendData(&datumBuf,
									 fld_size,
									 fp);
					}
				}

				/* Clean up detoasted copy, if any */
				if (value != origvalue)
					pfree(DatumGetPointer(value));
			}
		}

		if (!binary)
			CopySendChar('\n', fp);
	}

	heap_endscan(scandesc);

	if (binary)
	{
		/* Generate trailer for a binary copy */
		int16		fld_count = -1;

		CopySendData(&fld_count, sizeof(int16), fp);
	}

	pfree(out_functions);
	pfree(elements);
	pfree(isvarlena);
}


/*
 * Copy FROM file to relation.
 */
static void
CopyFrom(Relation rel, List *attlist, bool binary, bool oids, 
		 FILE *fp, char *delim, char *null_print)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Form_pg_attribute *attr;
	AttrNumber	attr_count, copy_attr_count, def_attr_count;
	FmgrInfo   *in_functions;
	Oid		   *elements;
	int			i;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls;
	int			done = 0;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	TupleTable	tupleTable;
	TupleTableSlot *slot;
	bool		file_has_oids;
	int		   *attmap = NULL;
	int		   *defmap = NULL;
	Node	  **defexprs = NULL; /* array of default att expressions */
	ExprContext *econtext; /* used for ExecEvalExpr for default atts */
	ExprDoneCond isdone;

	tupDesc = RelationGetDescr(rel);
	attr = tupDesc->attrs;
	attr_count = tupDesc->natts;
	copy_attr_count = length(attlist);
	def_attr_count = 0;

	/*
	 * We need a ResultRelInfo so we can use the regular executor's
	 * index-entry-making machinery.  (There used to be a huge amount of
	 * code here that basically duplicated execUtils.c ...)
	 */
	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = rel;
	resultRelInfo->ri_TrigDesc = rel->trigdesc;

	ExecOpenIndices(resultRelInfo);

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	/* Set up a dummy tuple table too */
	tupleTable = ExecCreateTupleTable(1);
	slot = ExecAllocTableSlot(tupleTable);
	ExecSetSlotDescriptor(slot, tupDesc, false);

	if (!binary)
	{
		/*
		 * pick up the input function and default expression (if any) for 
		 * each attribute in the relation.
		 */
		attmap = (int *) palloc(sizeof(int) * attr_count);
		defmap = (int *) palloc(sizeof(int) * attr_count);
		defexprs = (Node **) palloc(sizeof(Node *) * attr_count);
		in_functions = (FmgrInfo *) palloc(attr_count * sizeof(FmgrInfo));
		elements = (Oid *) palloc(attr_count * sizeof(Oid));
		for (i = 0; i < attr_count; i++)
		{
			List	*l;
			int		 p = 0;
			bool	 specified = false;

			in_func_oid = (Oid) GetInputFunction(attr[i]->atttypid);
			fmgr_info(in_func_oid, &in_functions[i]);
			elements[i] = GetTypeElement(attr[i]->atttypid);

			foreach(l, attlist)
			{
				if (namestrcmp(&attr[i]->attname, strVal(lfirst(l))) == 0)
				{
					attmap[p] = i;
					specified = true;
					continue;
				}
				p++;
			}

			/* if column not specified, use default value if one exists */
			if (! specified)
			{
				defexprs[def_attr_count] = build_column_default(rel, i + 1);

				if (defexprs[def_attr_count] != NULL)
				{
					defmap[def_attr_count] = i;
					def_attr_count++;
				}
			}
		}
		file_has_oids = oids;	/* must rely on user to tell us this... */
	}
	else
	{
		/* Read and verify binary header */
		char		readSig[12];
		int32		tmp;

		/* Signature */
		CopyGetData(readSig, 12, fp);
		if (CopyGetEof(fp) || memcmp(readSig, BinarySignature, 12) != 0)
			elog(ERROR, "COPY BINARY: file signature not recognized");
		/* Integer layout field */
		CopyGetData(&tmp, sizeof(int32), fp);
		if (CopyGetEof(fp) || tmp != 0x01020304)
			elog(ERROR, "COPY BINARY: incompatible integer layout");
		/* Flags field */
		CopyGetData(&tmp, sizeof(int32), fp);
		if (CopyGetEof(fp))
			elog(ERROR, "COPY BINARY: bogus file header (missing flags)");
		file_has_oids = (tmp & (1 << 16)) != 0;
		tmp &= ~(1 << 16);
		if ((tmp >> 16) != 0)
			elog(ERROR, "COPY BINARY: unrecognized critical flags in header");
		/* Header extension length */
		CopyGetData(&tmp, sizeof(int32), fp);
		if (CopyGetEof(fp) || tmp < 0)
			elog(ERROR, "COPY BINARY: bogus file header (missing length)");
		/* Skip extension header, if present */
		while (tmp-- > 0)
		{
			CopyGetData(readSig, 1, fp);
			if (CopyGetEof(fp))
				elog(ERROR, "COPY BINARY: bogus file header (wrong length)");
		}

		in_functions = NULL;
		elements = NULL;
	}

	values = (Datum *) palloc(attr_count * sizeof(Datum));
	nulls = (char *) palloc(attr_count * sizeof(char));

	copy_lineno = 0;
	fe_eof = false;

	econtext = GetPerTupleExprContext(estate);

	while (!done)
	{
		bool	skip_tuple;
		Oid		loaded_oid;

		CHECK_FOR_INTERRUPTS();

		copy_lineno++;
		
		/* Reset the per-output-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/* Initialize all values for row to NULL */
		MemSet(values, 0, attr_count * sizeof(Datum));
		MemSet(nulls, 'n', attr_count * sizeof(char));

		if (!binary)
		{
			CopyReadResult	 result;
			char			*string;

			if (file_has_oids)
			{
				string = CopyReadAttribute(fp, delim, &result);

				if (result == END_OF_FILE)
					done = 1;
				else if (string == NULL || strcmp(string, null_print) == 0)
					elog(ERROR, "COPY TEXT: NULL Oid");
				else
				{
					loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
											   CStringGetDatum(string)));
					if (loaded_oid == InvalidOid)
						elog(ERROR, "COPY TEXT: Invalid Oid");
				}
			}
      
			/* 
			 * here, we only try to read as many attributes as 
			 * were specified.
			 */
			for (i = 0; i < copy_attr_count && !done; i++)
			{
				int m = attmap[i];

				string = CopyReadAttribute(fp, delim, &result);

				/* If we got an end-of-line before we expected, bail out */
				if (result == END_OF_LINE && i < (copy_attr_count - 1))
					elog(ERROR, "COPY TEXT: Missing data for attribute %d", i + 1);

				if (result == END_OF_FILE)
					done = 1;
				else if (strcmp(string, null_print) == 0)
				{
					/* we read an SQL NULL, no need to do anything */
				}
				else
				{
					values[m] = FunctionCall3(&in_functions[m],
											  CStringGetDatum(string),
										   	  ObjectIdGetDatum(elements[m]),
									  		  Int32GetDatum(attr[m]->atttypmod));
					nulls[m] = ' ';
				}
			}

			if (result == NORMAL_ATTR && !done)
				elog(ERROR, "COPY TEXT: Extra data encountered");

			/*
			 * as above, we only try a default lookup if one is
			 * known to be available
			 */
			for (i = 0; i < def_attr_count && !done; i++)
			{
				bool isnull;
				values[defmap[i]] = ExecEvalExpr(defexprs[i], econtext,
												 &isnull, &isdone);

				if (! isnull)
					nulls[defmap[i]] = ' ';
			}
		}
		else
		{						/* binary */
			int16		fld_count,
						fld_size;

			CopyGetData(&fld_count, sizeof(int16), fp);
			if (CopyGetEof(fp) || fld_count == -1)
				done = 1;
			else
			{
				if (fld_count <= 0 || fld_count > attr_count)
					elog(ERROR, "COPY BINARY: tuple field count is %d, expected %d",
						 (int) fld_count, attr_count);

				if (file_has_oids)
				{
					CopyGetData(&fld_size, sizeof(int16), fp);
					if (CopyGetEof(fp))
						elog(ERROR, "COPY BINARY: unexpected EOF");
					if (fld_size != (int16) sizeof(Oid))
						elog(ERROR, "COPY BINARY: sizeof(Oid) is %d, expected %d",
							 (int) fld_size, (int) sizeof(Oid));
					CopyGetData(&loaded_oid, sizeof(Oid), fp);
					if (CopyGetEof(fp))
						elog(ERROR, "COPY BINARY: unexpected EOF");
					if (loaded_oid == InvalidOid)
						elog(ERROR, "COPY BINARY: Invalid Oid");
				}

				for (i = 0; i < (int) fld_count; i++)
				{
					CopyGetData(&fld_size, sizeof(int16), fp);
					if (CopyGetEof(fp))
						elog(ERROR, "COPY BINARY: unexpected EOF");
					if (fld_size == 0)
						continue;		/* it's NULL; nulls[i] already set */
					if (fld_size != attr[i]->attlen)
						elog(ERROR, "COPY BINARY: sizeof(field %d) is %d, expected %d",
						   i + 1, (int) fld_size, (int) attr[i]->attlen);
					if (fld_size == -1)
					{
						/* varlena field */
						int32		varlena_size;
						Pointer		varlena_ptr;

						CopyGetData(&varlena_size, sizeof(int32), fp);
						if (CopyGetEof(fp))
							elog(ERROR, "COPY BINARY: unexpected EOF");
						if (varlena_size < (int32) sizeof(int32))
							elog(ERROR, "COPY BINARY: bogus varlena length");
						varlena_ptr = (Pointer) palloc(varlena_size);
						VARATT_SIZEP(varlena_ptr) = varlena_size;
						CopyGetData(VARDATA(varlena_ptr),
									varlena_size - sizeof(int32),
									fp);
						if (CopyGetEof(fp))
							elog(ERROR, "COPY BINARY: unexpected EOF");
						values[i] = PointerGetDatum(varlena_ptr);
					}
					else if (!attr[i]->attbyval)
					{
						/* fixed-length pass-by-reference */
						Pointer		refval_ptr;

						Assert(fld_size > 0);
						refval_ptr = (Pointer) palloc(fld_size);
						CopyGetData(refval_ptr, fld_size, fp);
						if (CopyGetEof(fp))
							elog(ERROR, "COPY BINARY: unexpected EOF");
						values[i] = PointerGetDatum(refval_ptr);
					}
					else
					{
						/* pass-by-value */
						Datum		datumBuf;

						/*
						 * We need this horsing around because we don't
						 * know how shorter data values are aligned within
						 * a Datum.
						 */
						Assert(fld_size > 0 && fld_size <= sizeof(Datum));
						CopyGetData(&datumBuf, fld_size, fp);
						if (CopyGetEof(fp))
							elog(ERROR, "COPY BINARY: unexpected EOF");
						values[i] = fetch_att(&datumBuf, true, fld_size);
					}

					nulls[i] = ' ';
				}
			}
		}

		if (done)
			break;

		tuple = heap_formtuple(tupDesc, values, nulls);
  	
		if (oids && file_has_oids)
			HeapTupleSetOid(tuple, loaded_oid);

		skip_tuple = false;

		/* BEFORE ROW INSERT Triggers */
		if (resultRelInfo->ri_TrigDesc &&
			resultRelInfo->ri_TrigDesc->n_before_row[TRIGGER_EVENT_INSERT] > 0)
		{
			HeapTuple	newtuple;

			newtuple = ExecBRInsertTriggers(estate, resultRelInfo, tuple);

			if (newtuple == NULL)		/* "do nothing" */
				skip_tuple = true;
			else if (newtuple != tuple) /* modified by Trigger(s) */
			{
				heap_freetuple(tuple);
				tuple = newtuple;
			}
		}

		if (!skip_tuple)
		{
			ExecStoreTuple(tuple, slot, InvalidBuffer, false);

			/*
			 * Check the constraints of the tuple
			 */
			if (rel->rd_att->constr)
				ExecConstraints("CopyFrom", resultRelInfo, slot, estate);

			/*
			 * OK, store the tuple and create index entries for it
			 */
			simple_heap_insert(rel, tuple);

			if (resultRelInfo->ri_NumIndices > 0)
				ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

			/* AFTER ROW INSERT Triggers */
			if (resultRelInfo->ri_TrigDesc)
				ExecARInsertTriggers(estate, resultRelInfo, tuple);
		}

		heap_freetuple(tuple);
	}

	/*
	 * Done, clean up
	 */
	copy_lineno = 0;

	pfree(values);
	pfree(nulls);

	if (!binary)
	{
		pfree(in_functions);
		pfree(elements);
	}

	ExecDropTupleTable(tupleTable, true);

	ExecCloseIndices(resultRelInfo);
}


static Oid
GetInputFunction(Oid type)
{
	HeapTuple	typeTuple;
	Oid			result;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "GetInputFunction: Cache lookup of type %u failed", type);
	result = ((Form_pg_type) GETSTRUCT(typeTuple))->typinput;
	ReleaseSysCache(typeTuple);
	return result;
}

static Oid
GetTypeElement(Oid type)
{
	HeapTuple	typeTuple;
	Oid			result;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "GetTypeElement: Cache lookup of type %u failed", type);
	result = ((Form_pg_type) GETSTRUCT(typeTuple))->typelem;
	ReleaseSysCache(typeTuple);
	return result;
}

/*
 * Read the value of a single attribute.
 *
 * Results are returned in the status indicator, as well as the
 * return value. If a value was successfully read but there is
 * more to read before EOL, NORMAL_ATTR is set and the value read
 * is returned. If a value was read and we hit EOL, END_OF_LINE
 * is set and the value read is returned. If we hit the EOF,
 * END_OF_FILE is set and NULL is returned.
 *
 * Note: This function does not care about SQL NULL values -- it
 * is the caller's responsibility to check if the returned string
 * matches what the user specified for the SQL NULL value.
 *
 * delim is the column delimiter string.
 */

static char *
CopyReadAttribute(FILE *fp, const char *delim, CopyReadResult *result)
{
	int			c;
	int			delimc = (unsigned char)delim[0];

#ifdef MULTIBYTE
	int			mblen;
	unsigned char s[2];
	char	   *cvt;
	int			j;

	s[1] = 0;
#endif

	/* reset attribute_buf to empty */
	attribute_buf.len = 0;
	attribute_buf.data[0] = '\0';

	/* set default */
	*result = NORMAL_ATTR;

	for (;;)
	{
		c = CopyGetChar(fp);
		if (c == EOF)
		{
			*result = END_OF_FILE;
			return NULL;
		}
		if (c == '\n')
		{
			*result = END_OF_LINE;
			break;
		}
		if (c == delimc)
			break;
		if (c == '\\')
		{
			c = CopyGetChar(fp);
			if (c == EOF)
			{
				*result = END_OF_FILE;
				return NULL;
			}
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

						val = OCTVALUE(c);
						c = CopyPeekChar(fp);
						if (ISOCTAL(c))
						{
							val = (val << 3) + OCTVALUE(c);
							CopyDonePeek(fp, c, true /* pick up */ );
							c = CopyPeekChar(fp);
							if (ISOCTAL(c))
							{
								val = (val << 3) + OCTVALUE(c);
								CopyDonePeek(fp, c, true /* pick up */ );
							}
							else
							{
								if (c == EOF)
								{
									*result = END_OF_FILE;
									return NULL;
								}
								CopyDonePeek(fp, c, false /* put back */ );
							}
						}
						else
						{
							if (c == EOF)
							{
								*result = END_OF_FILE;
								return NULL;
							}
							CopyDonePeek(fp, c, false /* put back */ );
						}
						c = val & 0377;
					}
					break;

					/*
					 * This is a special hack to parse `\N' as
					 * <backslash-N> rather then just 'N' to provide
					 * compatibility with the default NULL output. -- pe
					 */
				case 'N':
					appendStringInfoCharMacro(&attribute_buf, '\\');
					c = 'N';
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
				case '.':
					c = CopyGetChar(fp);
					if (c != '\n')
						elog(ERROR, "CopyReadAttribute: end of record marker corrupted");
					*result = END_OF_FILE;
					return NULL;
			}
		}
		appendStringInfoCharMacro(&attribute_buf, c);
#ifdef MULTIBYTE
		/* XXX shouldn't this be done even when encoding is the same? */
		if (client_encoding != server_encoding)
		{
			/* get additional bytes of the char, if any */
			s[0] = c;
			mblen = pg_encoding_mblen(client_encoding, s);
			for (j = 1; j < mblen; j++)
			{
				c = CopyGetChar(fp);
				if (c == EOF)
				{
					*result = END_OF_FILE;
					return NULL;
				}
				appendStringInfoCharMacro(&attribute_buf, c);
			}
		}
#endif
	}

#ifdef MULTIBYTE
	if (client_encoding != server_encoding)
	{
		cvt = (char *) pg_client_to_server((unsigned char *) attribute_buf.data,
										   attribute_buf.len);
		if (cvt != attribute_buf.data)
		{
			/* transfer converted data back to attribute_buf */
			attribute_buf.len = 0;
			attribute_buf.data[0] = '\0';
			appendBinaryStringInfo(&attribute_buf, cvt, strlen(cvt));
			pfree(cvt);
		}
	}
#endif

	return attribute_buf.data;
}

static void
CopyAttributeOut(FILE *fp, char *server_string, char *delim)
{
	char	   *string;
	char		c;
	char		delimc = delim[0];

#ifdef MULTIBYTE
	bool		same_encoding;
	char	   *string_start;
	int			mblen;
	int			i;
#endif

#ifdef MULTIBYTE
	same_encoding = (server_encoding == client_encoding);
	if (!same_encoding)
	{
		string = (char *) pg_server_to_client((unsigned char *) server_string,
											  strlen(server_string));
		string_start = string;
	}
	else
	{
		string = server_string;
		string_start = NULL;
	}
#else
	string = server_string;
#endif

#ifdef MULTIBYTE
	for (; (c = *string) != '\0'; string += mblen)
#else
	for (; (c = *string) != '\0'; string++)
#endif
	{
#ifdef MULTIBYTE
		mblen = 1;
#endif
		switch (c)
		{
			case '\b':
				CopySendString("\\b", fp);
				break;
			case '\f':
				CopySendString("\\f", fp);
				break;
			case '\n':
				CopySendString("\\n", fp);
				break;
			case '\r':
				CopySendString("\\r", fp);
				break;
			case '\t':
				CopySendString("\\t", fp);
				break;
			case '\v':
				CopySendString("\\v", fp);
				break;
			case '\\':
				CopySendString("\\\\", fp);
				break;
			default:
				if (c == delimc)
					CopySendChar('\\', fp);
				CopySendChar(c, fp);
#ifdef MULTIBYTE
				/* XXX shouldn't this be done even when encoding is same? */
				if (!same_encoding)
				{
					/* send additional bytes of the char, if any */
					mblen = pg_encoding_mblen(client_encoding, string);
					for (i = 1; i < mblen; i++)
						CopySendChar(string[i], fp);
				}
#endif
				break;
		}
	}

#ifdef MULTIBYTE
	if (string_start)
		pfree(string_start);	/* pfree pg_server_to_client result */
#endif
}

/*
 * CopyCheckAttlist: elog(ERROR,...) if the specified attlist
 *                    is not valid for the Relation
 */
static void
CopyCheckAttlist(Relation rel, List *attlist)
{
	TupleDesc	 tupDesc;
	int			 attr_count;
	List		*l;

	if (attlist == NIL)
		return;

	tupDesc = RelationGetDescr(rel);
	Assert(tupDesc != NULL);
 
	/*
	 * make sure there aren't more columns specified than are in the table 
	 */
	attr_count = tupDesc->natts;
	if (attr_count < length(attlist))
		elog(ERROR, "COPY: Too many columns specified");

	/* 
	 * make sure no columns are specified that don't exist in the table 
	 */
	foreach(l, attlist)
	{
		char	*colName = strVal(lfirst(l));
		bool	 found = false;
		int		 i;

		for (i = 0; i < attr_count; i++)
		{
			if (namestrcmp(&tupDesc->attrs[i]->attname, colName) == 0)
			{
				found = true;
				break;
			}
		}
		
		if (!found)
			elog(ERROR, "COPY: Specified column \"%s\" does not exist",
				 colName);
	}
}

