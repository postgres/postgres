/*-------------------------------------------------------------------------
 *
 * copy.c
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/copy.c,v 1.145.2.2 2003/04/11 20:51:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/printtup.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/executor.h"
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


/* non-export function prototypes */
static void CopyTo(Relation rel, bool binary, bool oids, FILE *fp, char *delim, char *null_print);
static void CopyFrom(Relation rel, bool binary, bool oids, FILE *fp, char *delim, char *null_print);
static Oid	GetInputFunction(Oid type);
static Oid	GetTypeElement(Oid type);
static void CopyReadNewline(FILE *fp, int *newline);
static char *CopyReadAttribute(FILE *fp, bool *isnull, char *delim, int *newline, char *null_print);
static void CopyAttributeOut(FILE *fp, char *string, char *delim);

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
 * Either unload or reload contents of table <relname>, depending on <from>.
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
DoCopy(char *relname, bool binary, bool oids, bool from, bool pipe,
	   char *filename, char *delim, char *null_print)
{
	FILE	   *fp;
	Relation	rel;
	const AclMode required_access = (from ? ACL_INSERT : ACL_SELECT);
	int			result;

	/*
	 * Open and lock the relation, using the appropriate lock type.
	 */
	rel = heap_openr(relname, (from ? RowExclusiveLock : AccessShareLock));

	result = pg_aclcheck(relname, GetUserId(), required_access);
	if (result != ACLCHECK_OK)
		elog(ERROR, "%s: %s", relname, aclcheck_error_strings[result]);
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
	 * Set up variables to avoid per-attribute overhead.
	 */
	initStringInfo(&attribute_buf);
#ifdef MULTIBYTE
	client_encoding = pg_get_client_encoding();
	server_encoding = GetDatabaseEncoding();
#endif

	if (from)
	{							/* copy from file to database */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				elog(ERROR, "You cannot copy view %s", relname);
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				elog(ERROR, "You cannot change sequence relation %s", relname);
			else
				elog(ERROR, "You cannot copy object %s", relname);
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
			fp = AllocateFile(filename, PG_BINARY_R);
			if (fp == NULL)
				elog(ERROR, "COPY command, running in backend with "
					 "effective uid %d, could not open file '%s' for "
					 "reading.  Errno = %s (%d).",
					 (int) geteuid(), filename, strerror(errno), errno);
		}
		CopyFrom(rel, binary, oids, fp, delim, null_print);
	}
	else
	{							/* copy from database to file */
		if (rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (rel->rd_rel->relkind == RELKIND_VIEW)
				elog(ERROR, "You cannot copy view %s", relname);
			else if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
				elog(ERROR, "You cannot copy sequence %s", relname);
			else
				elog(ERROR, "You cannot copy object %s", relname);
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
		}
		CopyTo(rel, binary, oids, fp, delim, null_print);
	}

	if (!pipe)
		FreeFile(fp);
	else if (!from)
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
	heap_close(rel, (from ? NoLock : AccessShareLock));
}


/*
 * Copy from relation TO file.
 */
