/*-------------------------------------------------------------------------
 *
 * bootstrap.c
 *	  routines to support running postgres in 'bootstrap' mode
 *	bootstrap mode is used to create the initial template database
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/bootstrap/bootstrap.c,v 1.166 2003/09/02 19:04:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#define BOOTSTRAP_INCLUDE		/* mask out stuff in tcop/tcopprot.h */

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xlog.h"
#include "bootstrap/bootstrap.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"


#define ALLOC(t, c)		((t *) calloc((unsigned)(c), sizeof(t)))


extern int	Int_yyparse(void);
static hashnode *AddStr(char *str, int strlength, int mderef);
static Form_pg_attribute AllocateAttribute(void);
static bool BootstrapAlreadySeen(Oid id);
static int	CompHash(char *str, int len);
static hashnode *FindStr(char *str, int length, hashnode *mderef);
static Oid	gettype(char *type);
static void cleanup(void);

/* ----------------
 *		global variables
 * ----------------
 */

Relation	boot_reldesc;		/* current relation descriptor */

/*
 * In the lexical analyzer, we need to get the reference number quickly from
 * the string, and the string from the reference number.  Thus we have
 * as our data structure a hash table, where the hashing key taken from
 * the particular string.  The hash table is chained.  One of the fields
 * of the hash table node is an index into the array of character pointers.
 * The unique index number that every string is assigned is simply the
 * position of its string pointer in the array of string pointers.
 */

#define STRTABLESIZE	10000
#define HASHTABLESIZE	503

/* Hash function numbers */
#define NUM		23
#define NUMSQR	529
#define NUMCUBE 12167

char	   *strtable[STRTABLESIZE];
hashnode   *hashtable[HASHTABLESIZE];

static int	strtable_end = -1;	/* Tells us last occupied string space */

/*-
 * Basic information associated with each type.  This is used before
 * pg_type is created.
 *
 *		XXX several of these input/output functions do catalog scans
 *			(e.g., F_REGPROCIN scans pg_proc).	this obviously creates some
 *			order dependencies in the catalog creation process.
 */
struct typinfo
{
	char		name[NAMEDATALEN];
	Oid			oid;
	Oid			elem;
	int16		len;
	Oid			inproc;
	Oid			outproc;
};

static struct typinfo Procid[] = {
	{"bool", BOOLOID, 0, 1, F_BOOLIN, F_BOOLOUT},
	{"bytea", BYTEAOID, 0, -1, F_BYTEAIN, F_BYTEAOUT},
	{"char", CHAROID, 0, 1, F_CHARIN, F_CHAROUT},
	{"name", NAMEOID, 0, NAMEDATALEN, F_NAMEIN, F_NAMEOUT},
	{"int2", INT2OID, 0, 2, F_INT2IN, F_INT2OUT},
	{"int2vector", INT2VECTOROID, 0, INDEX_MAX_KEYS * 2, F_INT2VECTORIN, F_INT2VECTOROUT},
	{"int4", INT4OID, 0, 4, F_INT4IN, F_INT4OUT},
	{"regproc", REGPROCOID, 0, 4, F_REGPROCIN, F_REGPROCOUT},
	{"regclass", REGCLASSOID, 0, 4, F_REGCLASSIN, F_REGCLASSOUT},
	{"regtype", REGTYPEOID, 0, 4, F_REGTYPEIN, F_REGTYPEOUT},
	{"text", TEXTOID, 0, -1, F_TEXTIN, F_TEXTOUT},
	{"oid", OIDOID, 0, 4, F_OIDIN, F_OIDOUT},
	{"tid", TIDOID, 0, 6, F_TIDIN, F_TIDOUT},
	{"xid", XIDOID, 0, 4, F_XIDIN, F_XIDOUT},
	{"cid", CIDOID, 0, 4, F_CIDIN, F_CIDOUT},
	{"oidvector", OIDVECTOROID, 0, INDEX_MAX_KEYS * 4, F_OIDVECTORIN, F_OIDVECTOROUT},
	{"smgr", 210, 0, 2, F_SMGRIN, F_SMGROUT},
	{"_int4", 1007, INT4OID, -1, F_ARRAY_IN, F_ARRAY_OUT},
	{"_aclitem", 1034, 1033, -1, F_ARRAY_IN, F_ARRAY_OUT}
};

static int	n_types = sizeof(Procid) / sizeof(struct typinfo);

