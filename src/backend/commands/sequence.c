/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  PostgreSQL sequences support code.
 *
 *-------------------------------------------------------------------------
 */

#include <ctype.h>

#include "postgres.h"

#include "access/heapam.h"
#include "commands/creatinh.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"

#define SEQ_MAGIC	  0x1717

#define SEQ_MAXVALUE	((int4)0x7FFFFFFF)
#define SEQ_MINVALUE	-(SEQ_MAXVALUE)

/*
 * We don't want to log each fetching values from sequences,
 * so we pre-log a few fetches in advance. In the event of
 * crash we can lose as much as we pre-logged.
 */
#define	SEQ_LOG_VALS	32

typedef struct sequence_magic
{
	uint32		magic;
} sequence_magic;

typedef struct SeqTableData
{
	char	   *name;
	Oid			relid;
	Relation	rel;
	int4		cached;
	int4		last;
	int4		increment;
	struct SeqTableData *next;
} SeqTableData;

typedef SeqTableData *SeqTable;

static SeqTable seqtab = NULL;

static char *get_seq_name(text *seqin);
static SeqTable init_sequence(char *caller, char *name);
static Form_pg_sequence read_info(char *caller, SeqTable elm, Buffer *buf);
static void init_params(CreateSeqStmt *seq, Form_pg_sequence new);
static int	get_param(DefElem *def);
static void do_setval(char *seqname, int32 next, bool iscalled);

/*
 * DefineSequence
 *				Creates a new sequence relation
 */
void
DefineSequence(CreateSeqStmt *seq)
{
	FormData_pg_sequence new;
	CreateStmt *stmt = makeNode(CreateStmt);
	ColumnDef  *coldef;
	TypeName   *typnam;
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
		typnam = makeNode(TypeName);
		typnam->setof = FALSE;
		typnam->arrayBounds = NULL;
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
				typnam->name = "name";
				coldef->colname = "sequence_name";
				namestrcpy(&name, seq->seqname);
				value[i - 1] = NameGetDatum(&name);
				break;
			case SEQ_COL_LASTVAL:
				typnam->name = "int4";
				coldef->colname = "last_value";
				value[i - 1] = Int32GetDatum(new.last_value);
				break;
			case SEQ_COL_INCBY:
				typnam->name = "int4";
				coldef->colname = "increment_by";
				value[i - 1] = Int32GetDatum(new.increment_by);
				break;
			case SEQ_COL_MAXVALUE:
				typnam->name = "int4";
				coldef->colname = "max_value";
				value[i - 1] = Int32GetDatum(new.max_value);
				break;
			case SEQ_COL_MINVALUE:
				typnam->name = "int4";
				coldef->colname = "min_value";
				value[i - 1] = Int32GetDatum(new.min_value);
				break;
			case SEQ_COL_CACHE:
				typnam->name = "int4";
				coldef->colname = "cache_value";
				value[i - 1] = Int32GetDatum(new.cache_value);
				break;
			case SEQ_COL_LOG:
				typnam->name = "int4";
				coldef->colname = "log_cnt";
				value[i - 1] = Int32GetDatum((int32)1);
				break;
			case SEQ_COL_CYCLE:
				typnam->name = "char";
				coldef->colname = "is_cycled";
				value[i - 1] = CharGetDatum(new.is_cycled);
				break;
			case SEQ_COL_CALLED:
				typnam->name = "char";
				coldef->colname = "is_called";
				value[i - 1] = CharGetDatum('f');
				break;
		}
		stmt->tableElts = lappend(stmt->tableElts, coldef);
	}

	stmt->relname = seq->seqname;
	stmt->inhRelnames = NIL;
	stmt->constraints = NIL;

	DefineRelation(stmt, RELKIND_SEQUENCE);

	rel = heap_openr(seq->seqname, AccessExclusiveLock);

	tupDesc = RelationGetDescr(rel);

	Assert(RelationGetNumberOfBlocks(rel) == 0);
	buf = ReadBuffer(rel, P_NEW);

	if (!BufferIsValid(buf))
		elog(ERROR, "DefineSequence: ReadBuffer failed");

	page = (PageHeader) BufferGetPage(buf);

	PageInit((Page) page, BufferGetPageSize(buf), sizeof(sequence_magic));
	sm = (sequence_magic *) PageGetSpecialPointer(page);
	sm->magic = SEQ_MAGIC;

	/* Now - form & insert sequence tuple */
	tuple = heap_formtuple(tupDesc, value, null);
	heap_insert(rel, tuple);

	if (WriteBuffer(buf) == STATUS_ERROR)
		elog(ERROR, "DefineSequence: WriteBuffer failed");

	heap_close(rel, AccessExclusiveLock);
}


