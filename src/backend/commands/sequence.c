/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  PostgreSQL sequences support code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/sequence.c,v 1.78 2002/05/21 22:05:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/int8.h"


#define SEQ_MAGIC	  0x1717

#ifndef INT64_IS_BUSTED
#ifdef HAVE_LL_CONSTANTS
#define SEQ_MAXVALUE	((int64) 0x7FFFFFFFFFFFFFFFLL)
#else
#define SEQ_MAXVALUE	((int64) 0x7FFFFFFFFFFFFFFF)
#endif
#else							/* INT64_IS_BUSTED */
#define SEQ_MAXVALUE	((int64) 0x7FFFFFFF)
#endif   /* INT64_IS_BUSTED */

#define SEQ_MINVALUE	(-SEQ_MAXVALUE)

/*
 * We don't want to log each fetching of a value from a sequence,
 * so we pre-log a few fetches in advance. In the event of
 * crash we can lose as much as we pre-logged.
 */
#define SEQ_LOG_VALS	32

typedef struct sequence_magic
{
	uint32		magic;
} sequence_magic;

typedef struct SeqTableData
{
	Oid			relid;
	Relation	rel;			/* NULL if rel is not open in cur xact */
	int64		cached;
	int64		last;
	int64		increment;
	struct SeqTableData *next;
} SeqTableData;

typedef SeqTableData *SeqTable;

static SeqTable seqtab = NULL;

static SeqTable init_sequence(char *caller, RangeVar *relation);
static Form_pg_sequence read_info(char *caller, SeqTable elm, Buffer *buf);
static void init_params(CreateSeqStmt *seq, Form_pg_sequence new);
static int64 get_param(DefElem *def);
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

	/* Check and set values */
	init_params(seq, &new);

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
		coldef->raw_default = NULL;
		coldef->cooked_default = NULL;
		coldef->is_not_null = false;
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

	seqoid = DefineRelation(stmt, RELKIND_SEQUENCE);

	rel = heap_open(seqoid, AccessExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* Initialize first page of relation with special magic number */

	buf = ReadBuffer(rel, P_NEW);

	if (!BufferIsValid(buf))
		elog(ERROR, "DefineSequence: ReadBuffer failed");

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
	 * to have xmin = FrozenTransactionId now.  Otherwise it would become
	 * invisible to SELECTs after 2G transactions.  It is okay to do this
	 * because if the current transaction aborts, no other xact will ever
	 * examine the sequence tuple anyway.
	 *
	 * 2. Even though heap_insert emitted a WAL log record, we have to emit
	 * an XLOG_SEQ_LOG record too, since (a) the heap_insert record will
	 * not have the right xmin, and (b) REDO of the heap_insert record
	 * would re-init page and sequence magic number would be lost.  This
	 * means two log records instead of one :-(
	 */
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	START_CRIT_SECTION();

	{
		/*
		 * Note that the "tuple" structure is still just a local tuple record
		 * created by heap_formtuple; its t_data pointer doesn't point at the
		 * disk buffer.  To scribble on the disk buffer we need to fetch the
		 * item pointer.  But do the same to the local tuple, since that will
		 * be the source for the WAL log record, below.
		 */
		ItemId		itemId;
		Item		item;

		itemId = PageGetItemId((Page) page, FirstOffsetNumber);
		item = PageGetItem((Page) page, itemId);

		((HeapTupleHeader) item)->t_xmin = FrozenTransactionId;
		((HeapTupleHeader) item)->t_infomask |= HEAP_XMIN_COMMITTED;

		tuple->t_data->t_xmin = FrozenTransactionId;
		tuple->t_data->t_infomask |= HEAP_XMIN_COMMITTED;
	}

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


Datum
nextval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	RangeVar   *sequence;
	SeqTable	elm;
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
	elm = init_sequence("nextval", sequence);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
		elog(ERROR, "%s.nextval: you don't have permissions to set sequence %s",
			 sequence->relname, sequence->relname);

	if (elm->last != elm->cached)		/* some numbers were cached */
	{
		elm->last += elm->increment;
		PG_RETURN_INT64(elm->last);
	}

	seq = read_info("nextval", elm, &buf);		/* lock page' buffer and
												 * read tuple */
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
	 * Decide whether we should emit a WAL log record.  If so, force up
	 * the fetch count to grab SEQ_LOG_VALS more values than we actually
	 * need to cache.  (These will then be usable without logging.)
	 *
	 * If this is the first nextval after a checkpoint, we must force
	 * a new WAL record to be written anyway, else replay starting from the
	 * checkpoint would fail to advance the sequence past the logged
	 * values.  In this case we may as well fetch extra values.
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
					elog(ERROR, "%s.nextval: reached MAXVALUE (" INT64_FORMAT ")",
						 sequence->relname, maxv);
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
					elog(ERROR, "%s.nextval: reached MINVALUE (" INT64_FORMAT ")",
						 sequence->relname, minv);
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
	if (logit)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];

		xlrec.node = elm->rel->rd_node;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_seq_rec);
		rdata[0].next = &(rdata[1]);

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

	if (WriteBuffer(buf) == STATUS_ERROR)
		elog(ERROR, "%s.nextval: WriteBuffer failed", sequence->relname);

	PG_RETURN_INT64(result);
}