struct typmap
{								/* a hack */
	Oid			am_oid;
	FormData_pg_type am_typ;
};

static struct typmap **Typ = (struct typmap **) NULL;
static struct typmap *Ap = (struct typmap *) NULL;

static int	Warnings = 0;
static char Blanks[MAXATTR];

static char *relname;			/* current relation name */

Form_pg_attribute attrtypes[MAXATTR];	/* points to attribute info */
static Datum values[MAXATTR];	/* corresponding attribute values */
int			numattr;			/* number of attributes for cur. rel */

static MemoryContext nogc = NULL;		/* special no-gc mem context */

extern int	optind;
extern char *optarg;

/*
 *	At bootstrap time, we first declare all the indices to be built, and
 *	then build them.  The IndexList structure stores enough information
 *	to allow us to build the indices after they've been declared.
 */

typedef struct _IndexList
{
	Oid			il_heap;
	Oid			il_ind;
	IndexInfo  *il_info;
	struct _IndexList *il_next;
} IndexList;

static IndexList *ILHead = (IndexList *) NULL;


/* ----------------------------------------------------------------
 *						misc functions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		error handling / abort routines
 * ----------------
 */
void
err_out(void)
{
	Warnings++;
	cleanup();
}

/* usage:
 *		usage help for the bootstrap backend
 */
static void
usage(void)
{
	fprintf(stderr,
			gettext("Usage:\n"
					"  postgres -boot [OPTION]... DBNAME\n"
					"  -c NAME=VALUE    set run-time parameter\n"
					"  -d 1-5           debug level\n"
					"  -D datadir       data directory\n"
					"  -F               turn off fsync\n"
					"  -o file          send debug output to file\n"
					"  -x num           internal use\n"));

	proc_exit(1);
}



