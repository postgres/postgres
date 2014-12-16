/*-------------------------------------------------------------------------
 *
 * bootstrap.c
 *	  routines to support running postgres in 'bootstrap' mode
 *	bootstrap mode is used to create the initial template database
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootstrap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "access/htup_details.h"
#include "bootstrap/bootstrap.h"
#include "catalog/index.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pg_getopt.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "postmaster/walwriter.h"
#include "replication/walreceiver.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/rel.h"
#include "utils/relmapper.h"
#include "utils/tqual.h"

uint32		bootstrap_data_checksum_version = 0;		/* No checksum */


#define ALLOC(t, c)		((t *) calloc((unsigned)(c), sizeof(t)))

static void CheckerModeMain(void);
static void BootstrapModeMain(void);
static void bootstrap_signals(void);
static void ShutdownAuxiliaryProcess(int code, Datum arg);
static Form_pg_attribute AllocateAttribute(void);
static Oid	gettype(char *type);
static void cleanup(void);

/* ----------------
 *		global variables
 * ----------------
 */

AuxProcType MyAuxProcType = NotAnAuxProcess;	/* declared in miscadmin.h */

Relation	boot_reldesc;		/* current relation descriptor */

Form_pg_attribute attrtypes[MAXATTR];	/* points to attribute info */
int			numattr;			/* number of attributes for cur. rel */


/*
 * Basic information associated with each type.  This is used before
 * pg_type is filled, so it has to cover the datatypes used as column types
 * in the core "bootstrapped" catalogs.
 *
 *		XXX several of these input/output functions do catalog scans
 *			(e.g., F_REGPROCIN scans pg_proc).  this obviously creates some
 *			order dependencies in the catalog creation process.
 */
struct typinfo
{
	char		name[NAMEDATALEN];
	Oid			oid;
	Oid			elem;
	int16		len;
	bool		byval;
	char		align;
	char		storage;
	Oid			collation;
	Oid			inproc;
	Oid			outproc;
};

static const struct typinfo TypInfo[] = {
	{"bool", BOOLOID, 0, 1, true, 'c', 'p', InvalidOid,
	F_BOOLIN, F_BOOLOUT},
	{"bytea", BYTEAOID, 0, -1, false, 'i', 'x', InvalidOid,
	F_BYTEAIN, F_BYTEAOUT},
	{"char", CHAROID, 0, 1, true, 'c', 'p', InvalidOid,
	F_CHARIN, F_CHAROUT},
	{"int2", INT2OID, 0, 2, true, 's', 'p', InvalidOid,
	F_INT2IN, F_INT2OUT},
	{"int4", INT4OID, 0, 4, true, 'i', 'p', InvalidOid,
	F_INT4IN, F_INT4OUT},
	{"float4", FLOAT4OID, 0, 4, FLOAT4PASSBYVAL, 'i', 'p', InvalidOid,
	F_FLOAT4IN, F_FLOAT4OUT},
	{"name", NAMEOID, CHAROID, NAMEDATALEN, false, 'c', 'p', InvalidOid,
	F_NAMEIN, F_NAMEOUT},
	{"regclass", REGCLASSOID, 0, 4, true, 'i', 'p', InvalidOid,
	F_REGCLASSIN, F_REGCLASSOUT},
	{"regproc", REGPROCOID, 0, 4, true, 'i', 'p', InvalidOid,
	F_REGPROCIN, F_REGPROCOUT},
	{"regtype", REGTYPEOID, 0, 4, true, 'i', 'p', InvalidOid,
	F_REGTYPEIN, F_REGTYPEOUT},
	{"text", TEXTOID, 0, -1, false, 'i', 'x', DEFAULT_COLLATION_OID,
	F_TEXTIN, F_TEXTOUT},
	{"oid", OIDOID, 0, 4, true, 'i', 'p', InvalidOid,
	F_OIDIN, F_OIDOUT},
	{"tid", TIDOID, 0, 6, false, 's', 'p', InvalidOid,
	F_TIDIN, F_TIDOUT},
	{"xid", XIDOID, 0, 4, true, 'i', 'p', InvalidOid,
	F_XIDIN, F_XIDOUT},
	{"cid", CIDOID, 0, 4, true, 'i', 'p', InvalidOid,
	F_CIDIN, F_CIDOUT},
	{"pg_node_tree", PGNODETREEOID, 0, -1, false, 'i', 'x', DEFAULT_COLLATION_OID,
	F_PG_NODE_TREE_IN, F_PG_NODE_TREE_OUT},
	{"int2vector", INT2VECTOROID, INT2OID, -1, false, 'i', 'p', InvalidOid,
	F_INT2VECTORIN, F_INT2VECTOROUT},
	{"oidvector", OIDVECTOROID, OIDOID, -1, false, 'i', 'p', InvalidOid,
	F_OIDVECTORIN, F_OIDVECTOROUT},
	{"_int4", INT4ARRAYOID, INT4OID, -1, false, 'i', 'x', InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_text", 1009, TEXTOID, -1, false, 'i', 'x', DEFAULT_COLLATION_OID,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_oid", 1028, OIDOID, -1, false, 'i', 'x', InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_char", 1002, CHAROID, -1, false, 'i', 'x', InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_aclitem", 1034, ACLITEMOID, -1, false, 'i', 'x', InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT}
};