Datum
currval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	RangeVar   *sequence;
	SeqTable	elm;
	int64		result;

	sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin,
																"currval"));

	/* open and AccessShareLock sequence */
	elm = init_sequence("currval", sequence);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		elog(ERROR, "%s.currval: you don't have permissions to read sequence %s",
			 sequence->relname, sequence->relname);

	if (elm->increment == 0)	/* nextval/read_info were not called */
		elog(ERROR, "%s.currval is not yet defined in this session",
			 sequence->relname);

	result = elm->last;

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
	Buffer		buf;
	Form_pg_sequence seq;

	/* open and AccessShareLock sequence */
	elm = init_sequence("setval", sequence);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
		elog(ERROR, "%s.setval: you don't have permissions to set sequence %s",
			 sequence->relname, sequence->relname);

	/* lock page' buffer and read tuple */
	seq = read_info("setval", elm, &buf);

	if ((next < seq->min_value) || (next > seq->max_value))
		elog(ERROR, "%s.setval: value " INT64_FORMAT " is out of bounds (" INT64_FORMAT "," INT64_FORMAT ")",
			 sequence->relname, next, seq->min_value, seq->max_value);

	/* save info in local cache */
	elm->last = next;			/* last returned number */
	elm->cached = next;			/* last cached number (forget cached
								 * values) */

	START_CRIT_SECTION();
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		XLogRecData rdata[2];
		Page		page = BufferGetPage(buf);

		xlrec.node = elm->rel->rd_node;
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_seq_rec);
		rdata[0].next = &(rdata[1]);

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

	if (WriteBuffer(buf) == STATUS_ERROR)
		elog(ERROR, "%s.setval: WriteBuffer failed", sequence->relname);
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

static Form_pg_sequence
read_info(char *caller, SeqTable elm, Buffer *buf)
{
	PageHeader	page;
	ItemId		lp;
	HeapTupleData tuple;
	sequence_magic *sm;
	Form_pg_sequence seq;

	if (elm->rel->rd_nblocks > 1)
		elog(ERROR, "%s.%s: invalid number of blocks in sequence",
			 RelationGetRelationName(elm->rel), caller);

	*buf = ReadBuffer(elm->rel, 0);
	if (!BufferIsValid(*buf))
		elog(ERROR, "%s.%s: ReadBuffer failed",
			 RelationGetRelationName(elm->rel), caller);

	LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);

	page = (PageHeader) BufferGetPage(*buf);
	sm = (sequence_magic *) PageGetSpecialPointer(page);

	if (sm->magic != SEQ_MAGIC)
		elog(ERROR, "%s.%s: bad magic (%08X)",
			 RelationGetRelationName(elm->rel), caller, sm->magic);

	lp = PageGetItemId(page, FirstOffsetNumber);
	Assert(ItemIdIsUsed(lp));
	tuple.t_data = (HeapTupleHeader) PageGetItem((Page) page, lp);

	seq = (Form_pg_sequence) GETSTRUCT(&tuple);

	elm->increment = seq->increment_by;

	return seq;
}


static SeqTable
init_sequence(char *caller, RangeVar *relation)
{
	Oid			relid = RangeVarGetRelid(relation, false);
	SeqTable	elm,
				prev = (SeqTable) NULL;
	Relation	seqrel;
	
	/* Look to see if we already have a seqtable entry for relation */
	for (elm = seqtab; elm != (SeqTable) NULL; elm = elm->next)
	{
		if (elm->relid == relid)
			break;
		prev = elm;
	}

	/* If so, and if it's already been opened in this xact, just return it */
	if (elm != (SeqTable) NULL && elm->rel != (Relation) NULL)
		return elm;

	/* Else open and check it */
	seqrel = heap_open(relid, AccessShareLock);
	if (seqrel->rd_rel->relkind != RELKIND_SEQUENCE)
		elog(ERROR, "%s.%s: %s is not a sequence",
			 relation->relname, caller, relation->relname);

	/*
	 * If elm exists but elm->rel is NULL, the seqtable entry is left over
	 * from a previous xact -- update the entry and reuse it.
	 *
	 * NOTE: seqtable entries remain in the list for the life of a backend.
	 * If the sequence itself is deleted then the entry becomes wasted memory,
	 * but it's small enough that this should not matter.
	 */ 
	if (elm != (SeqTable) NULL)
	{
		elm->rel = seqrel;
	}
	else
	{
		/*
		 * Time to make a new seqtable entry.  These entries live as long
		 * as the backend does, so we use plain malloc for them.
		 */
		elm = (SeqTable) malloc(sizeof(SeqTableData));
		if (elm == NULL)
			elog(ERROR, "Memory exhausted in init_sequence");
		elm->rel = seqrel;
		elm->relid = relid;
		elm->cached = elm->last = elm->increment = 0;
		elm->next = (SeqTable) NULL;

		if (seqtab == (SeqTable) NULL)
			seqtab = elm;
		else
			prev->next = elm;
	}

	return elm;
}