int
BootstrapMain(int argc, char *argv[])
/* ----------------------------------------------------------------
 *	 The main loop for handling the backend in bootstrap mode
 *	 the bootstrap mode is used to initialize the template database
 *	 the bootstrap backend doesn't speak SQL, but instead expects
 *	 commands in a special bootstrap language.
 *
 *	 The arguments passed in to BootstrapMain are the run-time arguments
 *	 without the argument '-boot', the caller is required to have
 *	 removed -boot from the run-time args
 * ----------------------------------------------------------------
 */
{
	int			i;
	char	   *dbname;
	int			flag;
	int			xlogop = BS_XLOG_NOP;
	char	   *potential_DataDir = NULL;

	/*
	 * initialize globals
	 */

	MyProcPid = getpid();

	/*
	 * Fire up essential subsystems: error and memory management
	 *
	 * If we are running under the postmaster, this is done already.
	 */
	if (!IsUnderPostmaster /* when exec || ExecBackend */ )
		MemoryContextInit();

	/*
	 * process command arguments
	 */

	/* Set defaults, to be overriden by explicit options below */
	dbname = NULL;
	if (!IsUnderPostmaster /* when exec || ExecBackend */ )
	{
		InitializeGUCOptions();
		potential_DataDir = getenv("PGDATA");	/* Null if no PGDATA
												 * variable */
	}

	while ((flag = getopt(argc, argv, "B:c:d:D:Fo:p:x:-:")) != -1)
	{
		switch (flag)
		{
			case 'D':
				potential_DataDir = optarg;
				break;
			case 'd':
				{
					/* Turn on debugging for the bootstrap process. */
					char	   *debugstr = palloc(strlen("debug") + strlen(optarg) + 1);

					sprintf(debugstr, "debug%s", optarg);
					SetConfigOption("log_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					SetConfigOption("client_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					pfree(debugstr);
					break;
				}
				break;
			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'o':
				StrNCpy(OutputFileName, optarg, MAXPGPATH);
				break;
			case 'x':
				xlogop = atoi(optarg);
				break;
			case 'p':
				{
					/* indicates fork from postmaster */
#ifdef EXEC_BACKEND
					char	   *p;

					sscanf(optarg, "%d,%p,", &UsedShmemSegID, &UsedShmemSegAddr);
					p = strchr(optarg, ',');
					if (p)
						p = strchr(p + 1, ',');
					if (p)
						dbname = strdup(p + 1);
#else
					dbname = strdup(optarg);
#endif
					break;
				}
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					free(name);
					if (value)
						free(value);
					break;
				}
			default:
				usage();
				break;
		}
	}

	if (!dbname && argc - optind == 1)
	{
		dbname = argv[optind];
		optind++;
	}
	if (!dbname || argc != optind)
		usage();


	if (IsUnderPostmaster && ExecBackend && MyProc /* ordinary backend */ )
		AttachSharedMemoryAndSemaphores();

	if (!IsUnderPostmaster /* when exec || ExecBackend */ )
	{
		if (!potential_DataDir)
		{
			fprintf(stderr,
					gettext("%s does not know where to find the database system data.\n"
							"You must specify the directory that contains the database system\n"
							"either by specifying the -D invocation option or by setting the\n"
							"PGDATA environment variable.\n"),
					argv[0]);
			proc_exit(1);
		}
		SetDataDir(potential_DataDir);
	}

	/* Validate we have been given a reasonable-looking DataDir */
	Assert(DataDir);
	ValidatePgVersion(DataDir);

	/* Acquire configuration parameters */
	if (IsUnderPostmaster)
	{
#ifdef EXEC_BACKEND
		read_nondefault_variables();
#endif
	}
	else
		ProcessConfigFile(PGC_POSTMASTER);

	if (IsUnderPostmaster)
	{
		/*
		 * Properly accept or ignore signals the postmaster might send us
		 */
		pqsignal(SIGHUP, SIG_IGN);
		pqsignal(SIGINT, SIG_IGN);		/* ignore query-cancel */
		pqsignal(SIGTERM, die);
		pqsignal(SIGQUIT, quickdie);
		pqsignal(SIGALRM, SIG_IGN);
		pqsignal(SIGPIPE, SIG_IGN);
		pqsignal(SIGUSR1, SIG_IGN);
		pqsignal(SIGUSR2, SIG_IGN);

		/*
		 * Reset some signals that are accepted by postmaster but not here
		 */
		pqsignal(SIGCHLD, SIG_DFL);
		pqsignal(SIGTTIN, SIG_DFL);
		pqsignal(SIGTTOU, SIG_DFL);
		pqsignal(SIGCONT, SIG_DFL);
		pqsignal(SIGWINCH, SIG_DFL);

		/*
		 * Unblock signals (they were blocked when the postmaster forked
		 * us)
		 */
		PG_SETMASK(&UnBlockSig);
	}
	else
	{
		/* Set up appropriately for interactive use */
		pqsignal(SIGHUP, die);
		pqsignal(SIGINT, die);
		pqsignal(SIGTERM, die);
		pqsignal(SIGQUIT, die);

		/*
		 * Create lockfile for data directory.
		 */
		CreateDataDirLockFile(DataDir, false);
	}

	SetProcessingMode(BootstrapProcessing);
	IgnoreSystemIndexes(true);

	XLOGPathInit();

	BaseInit();

	if (IsUnderPostmaster)
		InitDummyProcess();		/* needed to get LWLocks */

	/*
	 * XLOG operations
	 */
	SetProcessingMode(NormalProcessing);

	switch (xlogop)
	{
		case BS_XLOG_NOP:
			break;

		case BS_XLOG_BOOTSTRAP:
			BootStrapXLOG();
			StartupXLOG();
			break;

		case BS_XLOG_CHECKPOINT:
			CreateDummyCaches();
			CreateCheckPoint(false, false);
			SetSavedRedoRecPtr();		/* pass redo ptr back to
										 * postmaster */
			proc_exit(0);		/* done */

		case BS_XLOG_STARTUP:
			StartupXLOG();
			LoadFreeSpaceMap();
			proc_exit(0);		/* done */

		case BS_XLOG_SHUTDOWN:
			ShutdownXLOG();
			DumpFreeSpaceMap();
			proc_exit(0);		/* done */

		default:
			elog(PANIC, "unrecognized XLOG op: %d", xlogop);
			proc_exit(0);
	}

	SetProcessingMode(BootstrapProcessing);

	/*
	 * backend initialization
	 */
	InitPostgres(dbname, NULL);

	/*
	 * In NOP mode, all we really want to do is create shared memory and
	 * semaphores (just to prove we can do it with the current GUC
	 * settings). So, quit now.
	 */
	if (xlogop == BS_XLOG_NOP)
		proc_exit(0);

	/* Initialize stuff for bootstrap-file processing */
	for (i = 0; i < MAXATTR; i++)
	{
		attrtypes[i] = (Form_pg_attribute) NULL;
		Blanks[i] = ' ';
	}
	for (i = 0; i < STRTABLESIZE; ++i)
		strtable[i] = NULL;
	for (i = 0; i < HASHTABLESIZE; ++i)
		hashtable[i] = NULL;

	/*
	 * abort processing resumes here (this is probably dead code?)
	 */
	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		Warnings++;
		AbortCurrentTransaction();
	}

	/*
	 * Process bootstrap input.
	 *
	 * the sed script boot.sed renamed yyparse to Int_yyparse for the
	 * bootstrap parser to avoid conflicts with the normal SQL parser
	 */
	Int_yyparse();

	/* Perform a checkpoint to ensure everything's down to disk */
	SetProcessingMode(NormalProcessing);
	CreateCheckPoint(true, true);
	SetProcessingMode(BootstrapProcessing);

	/* Clean up and exit */
	StartTransactionCommand();
	cleanup();

	/* not reached, here to make compiler happy */
	return 0;

}

