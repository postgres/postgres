/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  PostgreSQL sequences support code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/sequence.c,v 1.103.2.2 2004/04/06 16:39:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"

/*
 * We don't want to log each fetching of a value from a sequence,
 * so we pre-log a few fetches in advance. In the event of
 * crash we can lose as much as we pre-logged.
 */
#define SEQ_LOG_VALS	32

/*
 * The "special area" of a sequence's buffer page looks like this.
 */
#define SEQ_MAGIC	  0x1717

typedef struct sequence_magic
{
	uint32		magic;
} sequence_magic;

/*
 * We store a SeqTable item for every sequence we have touched in the current
 * session.  This is needed to hold onto nextval/currval state.  (We can't
 * rely on the relcache, since it's only, well, a cache, and may decide to
 * discard entries.)
 *
 * XXX We use linear search to find pre-existing SeqTable entries.	This is
 * good when only a small number of sequences are touched in a session, but
 * would suck with many different sequences.  Perhaps use a hashtable someday.
 */
typedef struct SeqTableData
{
	struct SeqTableData *next;	/* link to next SeqTable object */
	Oid			relid;			/* pg_class OID of this sequence */
	TransactionId xid;			/* xact in which we last did a seq op */
	int64		last;			/* value last returned by nextval */
	int64		cached;			/* last value already cached for nextval */
	/* if last != cached, we have not used up all the cached values */
	int64		increment;		/* copy of sequence's increment field */
} SeqTableData;

typedef SeqTableData *SeqTable;

static SeqTable seqtab = NULL;	/* Head of list of SeqTable items */


static void init_sequence(RangeVar *relation,
			  SeqTable *p_elm, Relation *p_rel);
static Form_pg_sequence read_info(SeqTable elm, Relation rel, Buffer *buf);
static void init_params(List *options, Form_pg_sequence new, bool isInit);
static void do_setval(RangeVar *sequence, int64 next, bool iscalled);

/*
 * DefineSequence
 *				Creates a new sequence relation
 */