static void
CopyTo(Relation rel, bool binary, bool oids, FILE *fp,
	   char *delim, char *null_print)
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

	if (oids && !rel->rd_rel->relhasoids)
		elog(ERROR, "COPY: table %s does not have OIDs",
			 RelationGetRelationName(rel));

	tupDesc = rel->rd_att;
	attr_count = rel->rd_att->natts;
	attr = rel->rd_att->attrs;

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

	scandesc = heap_beginscan(rel, 0, QuerySnapshot, 0, NULL);

	while (HeapTupleIsValid(tuple = heap_getnext(scandesc, 0)))
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
				fld_size = sizeof(Oid);
				CopySendData(&fld_size, sizeof(int16), fp);
				CopySendData(&tuple->t_data->t_oid, sizeof(Oid), fp);
			}
		}
		else
		{
			/* Text format has no per-tuple header, but send OID if wanted */
			if (oids)
			{
				string = DatumGetCString(DirectFunctionCall1(oidout,
								ObjectIdGetDatum(tuple->t_data->t_oid)));
				CopySendString(string, fp);
				pfree(string);
				need_delim = true;
			}
		}

		for (i = 0; i < attr_count; i++)
		{
			Datum		origvalue,
						value;
			bool		isnull;

			origvalue = heap_getattr(tuple, i + 1, tupDesc, &isnull);

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
				if (isvarlena[i])
					value = PointerGetDatum(PG_DETOAST_DATUM(origvalue));
				else
					value = origvalue;

				if (!binary)
				{
					string = DatumGetCString(FunctionCall3(&out_functions[i],
														   value,
										   ObjectIdGetDatum(elements[i]),
									 Int32GetDatum(attr[i]->atttypmod)));
					CopyAttributeOut(fp, string, delim);
					pfree(string);
				}
				else
				{
					fld_size = attr[i]->attlen;
					CopySendData(&fld_size, sizeof(int16), fp);
					if (isvarlena[i])
					{
						/* varlena */
						Assert(fld_size == -1);
						CopySendData(DatumGetPointer(value),
									 VARSIZE(value),
									 fp);
					}
					else if (!attr[i]->attbyval)
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
CopyFrom(Relation rel, bool binary, bool oids, FILE *fp,
		 char *delim, char *null_print)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Form_pg_attribute *attr;
	AttrNumber	attr_count;
	FmgrInfo   *in_functions;
	Oid		   *elements;
	int			i;
	Oid			in_func_oid;
	Datum	   *values;
	char	   *nulls;
	bool		isnull;
	int			done = 0;
	char	   *string;
	ResultRelInfo *resultRelInfo;
	EState	   *estate = CreateExecutorState(); /* for ExecConstraints() */
	TupleTable	tupleTable;
	TupleTableSlot *slot;
	Oid			loaded_oid = InvalidOid;
	bool		skip_tuple = false;
	bool		file_has_oids;

	tupDesc = RelationGetDescr(rel);
	attr = tupDesc->attrs;
	attr_count = tupDesc->natts;

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
		in_functions = (FmgrInfo *) palloc(attr_count * sizeof(FmgrInfo));
		elements = (Oid *) palloc(attr_count * sizeof(Oid));
		for (i = 0; i < attr_count; i++)
		{
			in_func_oid = (Oid) GetInputFunction(attr[i]->atttypid);
			fmgr_info(in_func_oid, &in_functions[i]);
			elements[i] = GetTypeElement(attr[i]->atttypid);
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
		if (CopyGetEof(fp) ||
			memcmp(readSig, BinarySignature, 12) != 0)
			elog(ERROR, "COPY BINARY: file signature not recognized");
		/* Integer layout field */
		CopyGetData(&tmp, sizeof(int32), fp);
		if (CopyGetEof(fp) ||
			tmp != 0x01020304)
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
		if (CopyGetEof(fp) ||
			tmp < 0)
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

	/* Silently drop incoming OIDs if table does not have OIDs */
	if (!rel->rd_rel->relhasoids)
		oids = false;

	values = (Datum *) palloc(attr_count * sizeof(Datum));
	nulls = (char *) palloc(attr_count * sizeof(char));

	copy_lineno = 0;
	fe_eof = false;

	while (!done)
	{
		CHECK_FOR_INTERRUPTS();

		copy_lineno++;

		/* Reset the per-output-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/* Initialize all values for row to NULL */
		MemSet(values, 0, attr_count * sizeof(Datum));
		MemSet(nulls, 'n', attr_count * sizeof(char));

		if (!binary)
		{
			int			newline = 0;

			if (file_has_oids)
			{
				string = CopyReadAttribute(fp, &isnull, delim,
										   &newline, null_print);
				if (isnull)
					elog(ERROR, "COPY TEXT: NULL Oid");
				else if (string == NULL)
					done = 1;	/* end of file */
				else
				{
					loaded_oid = DatumGetObjectId(DirectFunctionCall1(oidin,
											   CStringGetDatum(string)));
					if (loaded_oid == InvalidOid)
						elog(ERROR, "COPY TEXT: Invalid Oid");
				}
			}

			for (i = 0; i < attr_count && !done; i++)
			{
				string = CopyReadAttribute(fp, &isnull, delim,
										   &newline, null_print);
				if (isnull)
				{
					/* already set values[i] and nulls[i] */
				}
				else if (string == NULL)
					done = 1;	/* end of file */
				else
				{
					values[i] = FunctionCall3(&in_functions[i],
											  CStringGetDatum(string),
										   ObjectIdGetDatum(elements[i]),
									  Int32GetDatum(attr[i]->atttypmod));
					nulls[i] = ' ';
				}
			}
			if (!done)
				CopyReadNewline(fp, &newline);
		}
		else
		{						/* binary */
			int16		fld_count,
						fld_size;

			CopyGetData(&fld_count, sizeof(int16), fp);
			if (CopyGetEof(fp) ||
				fld_count == -1)
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
			tuple->t_data->t_oid = loaded_oid;

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
			heap_insert(rel, tuple);

			if (resultRelInfo->ri_NumIndices > 0)
				ExecInsertIndexTuples(slot, &(tuple->t_self), estate, false);

			/* AFTER ROW INSERT Triggers */
			if (resultRelInfo->ri_TrigDesc)
				ExecARInsertTriggers(estate, resultRelInfo, tuple);
		}

		for (i = 0; i < attr_count; i++)
		{
			if (!attr[i]->attbyval && nulls[i] != 'n')
				pfree(DatumGetPointer(values[i]));
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
 * Reads input from fp until an end of line is seen.
 */

static void
CopyReadNewline(FILE *fp, int *newline)
{
	if (!*newline)
	{
		elog(NOTICE, "CopyReadNewline: extra fields ignored");
		while (!CopyGetEof(fp) && (CopyGetChar(fp) != '\n'));
	}
	*newline = 0;
}

/*
 * Read the value of a single attribute.
 *
 * Result is either a string, or NULL (if EOF or a null attribute).
 * Note that the caller should not pfree the string!
 *
 * *isnull is set true if a null attribute, else false.
 * delim is the column delimiter string (currently always 1 character).
 * *newline remembers whether we've seen a newline ending this tuple.
 * null_print says how NULL values are represented
 */

static char *
CopyReadAttribute(FILE *fp, bool *isnull, char *delim, int *newline, char *null_print)
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

	/* if last delimiter was a newline return a NULL attribute */
	if (*newline)
	{
		*isnull = (bool) true;
		return NULL;
	}

	*isnull = (bool) false;		/* set default */

	for (;;)
	{
		c = CopyGetChar(fp);
		if (c == EOF)
			goto endOfFile;
		if (c == '\n')
		{
			*newline = 1;
			break;
		}
		if (c == delimc)
			break;
		if (c == '\\')
		{
			c = CopyGetChar(fp);
			if (c == EOF)
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

						val = OCTVALUE(c);
						c = CopyPeekChar(fp);
						if (ISOCTAL(c))
						{
							val = (val << 3) + OCTVALUE(c);
							CopyDonePeek(fp, c, true /*pick up*/);
							c = CopyPeekChar(fp);
							if (ISOCTAL(c))
							{
								val = (val << 3) + OCTVALUE(c);
								CopyDonePeek(fp, c, true /*pick up*/);
							}
							else
							{
								if (c == EOF)
									goto endOfFile;
								CopyDonePeek(fp, c, false /*put back*/);
							}
						}
						else
						{
							if (c == EOF)
								goto endOfFile;
							CopyDonePeek(fp, c, false /*put back*/);
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
					goto endOfFile;
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
					goto endOfFile;
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

	if (strcmp(attribute_buf.data, null_print) == 0)
		*isnull = true;

	return attribute_buf.data;

endOfFile:
	return NULL;
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