/*
 * CloseSequences
 *				is called by xact mgr at commit/abort.
 */
void
CloseSequences(void)
{
	SeqTable	elm;
	Relation	rel;

	for (elm = seqtab; elm != (SeqTable) NULL; elm = elm->next)
	{
		rel = elm->rel;
		if (rel != (Relation) NULL)		/* opened in current xact */
		{
			elm->rel = (Relation) NULL;
			heap_close(rel, AccessShareLock);
		}
	}
}


static void
init_params(CreateSeqStmt *seq, Form_pg_sequence new)
{
	DefElem    *last_value = NULL;
	DefElem    *increment_by = NULL;
	DefElem    *max_value = NULL;
	DefElem    *min_value = NULL;
	DefElem    *cache_value = NULL;
	List	   *option;

	new->is_cycled = false;
	foreach(option, seq->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "increment") == 0)
			increment_by = defel;
		else if (strcmp(defel->defname, "start") == 0)
			last_value = defel;
		else if (strcmp(defel->defname, "maxvalue") == 0)
			max_value = defel;
		else if (strcmp(defel->defname, "minvalue") == 0)
			min_value = defel;
		else if (strcmp(defel->defname, "cache") == 0)
			cache_value = defel;
		else if (strcmp(defel->defname, "cycle") == 0)
		{
			if (defel->arg != (Node *) NULL)
				elog(ERROR, "DefineSequence: CYCLE ??");
			new->is_cycled = true;
		}
		else
			elog(ERROR, "DefineSequence: option \"%s\" not recognized",
				 defel->defname);
	}

	if (increment_by == (DefElem *) NULL)		/* INCREMENT BY */
		new->increment_by = 1;
	else if ((new->increment_by = get_param(increment_by)) == 0)
		elog(ERROR, "DefineSequence: can't INCREMENT by 0");

	if (max_value == (DefElem *) NULL)	/* MAXVALUE */
	{
		if (new->increment_by > 0)
			new->max_value = SEQ_MAXVALUE;		/* ascending seq */
		else
			new->max_value = -1;	/* descending seq */
	}
	else
		new->max_value = get_param(max_value);

	if (min_value == (DefElem *) NULL)	/* MINVALUE */
	{
		if (new->increment_by > 0)
			new->min_value = 1; /* ascending seq */
		else
			new->min_value = SEQ_MINVALUE;		/* descending seq */
	}
	else
		new->min_value = get_param(min_value);

	if (new->min_value >= new->max_value)
		elog(ERROR, "DefineSequence: MINVALUE (" INT64_FORMAT ") can't be >= MAXVALUE (" INT64_FORMAT ")",
			 new->min_value, new->max_value);

	if (last_value == (DefElem *) NULL) /* START WITH */
	{
		if (new->increment_by > 0)
			new->last_value = new->min_value;	/* ascending seq */
		else
			new->last_value = new->max_value;	/* descending seq */
	}
	else
		new->last_value = get_param(last_value);

	if (new->last_value < new->min_value)
		elog(ERROR, "DefineSequence: START value (" INT64_FORMAT ") can't be < MINVALUE (" INT64_FORMAT ")",
			 new->last_value, new->min_value);
	if (new->last_value > new->max_value)
		elog(ERROR, "DefineSequence: START value (" INT64_FORMAT ") can't be > MAXVALUE (" INT64_FORMAT ")",
			 new->last_value, new->max_value);

	if (cache_value == (DefElem *) NULL)		/* CACHE */
		new->cache_value = 1;
	else if ((new->cache_value = get_param(cache_value)) <= 0)
		elog(ERROR, "DefineSequence: CACHE (" INT64_FORMAT ") can't be <= 0",
			 new->cache_value);

}

static int64
get_param(DefElem *def)
{
	if (def->arg == (Node *) NULL)
		elog(ERROR, "DefineSequence: \"%s\" value unspecified", def->defname);

	if (IsA(def->arg, Integer))
		return (int64) intVal(def->arg);

	/*
	 * Values too large for int4 will be represented as Float constants by
	 * the lexer.  Accept these if they are valid int8 strings.
	 */
	if (IsA(def->arg, Float))
		return DatumGetInt64(DirectFunctionCall1(int8in,
									 CStringGetDatum(strVal(def->arg))));

	/* Shouldn't get here unless parser messed up */
	elog(ERROR, "DefineSequence: \"%s\" value must be integer", def->defname);
	return 0;					/* not reached; keep compiler quiet */
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