Datum
nextval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	char	   *seqname = get_seq_name(seqin);
	SeqTable	elm;
	Buffer		buf;
	Form_pg_sequence seq;
	int32		incby,
				maxv,
				minv,
				cache,
				log,
				fetch,
				last;
	int32		result,
				next,
				rescnt = 0;
	bool		logit = false;

	if (pg_aclcheck(seqname, GetUserId(), ACL_WR) != ACLCHECK_OK)
		elog(ERROR, "%s.nextval: you don't have permissions to set sequence %s",
			 seqname, seqname);

	/* open and AccessShareLock sequence */
	elm = init_sequence("nextval", seqname);

	pfree(seqname);

	if (elm->last != elm->cached)		/* some numbers were cached */
	{
		elm->last += elm->increment;
		PG_RETURN_INT32(elm->last);
	}

	seq = read_info("nextval", elm, &buf);		/* lock page' buffer and
												 * read tuple */

	last = next = result = seq->last_value;
	incby = seq->increment_by;
	maxv = seq->max_value;
	minv = seq->min_value;
	fetch = cache = seq->cache_value;
	log = seq->log_cnt;

	if (seq->is_called != 't')
	{
		rescnt++;				/* last_value if not called */
		fetch--;
		log--;
	}

	if (log < fetch)
	{
		fetch = log = fetch - log + SEQ_LOG_VALS;
		logit = true;
	}

	while (fetch)		/* try to fetch cache [+ log ] numbers */
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
				if (seq->is_cycled != 't')
					elog(ERROR, "%s.nextval: reached MAXVALUE (%d)",
						 elm->name, maxv);
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
				if (seq->is_cycled != 't')
					elog(ERROR, "%s.nextval: reached MINVALUE (%d)",
						 elm->name, minv);
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
			if (rescnt == 1)		/* if it's first result - */
				result = next;		/* it's what to return */
		}
	}

	/* save info in local cache */
	elm->last = result;			/* last returned number */
	elm->cached = last;			/* last fetched number */

	START_CRIT_CODE;
	if (logit)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;

		if (fetch)		/* not all numbers were fetched */
			log -= fetch;

		xlrec.node = elm->rel->rd_node;
		xlrec.value = next;

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG|XLOG_NO_TRAN,
					(char*) &xlrec, sizeof(xlrec), NULL, 0);

		PageSetLSN(BufferGetPage(buf), recptr);
		PageSetSUI(BufferGetPage(buf), ThisStartUpID);
	}

	/* save info in sequence relation */
	seq->last_value = last;		/* last fetched number */
	Assert(log >= 0);
	seq->log_cnt = log;			/* how much is logged */
	seq->is_called = 't';
	END_CRIT_CODE;

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (WriteBuffer(buf) == STATUS_ERROR)
		elog(ERROR, "%s.nextval: WriteBuffer failed", elm->name);

	PG_RETURN_INT32(result);
}

Datum
currval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	char	   *seqname = get_seq_name(seqin);
	SeqTable	elm;
	int32		result;

	if (pg_aclcheck(seqname, GetUserId(), ACL_RD) != ACLCHECK_OK)
		elog(ERROR, "%s.currval: you don't have permissions to read sequence %s",
			 seqname, seqname);

	/* open and AccessShareLock sequence */
	elm = init_sequence("currval", seqname);

	if (elm->increment == 0)	/* nextval/read_info were not called */
		elog(ERROR, "%s.currval is not yet defined in this session",
			 seqname);

	result = elm->last;

	pfree(seqname);

	PG_RETURN_INT32(result);
}

static void
do_setval(char *seqname, int32 next, bool iscalled)
{
	SeqTable	elm;
	Buffer		buf;
	Form_pg_sequence seq;

	if (pg_aclcheck(seqname, GetUserId(), ACL_WR) != ACLCHECK_OK)
		elog(ERROR, "%s.setval: you don't have permissions to set sequence %s",
			 seqname, seqname);

	/* open and AccessShareLock sequence */
	elm = init_sequence("setval", seqname);
	seq = read_info("setval", elm, &buf);		/* lock page' buffer and
												 * read tuple */

	if ((next < seq->min_value) || (next > seq->max_value))
		elog(ERROR, "%s.setval: value %d is out of bounds (%d,%d)",
			 seqname, next, seq->min_value, seq->max_value);

	/* save info in local cache */
	elm->last = next;			/* last returned number */
	elm->cached = next;			/* last cached number (forget cached values) */

	/* save info in sequence relation */
	START_CRIT_CODE;
	seq->last_value = next;		/* last fetched number */
	seq->is_called = iscalled ? 't' : 'f';
	seq->log_cnt = (iscalled) ? 0 : 1;

	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;

		xlrec.node = elm->rel->rd_node;
		xlrec.value = next;

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_SET|XLOG_NO_TRAN,
					(char*) &xlrec, sizeof(xlrec), NULL, 0);

		PageSetLSN(BufferGetPage(buf), recptr);
		PageSetSUI(BufferGetPage(buf), ThisStartUpID);
	}
	END_CRIT_CODE;

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (WriteBuffer(buf) == STATUS_ERROR)
		elog(ERROR, "%s.setval: WriteBuffer failed", seqname);

	pfree(seqname);

}