static const int n_types = sizeof(TypInfo) / sizeof(struct typinfo);

struct typmap
{								/* a hack */
	Oid			am_oid;
	FormData_pg_type am_typ;
};

static struct typmap **Typ = NULL;
static struct typmap *Ap = NULL;

static Datum values[MAXATTR];	/* current row's attribute values */
static bool Nulls[MAXATTR];

static MemoryContext nogc = NULL;		/* special no-gc mem context */

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

static IndexList *ILHead = NULL;


/*
 *	 AuxiliaryProcessMain
 *
 *	 The main entry point for auxiliary processes, such as the bgwriter,
 *	 walwriter, walreceiver, bootstrapper and the shared memory checker code.
 *
 *	 This code is here just because of historical reasons.
 */
void
AuxiliaryProcessMain(int argc, char *argv[])
{
	char	   *progname = argv[0];
	int			flag;
	char	   *userDoption = NULL;

	/*
	 * initialize globals
	 */
	MyProcPid = getpid();

	MyStartTime = time(NULL);

	/* Compute paths, if we didn't inherit them from postmaster */
	if (my_exec_path[0] == '\0')
	{
		if (find_my_exec(progname, my_exec_path) < 0)
			elog(FATAL, "%s: could not locate my own executable path",
				 progname);
	}

	/*
	 * process command arguments
	 */

	/* Set defaults, to be overriden by explicit options below */
	if (!IsUnderPostmaster)
		InitializeGUCOptions();

	/* Ignore the initial --boot argument, if present */
	if (argc > 1 && strcmp(argv[1], "--boot") == 0)
	{
		argv++;
		argc--;
	}

	/* If no -x argument, we are a CheckerProcess */
	MyAuxProcType = CheckerProcess;

	while ((flag = getopt(argc, argv, "B:c:d:D:Fkr:x:-:")) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'D':
				userDoption = strdup(optarg);
				break;
			case 'd':
				{
					/* Turn on debugging for the bootstrap process. */
					char	   *debugstr;

					debugstr = psprintf("debug%s", optarg);
					SetConfigOption("log_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					SetConfigOption("client_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					pfree(debugstr);
				}
				break;
			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'k':
				bootstrap_data_checksum_version = PG_DATA_CHECKSUM_VERSION;
				break;
			case 'r':
				strlcpy(OutputFileName, optarg, MAXPGPATH);
				break;
			case 'x':
				MyAuxProcType = atoi(optarg);
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
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				proc_exit(1);
				break;
		}
	}

	if (argc != optind)
	{
		write_stderr("%s: invalid command-line arguments\n", progname);
		proc_exit(1);
	}

	/*
	 * Identify myself via ps
	 */
	if (IsUnderPostmaster)
	{
		const char *statmsg;

		switch (MyAuxProcType)
		{
			case StartupProcess:
				statmsg = "startup process";
				break;
			case BgWriterProcess:
				statmsg = "writer process";
				break;
			case CheckpointerProcess:
				statmsg = "checkpointer process";
				break;
			case WalWriterProcess:
				statmsg = "wal writer process";
				break;
			case WalReceiverProcess:
				statmsg = "wal receiver process";
				break;
			default:
				statmsg = "??? process";
				break;
		}
		init_ps_display(statmsg, "", "", "");
	}

	/* Acquire configuration parameters, unless inherited from postmaster */
	if (!IsUnderPostmaster)
	{
		if (!SelectConfigFiles(userDoption, progname))
			proc_exit(1);
	}

	/* Validate we have been given a reasonable-looking DataDir */
	Assert(DataDir);
	ValidatePgVersion(DataDir);

	/* Change into DataDir (if under postmaster, should be done already) */
	if (!IsUnderPostmaster)
		ChangeToDataDir();

	/* If standalone, create lockfile for data directory */
	if (!IsUnderPostmaster)
		CreateDataDirLockFile(false);

	SetProcessingMode(BootstrapProcessing);
	IgnoreSystemIndexes = true;

	/* Initialize MaxBackends (if under postmaster, was done already) */
	if (!IsUnderPostmaster)
		InitializeMaxBackends();

	BaseInit();

	/*
	 * When we are an auxiliary process, we aren't going to do the full
	 * InitPostgres pushups, but there are a couple of things that need to get
	 * lit up even in an auxiliary process.
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * Create a PGPROC so we can use LWLocks.  In the EXEC_BACKEND case,
		 * this was already done by SubPostmasterMain().
		 */
#ifndef EXEC_BACKEND
		InitAuxiliaryProcess();
#endif

		/*
		 * Assign the ProcSignalSlot for an auxiliary process.  Since it
		 * doesn't have a BackendId, the slot is statically allocated based on
		 * the auxiliary process type (MyAuxProcType).  Backends use slots
		 * indexed in the range from 1 to MaxBackends (inclusive), so we use
		 * MaxBackends + AuxProcType + 1 as the index of the slot for an
		 * auxiliary process.
		 *
		 * This will need rethinking if we ever want more than one of a
		 * particular auxiliary process type.
		 */
		ProcSignalInit(MaxBackends + MyAuxProcType + 1);

		/* finish setting up bufmgr.c */
		InitBufferPoolBackend();

		/* register a before-shutdown callback for LWLock cleanup */
		before_shmem_exit(ShutdownAuxiliaryProcess, 0);
	}

	/*
	 * XLOG operations
	 */
	SetProcessingMode(NormalProcessing);

	switch (MyAuxProcType)
	{
		case CheckerProcess:
			/* don't set signals, they're useless here */
			CheckerModeMain();
			proc_exit(1);		/* should never return */

		case BootstrapProcess:
			bootstrap_signals();
			BootStrapXLOG();
			BootstrapModeMain();
			proc_exit(1);		/* should never return */

		case StartupProcess:
			/* don't set signals, startup process has its own agenda */
			StartupProcessMain();
			proc_exit(1);		/* should never return */

		case BgWriterProcess:
			/* don't set signals, bgwriter has its own agenda */
			BackgroundWriterMain();
			proc_exit(1);		/* should never return */

		case CheckpointerProcess:
			/* don't set signals, checkpointer has its own agenda */
			CheckpointerMain();
			proc_exit(1);		/* should never return */

		case WalWriterProcess:
			/* don't set signals, walwriter has its own agenda */
			InitXLOGAccess();
			WalWriterMain();
			proc_exit(1);		/* should never return */

		case WalReceiverProcess:
			/* don't set signals, walreceiver has its own agenda */
			WalReceiverMain();
			proc_exit(1);		/* should never return */

		default:
			elog(PANIC, "unrecognized process type: %d", (int) MyAuxProcType);
			proc_exit(1);
	}
}