void
DefineSequence(CreateSeqStmt *seq)
{
	FormData_pg_sequence new;
	CreateStmt *stmt = makeNode(CreateStmt);
	Oid			seqoid;
	Relation	rel;
	Buffer		buf;
	PageHeader	page;
	sequence_magic *sm;
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Datum		value[SEQ_COL_LASTCOL];
	char		null[SEQ_COL_LASTCOL];
	int			i;
	NameData	name;

	/* Check and set all option values */
	init_params(seq->options, &new, true);

	/*
	 * Create relation (and fill *null & *value)
	 */
	stmt->tableElts = NIL;
	for (i = SEQ_COL_FIRSTCOL; i <= SEQ_COL_LASTCOL; i++)
	{
		ColumnDef  *coldef;
		TypeName   *typnam;

		typnam = makeNode(TypeName);
		typnam->setof = FALSE;
		typnam->arrayBounds = NIL;
		typnam->typmod = -1;

		coldef = makeNode(ColumnDef);
		coldef->typename = typnam;
		coldef->inhcount = 0;
		coldef->is_local = true;
		coldef->is_not_null = true;
		coldef->raw_default = NULL;
		coldef->cooked_default = NULL;
		coldef->constraints = NIL;
		coldef->support = NULL;

		null[i - 1] = ' ';

		switch (i)
		{
			case SEQ_COL_NAME:
				typnam->typeid = NAMEOID;
				coldef->colname = "sequence_name";
				namestrcpy(&name, seq->sequence->relname);
				value[i - 1] = NameGetDatum(&name);
				break;
			case SEQ_COL_LASTVAL:
				typnam->typeid = INT8OID;
				coldef->colname = "last_value";
				value[i - 1] = Int64GetDatumFast(new.last_value);
				break;
			case SEQ_COL_INCBY:
				typnam->typeid = INT8OID;
				coldef->colname = "increment_by";
				value[i - 1] = Int64GetDatumFast(new.increment_by);
				break;
			case SEQ_COL_MAXVALUE:
				typnam->typeid = INT8OID;
				coldef->colname = "max_value";
				value[i - 1] = Int64GetDatumFast(new.max_value);
				break;
			case SEQ_COL_MINVALUE:
				typnam->typeid = INT8OID;
				coldef->colname = "min_value";
				value[i - 1] = Int64GetDatumFast(new.min_value);
				break;
			case SEQ_COL_CACHE:
				typnam->typeid = INT8OID;
				coldef->colname = "cache_value";
				value[i - 1] = Int64GetDatumFast(new.cache_value);
				break;
			case SEQ_COL_LOG:
				typnam->typeid = INT8OID;
				coldef->colname = "log_cnt";
				value[i - 1] = Int64GetDatum((int64) 1);
				break;
			case SEQ_COL_CYCLE:
				typnam->typeid = BOOLOID;
				coldef->colname = "is_cycled";
				value[i - 1] = BoolGetDatum(new.is_cycled);
				break;
			case SEQ_COL_CALLED:
				typnam->typeid = BOOLOID;
				coldef->colname = "is_called";
				value[i - 1] = BoolGetDatum(false);
				break;
		}
		stmt->tableElts = lappend(stmt->tableElts, coldef);
	}

	stmt->relation = seq->sequence;
	stmt->inhRelations = NIL;
	stmt->constraints = NIL;
	stmt->hasoids = false;
	stmt->oncommit = ONCOMMIT_NOOP;

	seqoid = DefineRelation(stmt, RELKIND_SEQUENCE);

	rel = heap_open(seqoid, AccessExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* Initialize first page of relation with special magic number */

	buf = ReadBuffer(rel, P_NEW);

	if (!BufferIsValid(buf))
		elog(ERROR, "ReadBuffer failed");

	Assert(BufferGetBlockNumber(buf) == 0);

	page = (PageHeader) BufferGetPage(buf);

	PageInit((Page) page, BufferGetPageSize(buf), sizeof(sequence_magic));
	sm = (sequence_magic *) PageGetSpecialPointer(page);
	sm->magic = SEQ_MAGIC;

	/* hack: ensure heap_insert will insert on the just-created page */
	rel->rd_targblock = 0;

	/* Now form & insert sequence tuple */
	tuple = heap_formtuple(tupDesc, value, null);
	simple_heap_insert(rel, tuple);

	Assert(ItemPointerGetOffsetNumber(&(tuple->t_self)) == FirstOffsetNumber);

	/*
	 * Two special hacks here:
	 *
	 * 1. Since VACUUM does not process sequences, we have to force the tuple
	 * to have xmin = FrozenTransactionId now.	Otherwise it would become
	 * invisible to SELECTs after 2G transactions.	It is okay to do this
	 * because if the current transaction aborts, no other xact will ever
	 * examine the sequence tuple anyway.
	 *
	 * 2. Even though heap_insert emitted a WAL log record, we have to emit
	 * an XLOG_SEQ_LOG record too, since (a) the heap_insert record will
	 * not have the right xmin, and (b) REDO of the heap_insert record
	 * would re-init page and sequence magic number would be lost.	This
	 * means two log records instead of one :-(
	 */
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	START_CRIT_SECTION();

	{
		/*
		 * Note that the "tuple" structure is still just a local tuple
		 * record created by heap_formtuple; its t_data pointer doesn't
		 * point at the disk buffer.  To scribble on the disk buffer we
		 * need to fetch the item pointer.	But do the same to the local
		 * tuple, since that will be the source for the WAL log record,
		 * below.
		 */
		ItemId		itemId;
		Item		item;

		itemId = PageGetItemId((Page) page, FirstOffsetNumber);
		item = PageGetItem((Page) page, itemId);

		HeapTupleHeaderSetXmin((HeapTupleHeader) item, FrozenTransactionId);
		((HeapTupleHeader) item)->t_infomask |= HEAP_XMIN_COMMITTED;

		HeapTupleHeaderSetXmin(tuple->t_data, FrozenTransactionId);
		tuple->t_data->t_infomask |= HEAP_XMIN_COMMITTED;
	}

	/* XLOG stuff */
	if (!rel->rd_istemp)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];
		Form_pg_sequence newseq = (Form_pg_sequence) GETSTRUCT(tuple);

		/* We do not log first nextval call, so "advance" sequence here */
		/* Note we are scribbling on local tuple, not the disk buffer */
		newseq->is_called = true;
		newseq->log_cnt = 0;

		xlrec.node = rel->rd_node;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_seq_rec);
		rdata[0].next = &(rdata[1]);

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) tuple->t_data;
		rdata[1].len = tuple->t_len;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG | XLOG_NO_TRAN, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}

	END_CRIT_SECTION();

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buf);
	heap_close(rel, NoLock);
}