/* ----------------------------------------------------------------
 *				MANUAL BACKEND INTERACTIVE INTERFACE COMMANDS
 * ----------------------------------------------------------------
 */

/* ----------------
 *		boot_openrel
 * ----------------
 */
void
boot_openrel(char *relname)
{
	int			i;
	struct typmap **app;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;

	if (strlen(relname) >= NAMEDATALEN - 1)
		relname[NAMEDATALEN - 1] = '\0';

	if (Typ == (struct typmap **) NULL)
	{
		rel = heap_openr(TypeRelationName, NoLock);
		scan = heap_beginscan(rel, SnapshotNow, 0, (ScanKey) NULL);
		i = 0;
		while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
			++i;
		heap_endscan(scan);
		app = Typ = ALLOC(struct typmap *, i + 1);
		while (i-- > 0)
			*app++ = ALLOC(struct typmap, 1);
		*app = (struct typmap *) NULL;
		scan = heap_beginscan(rel, SnapshotNow, 0, (ScanKey) NULL);
		app = Typ;
		while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			(*app)->am_oid = HeapTupleGetOid(tup);
			memcpy((char *) &(*app)->am_typ,
				   (char *) GETSTRUCT(tup),
				   sizeof((*app)->am_typ));
			app++;
		}
		heap_endscan(scan);
		heap_close(rel, NoLock);
	}

	if (boot_reldesc != NULL)
		closerel(NULL);

	elog(DEBUG4, "open relation %s, attrsize %d",
		 relname ? relname : "(null)",
		 (int) ATTRIBUTE_TUPLE_SIZE);

	boot_reldesc = heap_openr(relname, NoLock);
	numattr = boot_reldesc->rd_rel->relnatts;
	for (i = 0; i < numattr; i++)
	{
		if (attrtypes[i] == NULL)
			attrtypes[i] = AllocateAttribute();
		memmove((char *) attrtypes[i],
				(char *) boot_reldesc->rd_att->attrs[i],
				ATTRIBUTE_TUPLE_SIZE);

		{
			Form_pg_attribute at = attrtypes[i];

			elog(DEBUG4, "create attribute %d name %s len %d num %d type %u",
				 i, NameStr(at->attname), at->attlen, at->attnum,
				 at->atttypid);
		}
	}
}

/* ----------------
 *		closerel
 * ----------------
 */
void
closerel(char *name)
{
	if (name)
	{
		if (boot_reldesc)
		{
			if (strcmp(RelationGetRelationName(boot_reldesc), name) != 0)
				elog(ERROR, "close of %s when %s was expected",
					 name, relname ? relname : "(null)");
		}
		else
			elog(ERROR, "close of %s before any relation was opened",
				 name);
	}

	if (boot_reldesc == NULL)
		elog(ERROR, "no open relation to close");
	else
	{
		elog(DEBUG4, "close relation %s", relname ? relname : "(null)");
		heap_close(boot_reldesc, NoLock);
		boot_reldesc = (Relation) NULL;
	}
}



/* ----------------
 * DEFINEATTR()
 *
 * define a <field,type> pair
 * if there are n fields in a relation to be created, this routine
 * will be called n times
 * ----------------
 */
