/*-------------------------------------------------------------------------
 *
 * proto.c
 *		logical replication protocol functions
 *
 * Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/replication/logical/proto.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "replication/logicalproto.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * Protocol message flags.
 */
#define LOGICALREP_IS_REPLICA_IDENTITY 1

#define TRUNCATE_CASCADE		(1<<0)
#define TRUNCATE_RESTART_SEQS	(1<<1)

static void logicalrep_write_attrs(StringInfo out, Relation rel);
static void logicalrep_write_tuple(StringInfo out, Relation rel,
								   HeapTuple tuple);

static void logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel);
static void logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple);

static void logicalrep_write_namespace(StringInfo out, Oid nspid);
static const char *logicalrep_read_namespace(StringInfo in);

/*
 * Write BEGIN to the output stream.
 */
void
logicalrep_write_begin(StringInfo out, ReorderBufferTXN *txn)
{
	pq_sendbyte(out, 'B');		/* BEGIN */

	/* fixed fields */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->commit_time);
	pq_sendint32(out, txn->xid);
}

/*
 * Read transaction BEGIN from the stream.
 */
void
logicalrep_read_begin(StringInfo in, LogicalRepBeginData *begin_data)
{
	/* read fields */
	begin_data->final_lsn = pq_getmsgint64(in);
	if (begin_data->final_lsn == InvalidXLogRecPtr)
		elog(ERROR, "final_lsn not set in begin message");
	begin_data->committime = pq_getmsgint64(in);
	begin_data->xid = pq_getmsgint(in, 4);
}


/*
 * Write COMMIT to the output stream.
 */
void
logicalrep_write_commit(StringInfo out, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn)
{
	uint8		flags = 0;

	pq_sendbyte(out, 'C');		/* sending COMMIT */

	/* send the flags field (unused for now) */
	pq_sendbyte(out, flags);

	/* send fields */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->commit_time);
}

/*
 * Read transaction COMMIT from the stream.
 */
void
logicalrep_read_commit(StringInfo in, LogicalRepCommitData *commit_data)
{
	/* read flags (unused for now) */
	uint8		flags = pq_getmsgbyte(in);

	if (flags != 0)
		elog(ERROR, "unrecognized flags %u in commit message", flags);

	/* read fields */
	commit_data->commit_lsn = pq_getmsgint64(in);
	commit_data->end_lsn = pq_getmsgint64(in);
	commit_data->committime = pq_getmsgint64(in);
}

/*
 * Write ORIGIN to the output stream.
 */
void
logicalrep_write_origin(StringInfo out, const char *origin,
						XLogRecPtr origin_lsn)
{
	pq_sendbyte(out, 'O');		/* ORIGIN */

	/* fixed fields */
	pq_sendint64(out, origin_lsn);

	/* origin string */
	pq_sendstring(out, origin);
}

/*
 * Read ORIGIN from the output stream.
 */
char *
logicalrep_read_origin(StringInfo in, XLogRecPtr *origin_lsn)
{
	/* fixed fields */
	*origin_lsn = pq_getmsgint64(in);

	/* return origin */
	return pstrdup(pq_getmsgstring(in));
}

/*
 * Write INSERT to the output stream.
 */
void
logicalrep_write_insert(StringInfo out, Relation rel, HeapTuple newtuple)
{
	pq_sendbyte(out, 'I');		/* action INSERT */

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	pq_sendbyte(out, 'N');		/* new tuple follows */
	logicalrep_write_tuple(out, rel, newtuple);
}

/*
 * Read INSERT from stream.
 *
 * Fills the new tuple.
 */