/*
 * AlterSequence
 *
 * Modify the definition of a sequence relation
 */
void
AlterSequence(AlterSeqStmt *stmt)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	Page		page;
	Form_pg_sequence seq;
	FormData_pg_sequence new;

	/* open and AccessShareLock sequence */
	init_sequence(stmt->sequence, &elm, &seqrel);

	/* allow ALTER to sequence owner only */
	if (!pg_class_ownercheck(elm->relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   stmt->sequence->relname);

	/* lock page' buffer and read tuple into new sequence structure */
	seq = read_info(elm, seqrel, &buf);
	page = BufferGetPage(buf);

	/* Copy old values of options into workspace */
	memcpy(&new, seq, sizeof(FormData_pg_sequence));

	/* Check and set new values */
	init_params(stmt->options, &new, false);

	/* Now okay to update the on-disk tuple */
	memcpy(seq, &new, sizeof(FormData_pg_sequence));

	/* Clear local cache so that we don't think we have cached numbers */
	elm->last = new.last_value;			/* last returned number */
	elm->cached = new.last_value;		/* last cached number (forget
										 * cached values) */

	START_CRIT_SECTION();

	/* XLOG stuff */
	if (!seqrel->rd_istemp)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.node = seqrel->rd_node;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_seq_rec);
		rdata[0].next = &(rdata[1]);

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) page + ((PageHeader) page)->pd_upper;
		rdata[1].len = ((PageHeader) page)->pd_special -
			((PageHeader) page)->pd_upper;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG | XLOG_NO_TRAN, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}

	END_CRIT_SECTION();

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	WriteBuffer(buf);

	relation_close(seqrel, NoLock);
}