void
DefineAttr(char *name, char *type, int attnum)
{
	int			attlen;
	Oid			typeoid;

	if (boot_reldesc != NULL)
	{
		elog(WARNING, "no open relations allowed with CREATE command");
		closerel(relname);
	}

	if (attrtypes[attnum] == (Form_pg_attribute) NULL)
		attrtypes[attnum] = AllocateAttribute();
	MemSet(attrtypes[attnum], 0, ATTRIBUTE_TUPLE_SIZE);

	namestrcpy(&attrtypes[attnum]->attname, name);
	elog(DEBUG4, "column %s %s", NameStr(attrtypes[attnum]->attname), type);
	attrtypes[attnum]->attnum = attnum + 1;		/* fillatt */

	typeoid = gettype(type);

	if (Typ != (struct typmap **) NULL)
	{
		attrtypes[attnum]->atttypid = Ap->am_oid;
		attlen = attrtypes[attnum]->attlen = Ap->am_typ.typlen;
		attrtypes[attnum]->attbyval = Ap->am_typ.typbyval;
		attrtypes[attnum]->attstorage = Ap->am_typ.typstorage;
		attrtypes[attnum]->attalign = Ap->am_typ.typalign;
	}
	else
	{
		attrtypes[attnum]->atttypid = Procid[typeoid].oid;
		attlen = attrtypes[attnum]->attlen = Procid[typeoid].len;

		/*
		 * Cheat like mad to fill in these items from the length only.
		 * This only has to work for types that appear in Procid[].
		 */
		switch (attlen)
		{
			case 1:
				attrtypes[attnum]->attbyval = true;
				attrtypes[attnum]->attstorage = 'p';
				attrtypes[attnum]->attalign = 'c';
				break;
			case 2:
				attrtypes[attnum]->attbyval = true;
				attrtypes[attnum]->attstorage = 'p';
				attrtypes[attnum]->attalign = 's';
				break;
			case 4:
				attrtypes[attnum]->attbyval = true;
				attrtypes[attnum]->attstorage = 'p';
				attrtypes[attnum]->attalign = 'i';
				break;
			case -1:
				attrtypes[attnum]->attbyval = false;
				attrtypes[attnum]->attstorage = 'x';
				attrtypes[attnum]->attalign = 'i';
				break;
			default:
				/* TID and fixed-length arrays, such as oidvector */
				attrtypes[attnum]->attbyval = false;
				attrtypes[attnum]->attstorage = 'p';
				attrtypes[attnum]->attalign = 'i';
				break;
		}
	}
	attrtypes[attnum]->attcacheoff = -1;
	attrtypes[attnum]->atttypmod = -1;
	attrtypes[attnum]->attislocal = true;

	/*
	 * Mark as "not null" if type is fixed-width and prior columns are
	 * too. This corresponds to case where column can be accessed directly
	 * via C struct declaration.
	 */
	if (attlen > 0)
	{
		int			i;

		for (i = 0; i < attnum; i++)
		{
			if (attrtypes[i]->attlen <= 0)
				break;
		}
		if (i == attnum)
			attrtypes[attnum]->attnotnull = true;
	}
}


/* ----------------
 *		InsertOneTuple
 *
 * If objectid is not zero, it is a specific OID to assign to the tuple.
 * Otherwise, an OID will be assigned (if necessary) by heap_insert.
 * ----------------
 */
void
InsertOneTuple(Oid objectid)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	int			i;

	elog(DEBUG4, "inserting row oid %u, %d columns", objectid, numattr);

	tupDesc = CreateTupleDesc(numattr,
							  RelationGetForm(boot_reldesc)->relhasoids,
							  attrtypes);
	tuple = heap_formtuple(tupDesc, values, Blanks);
	if (objectid != (Oid) 0)
		HeapTupleSetOid(tuple, objectid);
	pfree(tupDesc);				/* just free's tupDesc, not the attrtypes */

	simple_heap_insert(boot_reldesc, tuple);
	heap_freetuple(tuple);
	elog(DEBUG4, "row inserted");

	/*
	 * Reset blanks for next tuple
	 */
	for (i = 0; i < numattr; i++)
		Blanks[i] = ' ';
}

/* ----------------
 *		InsertOneValue
 * ----------------
 */