/*
 * In shared memory checker mode, all we really want to do is create shared
 * memory and semaphores (just to prove we can do it with the current GUC
 * settings).  Since, in fact, that was already done by BaseInit(),
 * we have nothing more to do here.
 */
static void
CheckerModeMain(void)
{
	proc_exit(0);
}

/*
 *	 The main entry point for running the backend in bootstrap mode
 *
 *	 The bootstrap mode is used to initialize the template database.
 *	 The bootstrap backend doesn't speak SQL, but instead expects
 *	 commands in a special bootstrap language.
 */
static void
BootstrapModeMain(void)
{
	int			i;

	Assert(!IsUnderPostmaster);

	SetProcessingMode(BootstrapProcessing);

	/*
	 * Do backend-like initialization for bootstrap mode
	 */
	InitProcess();

	InitPostgres(NULL, InvalidOid, NULL, NULL);

	/* Initialize stuff for bootstrap-file processing */
	for (i = 0; i < MAXATTR; i++)
	{
		attrtypes[i] = NULL;
		Nulls[i] = false;
	}

	/*
	 * Process bootstrap input.
	 */
	boot_yyparse();

	/*
	 * We should now know about all mapped relations, so it's okay to write
	 * out the initial relation mapping files.
	 */
	RelationMapFinishBootstrap();

	/* Clean up and exit */
	cleanup();
	proc_exit(0);
}