Datum
nextval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	RangeVar   *sequence;
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	Page		page;
	Form_pg_sequence seq;
	int64		incby,
				maxv,
				minv,
				cache,
				log,
				fetch,
				last;
	int64		result,
				next,
				rescnt = 0;
	bool		logit = false;

	sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin,
															 "nextval"));

	/* open and AccessShareLock sequence */
	init_sequence(sequence, &elm, &seqrel);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						sequence->relname)));

	if (elm->last != elm->cached)		/* some numbers were cached */
	{
		elm->last += elm->increment;
		relation_close(seqrel, NoLock);
		PG_RETURN_INT64(elm->last);
	}

	/* lock page' buffer and read tuple */
	seq = read_info(elm, seqrel, &buf);
	page = BufferGetPage(buf);

	last = next = result = seq->last_value;
	incby = seq->increment_by;
	maxv = seq->max_value;
	minv = seq->min_value;
	fetch = cache = seq->cache_value;
	log = seq->log_cnt;

	if (!seq->is_called)
	{
		rescnt++;				/* last_value if not called */
		fetch--;
		log--;
	}

	/*
	 * Decide whether we should emit a WAL log record.	If so, force up
	 * the fetch count to grab SEQ_LOG_VALS more values than we actually
	 * need to cache.  (These will then be usable without logging.)
	 *
	 * If this is the first nextval after a checkpoint, we must force a new
	 * WAL record to be written anyway, else replay starting from the
	 * checkpoint would fail to advance the sequence past the logged
	 * values.	In this case we may as well fetch extra values.
	 */
	if (log < fetch)
	{
		/* forced log to satisfy local demand for values */
		fetch = log = fetch + SEQ_LOG_VALS;
		logit = true;
	}
	else
	{
		XLogRecPtr	redoptr = GetRedoRecPtr();

		if (XLByteLE(PageGetLSN(page), redoptr))
		{
			/* last update of seq was before checkpoint */
			fetch = log = fetch + SEQ_LOG_VALS;
			logit = true;
		}
	}

	while (fetch)				/* try to fetch cache [+ log ] numbers */
	{
		/*
		 * Check MAXVALUE for ascending sequences and MINVALUE for
		 * descending sequences
		 */
		if (incby > 0)
		{
			/* ascending sequence */
			if ((maxv >= 0 && next > maxv - incby) ||
				(maxv < 0 && next + incby > maxv))
			{
				if (rescnt > 0)
					break;		/* stop fetching */
				if (!seq->is_cycled)
				{
					char		buf[100];

					snprintf(buf, sizeof(buf), INT64_FORMAT, maxv);
					ereport(ERROR,
					  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					   errmsg("nextval: reached maximum value of sequence \"%s\" (%s)",
							  sequence->relname, buf)));
				}
				next = minv;
			}
			else
				next += incby;
		}
		else
		{
			/* descending sequence */
			if ((minv < 0 && next < minv - incby) ||
				(minv >= 0 && next + incby < minv))
			{
				if (rescnt > 0)
					break;		/* stop fetching */
				if (!seq->is_cycled)
				{
					char		buf[100];

					snprintf(buf, sizeof(buf), INT64_FORMAT, minv);
					ereport(ERROR,
					  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					   errmsg("nextval: reached minimum value of sequence \"%s\" (%s)",
							  sequence->relname, buf)));
				}
				next = maxv;
			}
			else
				next += incby;
		}
		fetch--;
		if (rescnt < cache)
		{
			log--;
			rescnt++;
			last = next;
			if (rescnt == 1)	/* if it's first result - */
				result = next;	/* it's what to return */
		}
	}

	log -= fetch;				/* adjust for any unfetched numbers */
	Assert(log >= 0);

	/* save info in local cache */
	elm->last = result;			/* last returned number */
	elm->cached = last;			/* last fetched number */

	START_CRIT_SECTION();

	/* XLOG stuff */
	if (logit && !seqrel->rd_istemp)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.node = seqrel->rd_node;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_seq_rec);
		rdata[0].next = &(rdata[1]);

		/* set values that will be saved in xlog */
		seq->last_value = next;
		seq->is_called = true;
		seq->log_cnt = 0;

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) page + ((PageHeader) page)->pd_upper;
		rdata[1].len = ((PageHeader) page)->pd_special -
			((PageHeader) page)->pd_upper;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG | XLOG_NO_TRAN, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}

	/* update on-disk data */
	seq->last_value = last;		/* last fetched number */
	seq->is_called = true;
	seq->log_cnt = log;			/* how much is logged */

	END_CRIT_SECTION();

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	WriteBuffer(buf);

	relation_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}

Datum
currval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	RangeVar   *sequence;
	SeqTable	elm;
	Relation	seqrel;
	int64		result;

	sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin,
															 "currval"));

	/* open and AccessShareLock sequence */
	init_sequence(sequence, &elm, &seqrel);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						sequence->relname)));

	if (elm->increment == 0)	/* nextval/read_info were not called */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("currval of sequence \"%s\" is not yet defined in this session",
						sequence->relname)));

	result = elm->last;

	relation_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}

/*
 * Main internal procedure that handles 2 & 3 arg forms of SETVAL.
 *
 * Note that the 3 arg version (which sets the is_called flag) is
 * only for use in pg_dump, and setting the is_called flag may not
 * work if multiple users are attached to the database and referencing
 * the sequence (unlikely if pg_dump is restoring it).
 *
 * It is necessary to have the 3 arg version so that pg_dump can
 * restore the state of a sequence exactly during data-only restores -
 * it is the only way to clear the is_called flag in an existing
 * sequence.
 */