LogicalRepRelId
logicalrep_read_insert(StringInfo in, LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id */
	relid = pq_getmsgint(in, 4);

	action = pq_getmsgbyte(in);
	if (action != 'N')
		elog(ERROR, "expected new tuple but got %d",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * Write UPDATE to the output stream.
 */
void
logicalrep_write_update(StringInfo out, Relation rel, HeapTuple oldtuple,
						HeapTuple newtuple)
{
	pq_sendbyte(out, 'U');		/* action UPDATE */

	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	if (oldtuple != NULL)
	{
		if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
			pq_sendbyte(out, 'O');	/* old tuple follows */
		else
			pq_sendbyte(out, 'K');	/* old key follows */
		logicalrep_write_tuple(out, rel, oldtuple);
	}

	pq_sendbyte(out, 'N');		/* new tuple follows */
	logicalrep_write_tuple(out, rel, newtuple);
}

/*
 * Read UPDATE from stream.
 */
LogicalRepRelId
logicalrep_read_update(StringInfo in, bool *has_oldtuple,
					   LogicalRepTupleData *oldtup,
					   LogicalRepTupleData *newtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id */
	relid = pq_getmsgint(in, 4);

	/* read and verify action */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O' && action != 'N')
		elog(ERROR, "expected action 'N', 'O' or 'K', got %c",
			 action);

	/* check for old tuple */
	if (action == 'K' || action == 'O')
	{
		logicalrep_read_tuple(in, oldtup);
		*has_oldtuple = true;

		action = pq_getmsgbyte(in);
	}
	else
		*has_oldtuple = false;

	/* check for new  tuple */
	if (action != 'N')
		elog(ERROR, "expected action 'N', got %c",
			 action);

	logicalrep_read_tuple(in, newtup);

	return relid;
}

/*
 * Write DELETE to the output stream.
 */
void
logicalrep_write_delete(StringInfo out, Relation rel, HeapTuple oldtuple)
{
	Assert(rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL ||
		   rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX);

	pq_sendbyte(out, 'D');		/* action DELETE */

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		pq_sendbyte(out, 'O');	/* old tuple follows */
	else
		pq_sendbyte(out, 'K');	/* old key follows */

	logicalrep_write_tuple(out, rel, oldtuple);
}

/*
 * Read DELETE from stream.
 *
 * Fills the old tuple.
 */
LogicalRepRelId
logicalrep_read_delete(StringInfo in, LogicalRepTupleData *oldtup)
{
	char		action;
	LogicalRepRelId relid;

	/* read the relation id */
	relid = pq_getmsgint(in, 4);

	/* read and verify action */
	action = pq_getmsgbyte(in);
	if (action != 'K' && action != 'O')
		elog(ERROR, "expected action 'O' or 'K', got %c", action);

	logicalrep_read_tuple(in, oldtup);

	return relid;
}

/*
 * Write TRUNCATE to the output stream.
 */
void
logicalrep_write_truncate(StringInfo out,
						  int nrelids,
						  Oid relids[],
						  bool cascade, bool restart_seqs)
{
	int			i;
	uint8		flags = 0;

	pq_sendbyte(out, 'T');		/* action TRUNCATE */

	pq_sendint32(out, nrelids);

	/* encode and send truncate flags */
	if (cascade)
		flags |= TRUNCATE_CASCADE;
	if (restart_seqs)
		flags |= TRUNCATE_RESTART_SEQS;
	pq_sendint8(out, flags);

	for (i = 0; i < nrelids; i++)
		pq_sendint32(out, relids[i]);
}

/*
 * Read TRUNCATE from stream.
 */
List *
logicalrep_read_truncate(StringInfo in,
						 bool *cascade, bool *restart_seqs)
{
	int			i;
	int			nrelids;
	List	   *relids = NIL;
	uint8		flags;

	nrelids = pq_getmsgint(in, 4);

	/* read and decode truncate flags */
	flags = pq_getmsgint(in, 1);
	*cascade = (flags & TRUNCATE_CASCADE) > 0;
	*restart_seqs = (flags & TRUNCATE_RESTART_SEQS) > 0;

	for (i = 0; i < nrelids; i++)
		relids = lappend_oid(relids, pq_getmsgint(in, 4));

	return relids;
}

/*
 * Write relation description to the output stream.
 */
void
logicalrep_write_rel(StringInfo out, Relation rel)
{
	char	   *relname;

	pq_sendbyte(out, 'R');		/* sending RELATION */

	/* use Oid as relation identifier */
	pq_sendint32(out, RelationGetRelid(rel));

	/* send qualified relation name */
	logicalrep_write_namespace(out, RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);
	pq_sendstring(out, relname);

	/* send replica identity */
	pq_sendbyte(out, rel->rd_rel->relreplident);

	/* send the attribute info */
	logicalrep_write_attrs(out, rel);
}

/*
 * Read the relation info from stream and return as LogicalRepRelation.
 */
LogicalRepRelation *
logicalrep_read_rel(StringInfo in)
{
	LogicalRepRelation *rel = palloc(sizeof(LogicalRepRelation));

	rel->remoteid = pq_getmsgint(in, 4);

	/* Read relation name from stream */
	rel->nspname = pstrdup(logicalrep_read_namespace(in));
	rel->relname = pstrdup(pq_getmsgstring(in));

	/* Read the replica identity. */
	rel->replident = pq_getmsgbyte(in);

	/* Get attribute description */
	logicalrep_read_attrs(in, rel);

	return rel;
}

/*
 * Write type info to the output stream.
 *
 * This function will always write base type info.
 */
void
logicalrep_write_typ(StringInfo out, Oid typoid)
{
	Oid			basetypoid = getBaseType(typoid);
	HeapTuple	tup;
	Form_pg_type typtup;

	pq_sendbyte(out, 'Y');		/* sending TYPE */

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(basetypoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", basetypoid);
	typtup = (Form_pg_type) GETSTRUCT(tup);

	/* use Oid as relation identifier */
	pq_sendint32(out, typoid);

	/* send qualified type name */
	logicalrep_write_namespace(out, typtup->typnamespace);
	pq_sendstring(out, NameStr(typtup->typname));

	ReleaseSysCache(tup);
}

/*
 * Read type info from the output stream.
 */
void
logicalrep_read_typ(StringInfo in, LogicalRepTyp *ltyp)
{
	ltyp->remoteid = pq_getmsgint(in, 4);

	/* Read type name from stream */
	ltyp->nspname = pstrdup(logicalrep_read_namespace(in));
	ltyp->typname = pstrdup(pq_getmsgstring(in));
}

/*
 * Write a tuple to the outputstream, in the most efficient format possible.
 */
static void
logicalrep_write_tuple(StringInfo out, Relation rel, HeapTuple tuple)
{
	TupleDesc	desc;
	Datum		values[MaxTupleAttributeNumber];
	bool		isnull[MaxTupleAttributeNumber];
	int			i;
	uint16		nliveatts = 0;

	desc = RelationGetDescr(rel);

	for (i = 0; i < desc->natts; i++)
	{
		if (TupleDescAttr(desc, i)->attisdropped || TupleDescAttr(desc, i)->attgenerated)
			continue;
		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	/* try to allocate enough memory from the get-go */
	enlargeStringInfo(out, tuple->t_len +
					  nliveatts * (1 + 4));

	heap_deform_tuple(tuple, desc, values, isnull);

	/* Write the values */
	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typtup;
		Form_pg_type typclass;
		Form_pg_attribute att = TupleDescAttr(desc, i);
		char	   *outputstr;

		if (att->attisdropped || att->attgenerated)
			continue;

		if (isnull[i])
		{
			pq_sendbyte(out, 'n');	/* null column */
			continue;
		}
		else if (att->attlen == -1 && VARATT_IS_EXTERNAL_ONDISK(values[i]))
		{
			pq_sendbyte(out, 'u');	/* unchanged toast column */
			continue;
		}

		typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(typtup))
			elog(ERROR, "cache lookup failed for type %u", att->atttypid);
		typclass = (Form_pg_type) GETSTRUCT(typtup);

		pq_sendbyte(out, 't');	/* 'text' data follows */

		outputstr = OidOutputFunctionCall(typclass->typoutput, values[i]);
		pq_sendcountedtext(out, outputstr, strlen(outputstr), false);
		pfree(outputstr);

		ReleaseSysCache(typtup);
	}
}

/*
 * Read tuple in remote format from stream.
 *
 * The returned tuple points into the input stringinfo.
 */
static void
logicalrep_read_tuple(StringInfo in, LogicalRepTupleData *tuple)
{
	int			i;
	int			natts;

	/* Get number of attributes */
	natts = pq_getmsgint(in, 2);

	memset(tuple->changed, 0, sizeof(tuple->changed));

	/* Read the data */
	for (i = 0; i < natts; i++)
	{
		char		kind;

		kind = pq_getmsgbyte(in);

		switch (kind)
		{
			case 'n':			/* null */
				tuple->values[i] = NULL;
				tuple->changed[i] = true;
				break;
			case 'u':			/* unchanged column */
				/* we don't receive the value of an unchanged column */
				tuple->values[i] = NULL;
				break;
			case 't':			/* text formatted value */
				{
					int			len;

					tuple->changed[i] = true;

					len = pq_getmsgint(in, 4);	/* read length */

					/* and data */
					tuple->values[i] = palloc(len + 1);
					pq_copymsgbytes(in, tuple->values[i], len);
					tuple->values[i][len] = '\0';
				}
				break;
			default:
				elog(ERROR, "unrecognized data representation type '%c'", kind);
		}
	}
}

/*
 * Write relation attributes to the stream.
 */
static void
logicalrep_write_attrs(StringInfo out, Relation rel)
{
	TupleDesc	desc;
	int			i;
	uint16		nliveatts = 0;
	Bitmapset  *idattrs = NULL;
	bool		replidentfull;

	desc = RelationGetDescr(rel);

	/* send number of live attributes */
	for (i = 0; i < desc->natts; i++)
	{
		if (TupleDescAttr(desc, i)->attisdropped || TupleDescAttr(desc, i)->attgenerated)
			continue;
		nliveatts++;
	}
	pq_sendint16(out, nliveatts);

	/* fetch bitmap of REPLICATION IDENTITY attributes */
	replidentfull = (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL);
	if (!replidentfull)
		idattrs = RelationGetIndexAttrBitmap(rel,
											 INDEX_ATTR_BITMAP_IDENTITY_KEY);

	/* send the attributes */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		uint8		flags = 0;

		if (att->attisdropped || att->attgenerated)
			continue;

		/* REPLICA IDENTITY FULL means all columns are sent as part of key. */
		if (replidentfull ||
			bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			flags |= LOGICALREP_IS_REPLICA_IDENTITY;

		pq_sendbyte(out, flags);

		/* attribute name */
		pq_sendstring(out, NameStr(att->attname));

		/* attribute type id */
		pq_sendint32(out, (int) att->atttypid);

		/* attribute mode */
		pq_sendint32(out, att->atttypmod);
	}

	bms_free(idattrs);
}

/*
 * Read relation attribute names from the stream.
 */
static void
logicalrep_read_attrs(StringInfo in, LogicalRepRelation *rel)
{
	int			i;
	int			natts;
	char	  **attnames;
	Oid		   *atttyps;
	Bitmapset  *attkeys = NULL;

	natts = pq_getmsgint(in, 2);
	attnames = palloc(natts * sizeof(char *));
	atttyps = palloc(natts * sizeof(Oid));

	/* read the attributes */
	for (i = 0; i < natts; i++)
	{
		uint8		flags;

		/* Check for replica identity column */
		flags = pq_getmsgbyte(in);
		if (flags & LOGICALREP_IS_REPLICA_IDENTITY)
			attkeys = bms_add_member(attkeys, i);

		/* attribute name */
		attnames[i] = pstrdup(pq_getmsgstring(in));

		/* attribute type id */
		atttyps[i] = (Oid) pq_getmsgint(in, 4);

		/* we ignore attribute mode for now */
		(void) pq_getmsgint(in, 4);
	}

	rel->attnames = attnames;
	rel->atttyps = atttyps;
	rel->attkeys = attkeys;
	rel->natts = natts;
}

/*
 * Write the namespace name or empty string for pg_catalog (to save space).
 */
static void
logicalrep_write_namespace(StringInfo out, Oid nspid)
{
	if (nspid == PG_CATALOG_NAMESPACE)
		pq_sendbyte(out, '\0');
	else
	{
		char	   *nspname = get_namespace_name(nspid);

		if (nspname == NULL)
			elog(ERROR, "cache lookup failed for namespace %u",
				 nspid);

		pq_sendstring(out, nspname);
	}
}

/*
 * Read the namespace name while treating empty string as pg_catalog.
 */
static const char *
logicalrep_read_namespace(StringInfo in)
{
	const char *nspname = pq_getmsgstring(in);

	if (nspname[0] == '\0')
		nspname = "pg_catalog";

	return nspname;
}