void
InsertOneValue(char *value, int i)
{
	int			typeindex;
	char	   *prt;
	struct typmap **app;

	AssertArg(i >= 0 || i < MAXATTR);

	elog(DEBUG4, "inserting column %d value \"%s\"", i, value);

	if (Typ != (struct typmap **) NULL)
	{
		struct typmap *ap;

		elog(DEBUG4, "Typ != NULL");
		app = Typ;
		while (*app && (*app)->am_oid != boot_reldesc->rd_att->attrs[i]->atttypid)
			++app;
		ap = *app;
		if (ap == NULL)
		{
			elog(FATAL, "could not find atttypid %u in Typ list",
				 boot_reldesc->rd_att->attrs[i]->atttypid);
		}
		values[i] = OidFunctionCall3(ap->am_typ.typinput,
									 CStringGetDatum(value),
									 ObjectIdGetDatum(ap->am_typ.typelem),
									 Int32GetDatum(-1));
		prt = DatumGetCString(OidFunctionCall3(ap->am_typ.typoutput,
											   values[i],
									ObjectIdGetDatum(ap->am_typ.typelem),
											   Int32GetDatum(-1)));
		elog(DEBUG4, " -> %s", prt);
		pfree(prt);
	}
	else
	{
		for (typeindex = 0; typeindex < n_types; typeindex++)
		{
			if (Procid[typeindex].oid == attrtypes[i]->atttypid)
				break;
		}
		if (typeindex >= n_types)
			elog(ERROR, "type oid %u not found", attrtypes[i]->atttypid);
		elog(DEBUG4, "Typ == NULL, typeindex = %u", typeindex);
		values[i] = OidFunctionCall3(Procid[typeindex].inproc,
									 CStringGetDatum(value),
								ObjectIdGetDatum(Procid[typeindex].elem),
									 Int32GetDatum(-1));
		prt = DatumGetCString(OidFunctionCall3(Procid[typeindex].outproc,
											   values[i],
								ObjectIdGetDatum(Procid[typeindex].elem),
											   Int32GetDatum(-1)));
		elog(DEBUG4, " -> %s", prt);
		pfree(prt);
	}
	elog(DEBUG4, "inserted");
}

/* ----------------
 *		InsertOneNull
 * ----------------
 */
void
InsertOneNull(int i)
{
	elog(DEBUG4, "inserting column %d NULL", i);
	Assert(i >= 0 || i < MAXATTR);
	values[i] = PointerGetDatum(NULL);
	Blanks[i] = 'n';
}

#define MORE_THAN_THE_NUMBER_OF_CATALOGS 256

static bool
BootstrapAlreadySeen(Oid id)
{
	static Oid	seenArray[MORE_THAN_THE_NUMBER_OF_CATALOGS];
	static int	nseen = 0;
	bool		seenthis;
	int			i;

	seenthis = false;

	for (i = 0; i < nseen; i++)
	{
		if (seenArray[i] == id)
		{
			seenthis = true;
			break;
		}
	}
	if (!seenthis)
	{
		seenArray[nseen] = id;
		nseen++;
	}
	return seenthis;
}

/* ----------------
 *		cleanup
 * ----------------
 */
static void
cleanup(void)
{
	static int	beenhere = 0;

	if (!beenhere)
		beenhere = 1;
	else
	{
		elog(FATAL, "cleanup called twice");
		proc_exit(1);
	}
	if (boot_reldesc != NULL)
		closerel(NULL);
	CommitTransactionCommand();
	proc_exit(Warnings ? 1 : 0);
}

/* ----------------
 *		gettype
 *
 * NB: this is really ugly; it will return an integer index into Procid[],
 * and not an OID at all, until the first reference to a type not known in
 * Procid[].  At that point it will read and cache pg_type in the Typ array,
 * and subsequently return a real OID (and set the global pointer Ap to
 * point at the found row in Typ).	So caller must check whether Typ is
 * still NULL to determine what the return value is!
 * ----------------
 */
static Oid
gettype(char *type)
{
	int			i;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;
	struct typmap **app;

	if (Typ != (struct typmap **) NULL)
	{
		for (app = Typ; *app != (struct typmap *) NULL; app++)
		{
			if (strncmp(NameStr((*app)->am_typ.typname), type, NAMEDATALEN) == 0)
			{
				Ap = *app;
				return (*app)->am_oid;
			}
		}
	}
	else
	{
		for (i = 0; i < n_types; i++)
		{
			if (strncmp(type, Procid[i].name, NAMEDATALEN) == 0)
				return i;
		}
		elog(DEBUG4, "external type: %s", type);
		rel = heap_openr(TypeRelationName, NoLock);
		scan = heap_beginscan(rel, SnapshotNow, 0, (ScanKey) NULL);
		i = 0;
		while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
			++i;
		heap_endscan(scan);
		app = Typ = ALLOC(struct typmap *, i + 1);
		while (i-- > 0)
			*app++ = ALLOC(struct typmap, 1);
		*app = (struct typmap *) NULL;
		scan = heap_beginscan(rel, SnapshotNow, 0, (ScanKey) NULL);
		app = Typ;
		while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			(*app)->am_oid = HeapTupleGetOid(tup);
			memmove((char *) &(*app++)->am_typ,
					(char *) GETSTRUCT(tup),
					sizeof((*app)->am_typ));
		}
		heap_endscan(scan);
		heap_close(rel, NoLock);
		return gettype(type);
	}
	elog(ERROR, "unrecognized type \"%s\"", type);
	err_out();
	/* not reached, here to make compiler happy */
	return 0;
}