Datum
setval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	int32		next = PG_GETARG_INT32(1);
	char	   *seqname = get_seq_name(seqin);

	do_setval(seqname, next, true);

	PG_RETURN_INT32(next);
}

Datum
setval_and_iscalled(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_P(0);
	int32		next = PG_GETARG_INT32(1);
	bool		iscalled = PG_GETARG_BOOL(2);
	char	   *seqname = get_seq_name(seqin);

	do_setval(seqname, next, iscalled);

	PG_RETURN_INT32(next);
}

/*
 * Given a 'text' parameter to a sequence function, extract the actual
 * sequence name.  We downcase the name if it's not double-quoted.
 *
 * This is a kluge, really --- should be able to write nextval(seqrel).
 */
static char *
get_seq_name(text *seqin)
{
	char	   *rawname = DatumGetCString(DirectFunctionCall1(textout,
													PointerGetDatum(seqin)));
	int			rawlen = strlen(rawname);
	char	   *seqname;

	if (rawlen >= 2 &&
		rawname[0] == '\"' && rawname[rawlen - 1] == '\"')
	{
		/* strip off quotes, keep case */
		rawname[rawlen - 1] = '\0';
		seqname = pstrdup(rawname + 1);
		pfree(rawname);
	}
	else
	{
		seqname = rawname;
		/*
		 * It's important that this match the identifier downcasing code
		 * used by backend/parser/scan.l.
		 */
		for (; *rawname; rawname++)
		{
			if (isupper((unsigned char) *rawname))
				*rawname = tolower((unsigned char) *rawname);
		}
	}
	return seqname;
}

static Form_pg_sequence
read_info(char *caller, SeqTable elm, Buffer *buf)
{
	PageHeader	page;
	ItemId		lp;
	HeapTupleData tuple;
	sequence_magic *sm;
	Form_pg_sequence seq;

	if (RelationGetNumberOfBlocks(elm->rel) != 1)
		elog(ERROR, "%s.%s: invalid number of blocks in sequence",
			 elm->name, caller);

	*buf = ReadBuffer(elm->rel, 0);
	if (!BufferIsValid(*buf))
		elog(ERROR, "%s.%s: ReadBuffer failed", elm->name, caller);

	LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);

	page = (PageHeader) BufferGetPage(*buf);
	sm = (sequence_magic *) PageGetSpecialPointer(page);

	if (sm->magic != SEQ_MAGIC)
		elog(ERROR, "%s.%s: bad magic (%08X)", elm->name, caller, sm->magic);

	lp = PageGetItemId(page, FirstOffsetNumber);
	Assert(ItemIdIsUsed(lp));
	tuple.t_data = (HeapTupleHeader) PageGetItem((Page) page, lp);

	seq = (Form_pg_sequence) GETSTRUCT(&tuple);

	elm->increment = seq->increment_by;

	return seq;

}