/* ----------------------------------------------------------------
 *						misc functions
 * ----------------------------------------------------------------
 */

/*
 * Set up signal handling for a bootstrap process
 */
static void
bootstrap_signals(void)
{
	if (IsUnderPostmaster)
	{
		/*
		 * If possible, make this process a group leader, so that the
		 * postmaster can signal any child processes too.
		 */
#ifdef HAVE_SETSID
		if (setsid() < 0)
			elog(FATAL, "setsid() failed: %m");
#endif

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
		 * Unblock signals (they were blocked when the postmaster forked us)
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
	}
}

/*
 * Begin shutdown of an auxiliary process.  This is approximately the equivalent
 * of ShutdownPostgres() in postinit.c.  We can't run transactions in an
 * auxiliary process, so most of the work of AbortTransaction() is not needed,
 * but we do need to make sure we've released any LWLocks we are holding.
 * (This is only critical during an error exit.)
 */
static void
ShutdownAuxiliaryProcess(int code, Datum arg)
{
	LWLockReleaseAll();
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

	if (strlen(relname) >= NAMEDATALEN)
		relname[NAMEDATALEN - 1] = '\0';

	if (Typ == NULL)
	{
		/* We can now load the pg_type data */
		rel = heap_open(TypeRelationId, NoLock);
		scan = heap_beginscan_catalog(rel, 0, NULL);
		i = 0;
		while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
			++i;
		heap_endscan(scan);
		app = Typ = ALLOC(struct typmap *, i + 1);
		while (i-- > 0)
			*app++ = ALLOC(struct typmap, 1);
		*app = NULL;
		scan = heap_beginscan_catalog(rel, 0, NULL);
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
		 relname, (int) ATTRIBUTE_FIXED_PART_SIZE);

	boot_reldesc = heap_openrv(makeRangeVar(NULL, relname, -1), NoLock);
	numattr = boot_reldesc->rd_rel->relnatts;
	for (i = 0; i < numattr; i++)
	{
		if (attrtypes[i] == NULL)
			attrtypes[i] = AllocateAttribute();
		memmove((char *) attrtypes[i],
				(char *) boot_reldesc->rd_att->attrs[i],
				ATTRIBUTE_FIXED_PART_SIZE);

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
					 name, RelationGetRelationName(boot_reldesc));
		}
		else
			elog(ERROR, "close of %s before any relation was opened",
				 name);
	}

	if (boot_reldesc == NULL)
		elog(ERROR, "no open relation to close");
	else
	{
		elog(DEBUG4, "close relation %s",
			 RelationGetRelationName(boot_reldesc));
		heap_close(boot_reldesc, NoLock);
		boot_reldesc = NULL;
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
	Oid			typeoid;

	if (boot_reldesc != NULL)
	{
		elog(WARNING, "no open relations allowed with CREATE command");
		closerel(NULL);
	}

	if (attrtypes[attnum] == NULL)
		attrtypes[attnum] = AllocateAttribute();
	MemSet(attrtypes[attnum], 0, ATTRIBUTE_FIXED_PART_SIZE);

	namestrcpy(&attrtypes[attnum]->attname, name);
	elog(DEBUG4, "column %s %s", NameStr(attrtypes[attnum]->attname), type);
	attrtypes[attnum]->attnum = attnum + 1;		/* fillatt */

	typeoid = gettype(type);

	if (Typ != NULL)
	{
		attrtypes[attnum]->atttypid = Ap->am_oid;
		attrtypes[attnum]->attlen = Ap->am_typ.typlen;
		attrtypes[attnum]->attbyval = Ap->am_typ.typbyval;
		attrtypes[attnum]->attstorage = Ap->am_typ.typstorage;
		attrtypes[attnum]->attalign = Ap->am_typ.typalign;
		attrtypes[attnum]->attcollation = Ap->am_typ.typcollation;
		/* if an array type, assume 1-dimensional attribute */
		if (Ap->am_typ.typelem != InvalidOid && Ap->am_typ.typlen < 0)
			attrtypes[attnum]->attndims = 1;
		else
			attrtypes[attnum]->attndims = 0;
	}
	else
	{
		attrtypes[attnum]->atttypid = TypInfo[typeoid].oid;
		attrtypes[attnum]->attlen = TypInfo[typeoid].len;
		attrtypes[attnum]->attbyval = TypInfo[typeoid].byval;
		attrtypes[attnum]->attstorage = TypInfo[typeoid].storage;
		attrtypes[attnum]->attalign = TypInfo[typeoid].align;
		attrtypes[attnum]->attcollation = TypInfo[typeoid].collation;
		/* if an array type, assume 1-dimensional attribute */
		if (TypInfo[typeoid].elem != InvalidOid &&
			attrtypes[attnum]->attlen < 0)
			attrtypes[attnum]->attndims = 1;
		else
			attrtypes[attnum]->attndims = 0;
	}

	attrtypes[attnum]->attstattarget = -1;
	attrtypes[attnum]->attcacheoff = -1;
	attrtypes[attnum]->atttypmod = -1;
	attrtypes[attnum]->attislocal = true;

	/*
	 * Mark as "not null" if type is fixed-width and prior columns are too.
	 * This corresponds to case where column can be accessed directly via C
	 * struct declaration.
	 *
	 * oidvector and int2vector are also treated as not-nullable, even though
	 * they are no longer fixed-width.
	 */
#define MARKNOTNULL(att) \
	((att)->attlen > 0 || \
	 (att)->atttypid == OIDVECTOROID || \
	 (att)->atttypid == INT2VECTOROID)

	if (MARKNOTNULL(attrtypes[attnum]))
	{
		int			i;

		for (i = 0; i < attnum; i++)
		{
			if (!MARKNOTNULL(attrtypes[i]))
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
	tuple = heap_form_tuple(tupDesc, values, Nulls);
	if (objectid != (Oid) 0)
		HeapTupleSetOid(tuple, objectid);
	pfree(tupDesc);				/* just free's tupDesc, not the attrtypes */

	simple_heap_insert(boot_reldesc, tuple);
	heap_freetuple(tuple);
	elog(DEBUG4, "row inserted");

	/*
	 * Reset null markers for next tuple
	 */
	for (i = 0; i < numattr; i++)
		Nulls[i] = false;
}

/* ----------------
 *		InsertOneValue
 * ----------------
 */
void
InsertOneValue(char *value, int i)
{
	Oid			typoid;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			typinput;
	Oid			typoutput;

	AssertArg(i >= 0 && i < MAXATTR);

	elog(DEBUG4, "inserting column %d value \"%s\"", i, value);

	typoid = boot_reldesc->rd_att->attrs[i]->atttypid;

	boot_get_type_io_data(typoid,
						  &typlen, &typbyval, &typalign,
						  &typdelim, &typioparam,
						  &typinput, &typoutput);

	values[i] = OidInputFunctionCall(typinput, value, typioparam, -1);

	/*
	 * We use ereport not elog here so that parameters aren't evaluated unless
	 * the message is going to be printed, which generally it isn't
	 */
	ereport(DEBUG4,
			(errmsg_internal("inserted -> %s",
							 OidOutputFunctionCall(typoutput, values[i]))));
}

/* ----------------
 *		InsertOneNull
 * ----------------
 */
void
InsertOneNull(int i)
{
	elog(DEBUG4, "inserting column %d NULL", i);
	Assert(i >= 0 && i < MAXATTR);
	values[i] = PointerGetDatum(NULL);
	Nulls[i] = true;
}

/* ----------------
 *		cleanup
 * ----------------
 */
static void
cleanup(void)
{
	if (boot_reldesc != NULL)
		closerel(NULL);
}

/* ----------------
 *		gettype
 *
 * NB: this is really ugly; it will return an integer index into TypInfo[],
 * and not an OID at all, until the first reference to a type not known in
 * TypInfo[].  At that point it will read and cache pg_type in the Typ array,
 * and subsequently return a real OID (and set the global pointer Ap to
 * point at the found row in Typ).  So caller must check whether Typ is
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

	if (Typ != NULL)
	{
		for (app = Typ; *app != NULL; app++)
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
			if (strncmp(type, TypInfo[i].name, NAMEDATALEN) == 0)
				return i;
		}
		elog(DEBUG4, "external type: %s", type);
		rel = heap_open(TypeRelationId, NoLock);
		scan = heap_beginscan_catalog(rel, 0, NULL);
		i = 0;
		while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
			++i;
		heap_endscan(scan);
		app = Typ = ALLOC(struct typmap *, i + 1);
		while (i-- > 0)
			*app++ = ALLOC(struct typmap, 1);
		*app = NULL;
		scan = heap_beginscan_catalog(rel, 0, NULL);
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
	/* not reached, here to make compiler happy */
	return 0;
}

/* ----------------
 *		boot_get_type_io_data
 *
 * Obtain type I/O information at bootstrap time.  This intentionally has
 * almost the same API as lsyscache.c's get_type_io_data, except that
 * we only support obtaining the typinput and typoutput routines, not
 * the binary I/O routines.  It is exported so that array_in and array_out
 * can be made to work during early bootstrap.
 * ----------------
 */
void
boot_get_type_io_data(Oid typid,
					  int16 *typlen,
					  bool *typbyval,
					  char *typalign,
					  char *typdelim,
					  Oid *typioparam,
					  Oid *typinput,
					  Oid *typoutput)
{
	if (Typ != NULL)
	{
		/* We have the boot-time contents of pg_type, so use it */
		struct typmap **app;
		struct typmap *ap;

		app = Typ;
		while (*app && (*app)->am_oid != typid)
			++app;
		ap = *app;
		if (ap == NULL)
			elog(ERROR, "type OID %u not found in Typ list", typid);

		*typlen = ap->am_typ.typlen;
		*typbyval = ap->am_typ.typbyval;
		*typalign = ap->am_typ.typalign;
		*typdelim = ap->am_typ.typdelim;

		/* XXX this logic must match getTypeIOParam() */
		if (OidIsValid(ap->am_typ.typelem))
			*typioparam = ap->am_typ.typelem;
		else
			*typioparam = typid;

		*typinput = ap->am_typ.typinput;
		*typoutput = ap->am_typ.typoutput;
	}
	else
	{
		/* We don't have pg_type yet, so use the hard-wired TypInfo array */
		int			typeindex;

		for (typeindex = 0; typeindex < n_types; typeindex++)
		{
			if (TypInfo[typeindex].oid == typid)
				break;
		}
		if (typeindex >= n_types)
			elog(ERROR, "type OID %u not found in TypInfo", typid);

		*typlen = TypInfo[typeindex].len;
		*typbyval = TypInfo[typeindex].byval;
		*typalign = TypInfo[typeindex].align;
		/* We assume typdelim is ',' for all boot-time types */
		*typdelim = ',';

		/* XXX this logic must match getTypeIOParam() */
		if (OidIsValid(TypInfo[typeindex].elem))
			*typioparam = TypInfo[typeindex].elem;
		else
			*typioparam = typid;

		*typinput = TypInfo[typeindex].inproc;
		*typoutput = TypInfo[typeindex].outproc;
	}
}

/* ----------------
 *		AllocateAttribute
 *
 * Note: bootstrap never sets any per-column ACLs, so we only need
 * ATTRIBUTE_FIXED_PART_SIZE space per attribute.
 * ----------------
 */
static Form_pg_attribute
AllocateAttribute(void)
{
	Form_pg_attribute attribute = (Form_pg_attribute) malloc(ATTRIBUTE_FIXED_PART_SIZE);

	if (!PointerIsValid(attribute))
		elog(FATAL, "out of memory");
	MemSet(attribute, 0, ATTRIBUTE_FIXED_PART_SIZE);

	return attribute;
}

/*
 *		MapArrayTypeName
 *
 * Given a type name, produce the corresponding array type name by prepending
 * '_' and truncating as needed to fit in NAMEDATALEN-1 bytes.  This is only
 * used in bootstrap mode, so we can get away with assuming that the input is
 * ASCII and we don't need multibyte-aware truncation.
 *
 * The given string normally ends with '[]' or '[digits]'; we discard that.
 *
 * The result is a palloc'd string.
 */
char *
MapArrayTypeName(const char *s)
{
	int			i,
				j;
	char		newStr[NAMEDATALEN];

	newStr[0] = '_';
	j = 1;
	for (i = 0; i < NAMEDATALEN - 2 && s[i] != '['; i++, j++)
		newStr[j] = s[i];

	newStr[j] = '\0';

	return pstrdup(newStr);
}


/*
 *	index_register() -- record an index that has been set up for building
 *						later.
 *
 *		At bootstrap time, we define a bunch of indexes on system catalogs.
 *		We postpone actually building the indexes until just before we're
 *		finished with initialization, however.  This is because the indexes
 *		themselves have catalog entries, and those have to be included in the
 *		indexes on those catalogs.  Doing it in two phases is the simplest
 *		way of making sure the indexes have the right contents at the end.
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
	 * bootstrap time.  we'll declare the indexes now, but want to create them
	 * later.
	 */

	if (nogc == NULL)
		nogc = AllocSetContextCreate(NULL,
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
	/* no exclusion constraints at bootstrap time, so no need to copy */
	Assert(indexInfo->ii_ExclusionOps == NULL);
	Assert(indexInfo->ii_ExclusionProcs == NULL);
	Assert(indexInfo->ii_ExclusionStrats == NULL);

	newind->il_next = ILHead;
	ILHead = newind;

	MemoryContextSwitchTo(oldcxt);
}


/*
 * build_indices -- fill in all the indexes registered earlier
 */
void
build_indices(void)
{
	for (; ILHead != NULL; ILHead = ILHead->il_next)
	{
		Relation	heap;
		Relation	ind;

		/* need not bother with locks during bootstrap */
		heap = heap_open(ILHead->il_heap, NoLock);
		ind = index_open(ILHead->il_ind, NoLock);

		index_build(heap, ind, ILHead->il_info, false, false);

		index_close(ind, NoLock);
		heap_close(heap, NoLock);
	}
}