/* ----------------
 *		AllocateAttribute
 * ----------------
 */
static Form_pg_attribute
AllocateAttribute(void)
{
	Form_pg_attribute attribute = (Form_pg_attribute) malloc(ATTRIBUTE_TUPLE_SIZE);

	if (!PointerIsValid(attribute))
		elog(FATAL, "out of memory");
	MemSet(attribute, 0, ATTRIBUTE_TUPLE_SIZE);

	return attribute;
}

/* ----------------
 *		MapArrayTypeName
 * XXX arrays of "basetype" are always "_basetype".
 *	   this is an evil hack inherited from rel. 3.1.
 * XXX array dimension is thrown away because we
 *	   don't support fixed-dimension arrays.  again,
 *	   sickness from 3.1.
 *
 * the string passed in must have a '[' character in it
 *
 * the string returned is a pointer to static storage and should NOT
 * be freed by the CALLER.
 * ----------------
 */
char *
MapArrayTypeName(char *s)
{
	int			i,
				j;
	static char newStr[NAMEDATALEN];	/* array type names < NAMEDATALEN
										 * long */

	if (s == NULL || s[0] == '\0')
		return s;

	j = 1;
	newStr[0] = '_';
	for (i = 0; i < NAMEDATALEN - 1 && s[i] != '['; i++, j++)
		newStr[j] = s[i];

	newStr[j] = '\0';

	return newStr;
}

/* ----------------
 *		EnterString
 *		returns the string table position of the identifier
 *		passed to it.  We add it to the table if we can't find it.
 * ----------------
 */
int
EnterString(char *str)
{
	hashnode   *node;
	int			len;

	len = strlen(str);

	node = FindStr(str, len, 0);
	if (node)
		return node->strnum;
	else
	{
		node = AddStr(str, len, 0);
		return node->strnum;
	}
}

/* ----------------
 *		LexIDStr
 *		when given an idnum into the 'string-table' return the string
 *		associated with the idnum
 * ----------------
 */
char *
LexIDStr(int ident_num)
{
	return strtable[ident_num];
}


/* ----------------
 *		CompHash
 *
 *		Compute a hash function for a given string.  We look at the first,
 *		the last, and the middle character of a string to try to get spread
 *		the strings out.  The function is rather arbitrary, except that we
 *		are mod'ing by a prime number.
 * ----------------
 */
static int
CompHash(char *str, int len)
{
	int			result;

	result = (NUM * str[0] + NUMSQR * str[len - 1] + NUMCUBE * str[(len - 1) / 2]);

	return result % HASHTABLESIZE;

}

/* ----------------
 *		FindStr
 *
 *		This routine looks for the specified string in the hash
 *		table.	It returns a pointer to the hash node found,
 *		or NULL if the string is not in the table.
 * ----------------
 */
static hashnode *
FindStr(char *str, int length, hashnode *mderef)
{
	hashnode   *node;

	node = hashtable[CompHash(str, length)];
	while (node != NULL)
	{
		/*
		 * We must differentiate between string constants that might have
		 * the same value as a identifier and the identifier itself.
		 */
		if (!strcmp(str, strtable[node->strnum]))
		{
			return node;		/* no need to check */
		}
		else
			node = node->next;
	}
	/* Couldn't find it in the list */
	return NULL;
}

/* ----------------
 *		AddStr
 *
 *		This function adds the specified string, along with its associated
 *		data, to the hash table and the string table.  We return the node
 *		so that the calling routine can find out the unique id that AddStr
 *		has assigned to this string.
 * ----------------
 */