static SeqTable
init_sequence(char *caller, char *name)
{
	SeqTable	elm,
				prev = (SeqTable) NULL;
	Relation	seqrel;

	/* Look to see if we already have a seqtable entry for name */
	for (elm = seqtab; elm != (SeqTable) NULL; elm = elm->next)
	{
		if (strcmp(elm->name, name) == 0)
			break;
		prev = elm;
	}

	/* If so, and if it's already been opened in this xact, just return it */
	if (elm != (SeqTable) NULL && elm->rel != (Relation) NULL)
		return elm;

	/* Else open and check it */
	seqrel = heap_openr(name, AccessShareLock);
	if (seqrel->rd_rel->relkind != RELKIND_SEQUENCE)
		elog(ERROR, "%s.%s: %s is not a sequence", name, caller, name);

	if (elm != (SeqTable) NULL)
	{

		/*
		 * We are using a seqtable entry left over from a previous xact;
		 * must check for relid change.
		 */
		elm->rel = seqrel;
		if (RelationGetRelid(seqrel) != elm->relid)
		{
			elog(NOTICE, "%s.%s: sequence was re-created",
				 name, caller);
			elm->relid = RelationGetRelid(seqrel);
			elm->cached = elm->last = elm->increment = 0;
		}
	}
	else
	{

		/*
		 * Time to make a new seqtable entry.  These entries live as long
		 * as the backend does, so we use plain malloc for them.
		 */
		elm = (SeqTable) malloc(sizeof(SeqTableData));
		elm->name = malloc(strlen(name) + 1);
		strcpy(elm->name, name);
		elm->rel = seqrel;
		elm->relid = RelationGetRelid(seqrel);
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
 *				is calling by xact mgr at commit/abort.
 */
void
CloseSequences(void)
{
	SeqTable	elm;
	Relation	rel;

	for (elm = seqtab; elm != (SeqTable) NULL; elm = elm->next)
	{
		if (elm->rel != (Relation) NULL)		/* opened in current xact */
		{
			rel = elm->rel;
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

	new->is_cycled = 'f';
	foreach(option, seq->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (!strcasecmp(defel->defname, "increment"))
			increment_by = defel;
		else if (!strcasecmp(defel->defname, "start"))
			last_value = defel;
		else if (!strcasecmp(defel->defname, "maxvalue"))
			max_value = defel;
		else if (!strcasecmp(defel->defname, "minvalue"))
			min_value = defel;
		else if (!strcasecmp(defel->defname, "cache"))
			cache_value = defel;
		else if (!strcasecmp(defel->defname, "cycle"))
		{
			if (defel->arg != (Node *) NULL)
				elog(ERROR, "DefineSequence: CYCLE ??");
			new->is_cycled = 't';
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
			new->max_value = -1;/* descending seq */
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
		elog(ERROR, "DefineSequence: MINVALUE (%d) can't be >= MAXVALUE (%d)",
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
		elog(ERROR, "DefineSequence: START value (%d) can't be < MINVALUE (%d)",
			 new->last_value, new->min_value);
	if (new->last_value > new->max_value)
		elog(ERROR, "DefineSequence: START value (%d) can't be > MAXVALUE (%d)",
			 new->last_value, new->max_value);

	if (cache_value == (DefElem *) NULL)		/* CACHE */
		new->cache_value = 1;
	else if ((new->cache_value = get_param(cache_value)) <= 0)
		elog(ERROR, "DefineSequence: CACHE (%d) can't be <= 0",
			 new->cache_value);

}

static int
get_param(DefElem *def)
{
	if (def->arg == (Node *) NULL)
		elog(ERROR, "DefineSequence: \"%s\" value unspecified", def->defname);

	if (nodeTag(def->arg) == T_Integer)
		return intVal(def->arg);

	elog(ERROR, "DefineSequence: \"%s\" value must be integer", def->defname);
	return -1;
}

void seq_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8				info = record->xl_info & ~XLR_INFO_MASK;
	Relation			reln;
	Buffer				buffer;
	Page				page;
	ItemId				lp;
	HeapTupleData		tuple;
	Form_pg_sequence	seq;
	xl_seq_rec		   *xlrec;

	if (info != XLOG_SEQ_LOG && info != XLOG_SEQ_SET)
		elog(STOP, "seq_redo: unknown op code %u", info);

	xlrec = (xl_seq_rec*) XLogRecGetData(record);

	reln = XLogOpenRelation(true, RM_SEQ_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;

	buffer = XLogReadBuffer(false, reln, 0);
	if (!BufferIsValid(buffer))
		elog(STOP, "seq_redo: can't read block of %u/%u", 
			xlrec->node.tblNode, xlrec->node.relNode);

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page) ||
		((sequence_magic *) PageGetSpecialPointer(page))->magic != SEQ_MAGIC)
		elog(STOP, "seq_redo: uninitialized page of %u/%u",
			xlrec->node.tblNode, xlrec->node.relNode);

	if (XLByteLE(lsn, PageGetLSN(page)))
	{
		UnlockAndReleaseBuffer(buffer);
		return;
	}

	lp = PageGetItemId(page, FirstOffsetNumber);
	Assert(ItemIdIsUsed(lp));
	tuple.t_data = (HeapTupleHeader) PageGetItem((Page) page, lp);

	seq = (Form_pg_sequence) GETSTRUCT(&tuple);

	seq->last_value = xlrec->value;		/* last logged value */
	seq->is_called = 't';
	seq->log_cnt = 0;

	PageSetLSN(page, lsn);
	PageSetSUI(page, ThisStartUpID);
	UnlockAndWriteBuffer(buffer);

	return;
}

void seq_undo(XLogRecPtr lsn, XLogRecord *record)
{
}

void seq_desc(char *buf, uint8 xl_info, char* rec)
{
	uint8				info = xl_info & ~XLR_INFO_MASK;
	xl_seq_rec		   *xlrec = (xl_seq_rec*) rec;

	if (info == XLOG_SEQ_LOG)
		strcat(buf, "log: ");
	else if (info == XLOG_SEQ_SET)
		strcat(buf, "set: ");
	else
	{
		strcat(buf, "UNKNOWN");
		return;
	}

	sprintf(buf + strlen(buf), "node %u/%u; value %d",
		xlrec->node.tblNode, xlrec->node.relNode, xlrec->value);
}