static void
do_setval(RangeVar *sequence, int64 next, bool iscalled)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	Form_pg_sequence seq;

	/* open and AccessShareLock sequence */
	init_sequence(sequence, &elm, &seqrel);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						sequence->relname)));

	/* lock page' buffer and read tuple */
	seq = read_info(elm, seqrel, &buf);

	if ((next < seq->min_value) || (next > seq->max_value))
	{
		char		bufv[100],
					bufm[100],
					bufx[100];

		snprintf(bufv, sizeof(bufv), INT64_FORMAT, next);
		snprintf(bufm, sizeof(bufm), INT64_FORMAT, seq->min_value);
		snprintf(bufx, sizeof(bufx), INT64_FORMAT, seq->max_value);
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("setval: value %s is out of bounds for sequence \"%s\" (%s..%s)",
						bufv, sequence->relname, bufm, bufx)));
	}

	/* save info in local cache */
	elm->last = next;			/* last returned number */
	elm->cached = next;			/* last cached number (forget cached
								 * values) */

	START_CRIT_SECTION();

	/* XLOG stuff */
	if (!seqrel->rd_istemp)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];
		Page		page = BufferGetPage(buf);

		xlrec.node = seqrel->rd_node;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_seq_rec);
		rdata[0].next = &(rdata[1]);

		/* set values that will be saved in xlog */
		seq->last_value = next;
		seq->is_called = true;
		seq->log_cnt = 0;

		rdata[1].buffer = InvalidBuffer;
		rdata[1].data = (char *) page + ((PageHeader) page)->pd_upper;
		rdata[1].len = ((PageHeader) page)->pd_special -
			((PageHeader) page)->pd_upper;
		rdata[1].next = NULL;

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG | XLOG_NO_TRAN, rdata);

		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}

	/* save info in sequence relation */
	seq->last_value = next;		/* last fetched number */
	seq->is_called = iscalled;
	seq->log_cnt = (iscalled) ? 0 : 1;

	END_CRIT_SECTION();

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	WriteBuffer(buf);

	relation_close(seqrel, NoLock);
}

/*
 * Implement the 2 arg setval procedure.
 * See do_setval for discussion.
 */
Datum
setval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	int64		next = PG_GETARG_INT64(1);
	RangeVar   *sequence;

	sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin,
															  "setval"));

	do_setval(sequence, next, true);

	PG_RETURN_INT64(next);
}

/*
 * Implement the 3 arg setval procedure.
 * See do_setval for discussion.
 */
Datum
setval_and_iscalled(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	int64		next = PG_GETARG_INT64(1);
	bool		iscalled = PG_GETARG_BOOL(2);
	RangeVar   *sequence;

	sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin,
															  "setval"));

	do_setval(sequence, next, iscalled);

	PG_RETURN_INT64(next);
}


/*
 * Given a relation name, open and lock the sequence.  p_elm and p_rel are
 * output parameters.
 */
static void
init_sequence(RangeVar *relation, SeqTable *p_elm, Relation *p_rel)
{
	Oid			relid = RangeVarGetRelid(relation, false);
	TransactionId thisxid = GetCurrentTransactionId();
	SeqTable	elm;
	Relation	seqrel;

	/* Look to see if we already have a seqtable entry for relation */
	for (elm = seqtab; elm != NULL; elm = elm->next)
	{
		if (elm->relid == relid)
			break;
	}

	/*
	 * Open the sequence relation, acquiring AccessShareLock if we don't
	 * already have a lock in the current xact.
	 */
	if (elm == NULL || elm->xid != thisxid)
		seqrel = relation_open(relid, AccessShareLock);
	else
		seqrel = relation_open(relid, NoLock);

	if (seqrel->rd_rel->relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence",
						relation->relname)));

	/*
	 * Allocate new seqtable entry if we didn't find one.
	 *
	 * NOTE: seqtable entries remain in the list for the life of a backend.
	 * If the sequence itself is deleted then the entry becomes wasted
	 * memory, but it's small enough that this should not matter.
	 */
	if (elm == NULL)
	{
		/*
		 * Time to make a new seqtable entry.  These entries live as long
		 * as the backend does, so we use plain malloc for them.
		 */
		elm = (SeqTable) malloc(sizeof(SeqTableData));
		if (elm == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		elm->relid = relid;
		/* increment is set to 0 until we do read_info (see currval) */
		elm->last = elm->cached = elm->increment = 0;
		elm->next = seqtab;
		seqtab = elm;
	}

	/* Flag that we have a lock in the current xact. */
	elm->xid = thisxid;

	*p_elm = elm;
	*p_rel = seqrel;
}