static hashnode *
AddStr(char *str, int strlength, int mderef)
{
	hashnode   *temp,
			   *trail,
			   *newnode;
	int			hashresult;
	int			len;

	if (++strtable_end >= STRTABLESIZE)
		elog(FATAL, "bootstrap string table overflow");

	/*
	 * Some of the utilites (eg, define type, create relation) assume that
	 * the string they're passed is a NAMEDATALEN.  We get array bound
	 * read violations from purify if we don't allocate at least
	 * NAMEDATALEN bytes for strings of this sort.	Because we're lazy, we
	 * allocate at least NAMEDATALEN bytes all the time.
	 */

	if ((len = strlength + 1) < NAMEDATALEN)
		len = NAMEDATALEN;

	strtable[strtable_end] = malloc((unsigned) len);
	strcpy(strtable[strtable_end], str);

	/* Now put a node in the hash table */

	newnode = (hashnode *) malloc(sizeof(hashnode) * 1);
	newnode->strnum = strtable_end;
	newnode->next = NULL;

	/* Find out where it goes */

	hashresult = CompHash(str, strlength);
	if (hashtable[hashresult] == NULL)
		hashtable[hashresult] = newnode;
	else
	{							/* There is something in the list */
		trail = hashtable[hashresult];
		temp = trail->next;
		while (temp != NULL)
		{
			trail = temp;
			temp = temp->next;
		}
		trail->next = newnode;
	}
	return newnode;
}



/*
 *	index_register() -- record an index that has been set up for building
 *						later.
 *
 *		At bootstrap time, we define a bunch of indices on system catalogs.
 *		We postpone actually building the indices until just before we're
 *		finished with initialization, however.	This is because more classes
 *		and indices may be defined, and we want to be sure that all of them
 *		are present in the index.
 */
void
index_register(Oid heap,
			   Oid ind,
			   IndexInfo *indexInfo)
{
	IndexList  *newind;
	MemoryContext oldcxt;

	/*
	 * XXX mao 10/31/92 -- don't gc index reldescs, associated info at
	 * bootstrap time.	we'll declare the indices now, but want to create
	 * them later.
	 */

	if (nogc == NULL)
		nogc = AllocSetContextCreate((MemoryContext) NULL,
									 "BootstrapNoGC",
									 ALLOCSET_DEFAULT_MINSIZE,
									 ALLOCSET_DEFAULT_INITSIZE,
									 ALLOCSET_DEFAULT_MAXSIZE);

	oldcxt = MemoryContextSwitchTo(nogc);

	newind = (IndexList *) palloc(sizeof(IndexList));
	newind->il_heap = heap;
	newind->il_ind = ind;
	newind->il_info = (IndexInfo *) palloc(sizeof(IndexInfo));

	memcpy(newind->il_info, indexInfo, sizeof(IndexInfo));
	/* expressions will likely be null, but may as well copy it */
	newind->il_info->ii_Expressions = (List *)
		copyObject(indexInfo->ii_Expressions);
	newind->il_info->ii_ExpressionsState = NIL;
	/* predicate will likely be null, but may as well copy it */
	newind->il_info->ii_Predicate = (List *)
		copyObject(indexInfo->ii_Predicate);
	newind->il_info->ii_PredicateState = NIL;

	newind->il_next = ILHead;
	ILHead = newind;

	MemoryContextSwitchTo(oldcxt);
}

void
build_indices()
{
	for (; ILHead != (IndexList *) NULL; ILHead = ILHead->il_next)
	{
		Relation	heap;
		Relation	ind;

		heap = heap_open(ILHead->il_heap, NoLock);
		ind = index_open(ILHead->il_ind);
		index_build(heap, ind, ILHead->il_info);

		/*
		 * In normal processing mode, index_build would close the heap and
		 * index, but in bootstrap mode it will not.
		 */

		/*
		 * All of the rest of this routine is needed only because in
		 * bootstrap processing we don't increment xact id's.  The normal
		 * DefineIndex code replaces a pg_class tuple with updated info
		 * including the relhasindex flag (which we need to have updated).
		 * Unfortunately, there are always two indices defined on each
		 * catalog causing us to update the same pg_class tuple twice for
		 * each catalog getting an index during bootstrap resulting in the
		 * ghost tuple problem (see heap_update).	To get around this we
		 * change the relhasindex field ourselves in this routine keeping
		 * track of what catalogs we already changed so that we don't
		 * modify those tuples twice.  The normal mechanism for updating
		 * pg_class is disabled during bootstrap.
		 *
		 * -mer
		 */
		if (!BootstrapAlreadySeen(RelationGetRelid(heap)))
			UpdateStats(RelationGetRelid(heap), 0);

		/* XXX Probably we ought to close the heap and index here? */
	}
}