/* Given an opened relation, lock the page buffer and find the tuple */
static Form_pg_sequence
read_info(SeqTable elm, Relation rel, Buffer *buf)
{
	PageHeader	page;
	ItemId		lp;
	HeapTupleData tuple;
	sequence_magic *sm;
	Form_pg_sequence seq;

	if (rel->rd_nblocks > 1)
		elog(ERROR, "invalid number of blocks in sequence \"%s\"",
			 RelationGetRelationName(rel));

	*buf = ReadBuffer(rel, 0);
	if (!BufferIsValid(*buf))
		elog(ERROR, "ReadBuffer failed");

	LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);

	page = (PageHeader) BufferGetPage(*buf);
	sm = (sequence_magic *) PageGetSpecialPointer(page);

	if (sm->magic != SEQ_MAGIC)
		elog(ERROR, "bad magic number in sequence \"%s\": %08X",
			 RelationGetRelationName(rel), sm->magic);

	lp = PageGetItemId(page, FirstOffsetNumber);
	Assert(ItemIdIsUsed(lp));
	tuple.t_data = (HeapTupleHeader) PageGetItem((Page) page, lp);

	seq = (Form_pg_sequence) GETSTRUCT(&tuple);

	elm->increment = seq->increment_by;

	return seq;
}

/*
 * init_params: process the options list of CREATE or ALTER SEQUENCE,
 * and store the values into appropriate fields of *new.
 *
 * If isInit is true, fill any unspecified options with default values;
 * otherwise, do not change existing options that aren't explicitly overridden.
 */
static void
init_params(List *options, Form_pg_sequence new, bool isInit)
{
	DefElem    *last_value = NULL;
	DefElem    *increment_by = NULL;
	DefElem    *max_value = NULL;
	DefElem    *min_value = NULL;
	DefElem    *cache_value = NULL;
	DefElem    *is_cycled = NULL;
	List	   *option;

	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "increment") == 0)
		{
			if (increment_by)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			increment_by = defel;
		}

		/*
		 * start is for a new sequence restart is for alter
		 */
		else if (strcmp(defel->defname, "start") == 0 ||
				 strcmp(defel->defname, "restart") == 0)
		{
			if (last_value)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			last_value = defel;
		}
		else if (strcmp(defel->defname, "maxvalue") == 0)
		{
			if (max_value)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			max_value = defel;
		}
		else if (strcmp(defel->defname, "minvalue") == 0)
		{
			if (min_value)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			min_value = defel;
		}
		else if (strcmp(defel->defname, "cache") == 0)
		{
			if (cache_value)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			cache_value = defel;
		}
		else if (strcmp(defel->defname, "cycle") == 0)
		{
			if (is_cycled)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			is_cycled = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/* INCREMENT BY */
	if (increment_by != (DefElem *) NULL)
	{
		new->increment_by = defGetInt64(increment_by);
		if (new->increment_by == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("INCREMENT must not be zero")));
	}
	else if (isInit)
		new->increment_by = 1;

	/* CYCLE */
	if (is_cycled != (DefElem *) NULL)
	{
		new->is_cycled = intVal(is_cycled->arg);
		Assert(new->is_cycled == false || new->is_cycled == true);
	}
	else if (isInit)
		new->is_cycled = false;

	/* MAXVALUE (null arg means NO MAXVALUE) */
	if (max_value != (DefElem *) NULL && max_value->arg)
	{
		new->max_value = defGetInt64(max_value);
	}
	else if (isInit || max_value != (DefElem *) NULL)
	{
		if (new->increment_by > 0)
			new->max_value = SEQ_MAXVALUE;		/* ascending seq */
		else
			new->max_value = -1;				/* descending seq */
	}

	/* MINVALUE (null arg means NO MINVALUE) */
	if (min_value != (DefElem *) NULL && min_value->arg)
	{
		new->min_value = defGetInt64(min_value);
	}
	else if (isInit || min_value != (DefElem *) NULL)
	{
		if (new->increment_by > 0)
			new->min_value = 1;					/* ascending seq */
		else
			new->min_value = SEQ_MINVALUE;		/* descending seq */
	}

	/* crosscheck min/max */
	if (new->min_value >= new->max_value)
	{
		char		bufm[100],
					bufx[100];

		snprintf(bufm, sizeof(bufm), INT64_FORMAT, new->min_value);
		snprintf(bufx, sizeof(bufx), INT64_FORMAT, new->max_value);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("MINVALUE (%s) must be less than MAXVALUE (%s)",
						bufm, bufx)));
	}

	/* START WITH */
	if (last_value != (DefElem *) NULL)
	{
		new->last_value = defGetInt64(last_value);
		new->is_called = false;
		new->log_cnt = 1;
	}
	else if (isInit)
	{
		if (new->increment_by > 0)
			new->last_value = new->min_value;	/* ascending seq */
		else
			new->last_value = new->max_value;	/* descending seq */
		new->is_called = false;
		new->log_cnt = 1;
	}

	/* crosscheck */
	if (new->last_value < new->min_value)
	{
		char		bufs[100],
					bufm[100];

		snprintf(bufs, sizeof(bufs), INT64_FORMAT, new->last_value);
		snprintf(bufm, sizeof(bufm), INT64_FORMAT, new->min_value);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			  errmsg("START value (%s) can't be less than MINVALUE (%s)",
					 bufs, bufm)));
	}
	if (new->last_value > new->max_value)
	{
		char		bufs[100],
					bufm[100];

		snprintf(bufs, sizeof(bufs), INT64_FORMAT, new->last_value);
		snprintf(bufm, sizeof(bufm), INT64_FORMAT, new->max_value);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("START value (%s) can't be greater than MAXVALUE (%s)",
				  bufs, bufm)));
	}

	/* CACHE */
	if (cache_value != (DefElem *) NULL)
	{
		new->cache_value = defGetInt64(cache_value);
		if (new->cache_value <= 0)
		{
			char		buf[100];

			snprintf(buf, sizeof(buf), INT64_FORMAT, new->cache_value);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("CACHE (%s) must be greater than zero",
							buf)));
		}
	}
	else if (isInit)
		new->cache_value = 1;
}


void
seq_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;
	Relation	reln;
	Buffer		buffer;
	Page		page;
	char	   *item;
	Size		itemsz;
	xl_seq_rec *xlrec = (xl_seq_rec *) XLogRecGetData(record);
	sequence_magic *sm;

	if (info != XLOG_SEQ_LOG)
		elog(PANIC, "seq_redo: unknown op code %u", info);

	reln = XLogOpenRelation(true, RM_SEQ_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer(true, reln, 0);
	if (!BufferIsValid(buffer))
		elog(PANIC, "seq_redo: can't read block of %u/%u",
			 xlrec->node.tblNode, xlrec->node.relNode);

	page = (Page) BufferGetPage(buffer);

	/* Always reinit the page and reinstall the magic number */
	/* See comments in DefineSequence */
	PageInit((Page) page, BufferGetPageSize(buffer), sizeof(sequence_magic));
	sm = (sequence_magic *) PageGetSpecialPointer(page);
	sm->magic = SEQ_MAGIC;

	item = (char *) xlrec + sizeof(xl_seq_rec);
	itemsz = record->xl_len - sizeof(xl_seq_rec);
	itemsz = MAXALIGN(itemsz);
	if (PageAddItem(page, (Item) item, itemsz,
					FirstOffsetNumber, LP_USED) == InvalidOffsetNumber)
		elog(PANIC, "seq_redo: failed to add item to page");

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);
}

void
seq_undo(XLogRecPtr lsn, XLogRecord *record)
{
}

void
seq_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;
	xl_seq_rec *xlrec = (xl_seq_rec *) rec;

	if (info == XLOG_SEQ_LOG)
		strcat(buf, "log: ");
	else
	{
		strcat(buf, "UNKNOWN");
		return;
	}

	sprintf(buf + strlen(buf), "node %u/%u",
			xlrec->node.tblNode, xlrec->node.relNode);
}
