/*-------------------------------------------------------------------------
 *
 * bootstrap.c--
 *	  routines to support running postgres in 'bootstrap' mode
 *	bootstrap mode is used to create the initial template database
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/bootstrap/bootstrap.c,v 1.40 1998/04/26 04:06:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>				/* For getopt() */
#include <time.h>
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>

#define BOOTSTRAP_INCLUDE		/* mask out stuff in tcop/tcopprot.h */

#include "postgres.h"

#include "miscadmin.h"
#include "fmgr.h"

#include "access/attnum.h"
#include "access/funcindex.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/strat.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "executor/execdesc.h"
#include "executor/hashjoin.h"
#include "executor/tuptable.h"
#include "libpq/pqsignal.h"
#include "nodes/execnodes.h"
#include "nodes/memnodes.h"
#include "nodes/nodes.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/itemptr.h"
#include "storage/lock.h"
#include "storage/off.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/geo_decls.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/mcxt.h"
#include "utils/nabstime.h"
#include "utils/portal.h"
#include "utils/rel.h"

#ifndef HAVE_MEMMOVE
#include "regex/utils.h"
#endif

#define ALLOC(t, c)		(t *)calloc((unsigned)(c), sizeof(t))
#define FIRST_TYPE_OID 16		/* OID of the first type */

extern int	Int_yyparse(void);
static hashnode *AddStr(char *str, int strlength, int mderef);
static AttributeTupleForm AllocateAttribute(void);
static bool BootstrapAlreadySeen(Oid id);
static int	CompHash(char *str, int len);
static hashnode *FindStr(char *str, int length, hashnode *mderef);
static int	gettype(char *type);
static void cleanup(void);

/* ----------------
 *		global variables
 * ----------------
 */
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
	{"bool", 16, 0, 1, F_BOOLIN, F_BOOLOUT},
	{"bytea", 17, 0, -1, F_BYTEAIN, F_BYTEAOUT},
	{"char", 18, 0, 1, F_CHARIN, F_CHAROUT},
	{"name", 19, 0, NAMEDATALEN, F_NAMEIN, F_NAMEOUT},
	{"dummy", 20, 0, 16, 0, 0},
/*	  { "dt",		  20,	 0,  4, F_DTIN,			F_DTOUT}, */
	{"int2", 21, 0, 2, F_INT2IN, F_INT2OUT},
	{"int28", 22, 0, 16, F_INT28IN, F_INT28OUT},
	{"int4", 23, 0, 4, F_INT4IN, F_INT4OUT},
	{"regproc", 24, 0, 4, F_REGPROCIN, F_REGPROCOUT},
	{"text", 25, 0, -1, F_TEXTIN, F_TEXTOUT},
	{"oid", 26, 0, 4, F_INT4IN, F_INT4OUT},
	{"tid", 27, 0, 6, F_TIDIN, F_TIDOUT},
	{"xid", 28, 0, 5, F_XIDIN, F_XIDOUT},
	{"iid", 29, 0, 1, F_CIDIN, F_CIDOUT},
	{"oid8", 30, 0, 32, F_OID8IN, F_OID8OUT},
	{"smgr", 210, 0, 2, F_SMGRIN, F_SMGROUT},
	{"_int4", 1007, 23, -1, F_ARRAY_IN, F_ARRAY_OUT},
	{"_aclitem", 1034, 1033, -1, F_ARRAY_IN, F_ARRAY_OUT}
};

static int	n_types = sizeof(Procid) / sizeof(struct typinfo);

struct typmap
{								/* a hack */
	Oid			am_oid;
	TypeTupleFormData am_typ;
};

static struct typmap **Typ = (struct typmap **) NULL;
static struct typmap *Ap = (struct typmap *) NULL;

static int	Warnings = 0;
static char Blanks[MAXATTR];

static char *relname;			/* current relation name */

AttributeTupleForm attrtypes[MAXATTR];	/* points to attribute info */
static char *values[MAXATTR];	/* cooresponding attribute values */
int			numattr;			/* number of attributes for cur. rel */
extern int	fsyncOff;			/* do not fsync the database */

/* The test for HAVE_SIGSETJMP fails on Linux 2.0.x because the test
 *	explicitly disallows sigsetjmp being a #define, which is how it
 *	is declared in Linux. So, to avoid compiler warnings about
 *	sigsetjmp() being redefined, let's not redefine unless necessary.
 * - thomas 1997-12-27
 */

#if !defined(HAVE_SIGSETJMP) && !defined(sigsetjmp)
static jmp_buf Warn_restart;

#define sigsetjmp(x,y)	setjmp(x)
#define siglongjmp longjmp

#else
static sigjmp_buf Warn_restart;

#endif

int			DebugMode;
static GlobalMemory nogc = (GlobalMemory) NULL; /* special no-gc mem
												 * context */

extern int	optind;
extern char *optarg;

/*
 *	At bootstrap time, we first declare all the indices to be built, and
 *	then build them.  The IndexList structure stores enough information
 *	to allow us to build the indices after they've been declared.
 */

typedef struct _IndexList
{
	char	   *il_heap;
	char	   *il_ind;
	int			il_natts;
	AttrNumber *il_attnos;
	uint16		il_nparams;
	Datum	   *il_params;
	FuncIndexInfo *il_finfo;
	PredInfo   *il_predInfo;
	struct _IndexList *il_next;
} IndexList;

static IndexList *ILHead = (IndexList *) NULL;

typedef void (*sig_func) ();




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
   usage help for the bootstrap backen
*/
static void
usage(void)
{
	fprintf(stderr, "Usage: postgres -boot [-d] [-C] [-F] [-O] [-Q] ");
	fprintf(stderr, "[-P portno] [dbName]\n");
	fprintf(stderr, "     d: debug mode\n");
	fprintf(stderr, "     C: disable version checking\n");
	fprintf(stderr, "     F: turn off fsync\n");
	fprintf(stderr, "     O: set BootstrapProcessing mode\n");
	fprintf(stderr, "     P portno: specify port number\n");

	exitpg(1);
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
	int			portFd = -1;
	char	   *dbName;
	int			flag;
	int			override = 1;	/* use BootstrapProcessing or
								 * InitProcessing mode */

	extern int	optind;
	extern char *optarg;

	/* ----------------
	 *	initialize signal handlers
	 * ----------------
	 */
	pqsignal(SIGINT, (sig_func) die);
	pqsignal(SIGHUP, (sig_func) die);
	pqsignal(SIGTERM, (sig_func) die);

	/* --------------------
	 *	initialize globals
	 * -------------------
	 */

	MyProcPid = getpid();

	/* ----------------
	 *	process command arguments
	 * ----------------
	 */

	/* Set defaults, to be overriden by explicit options below */
	Quiet = 0;
	Noversion = 0;
	dbName = NULL;
	DataDir = getenv("PGDATA"); /* Null if no PGDATA variable */

	while ((flag = getopt(argc, argv, "D:dCOQP:F")) != EOF)
	{
		switch (flag)
		{
			case 'D':
				DataDir = optarg;
				break;
			case 'd':
				DebugMode = 1;	/* print out debugging info while parsing */
				break;
			case 'C':
				Noversion = 1;
				break;
			case 'F':
				fsyncOff = 1;
				break;
			case 'O':
				override = true;
				break;
			case 'Q':
				Quiet = 1;
				break;
			case 'P':			/* specify port */
				portFd = atoi(optarg);
				break;
			default:
				usage();
				break;
		}
	}							/* while */

	if (argc - optind > 1)
	{
		usage();
	}
	else if (argc - optind == 1)
	{
		dbName = argv[optind];
	}

	if (!DataDir)
	{
		fprintf(stderr, "%s does not know where to find the database system "
				"data.  You must specify the directory that contains the "
				"database system either by specifying the -D invocation "
			 "option or by setting the PGDATA environment variable.\n\n",
				argv[0]);
		exitpg(1);
	}

	if (dbName == NULL)
	{
		dbName = getenv("USER");
		if (dbName == NULL)
		{
			fputs("bootstrap backend: failed, no db name specified\n", stderr);
			fputs("          and no USER enviroment variable\n", stderr);
			exitpg(1);
		}
	}

	/* ----------------
	 *	initialize input fd
	 * ----------------
	 */
	if (IsUnderPostmaster == true && portFd < 0)
	{
		fputs("backend: failed, no -P option with -postmaster opt.\n", stderr);
		exitpg(1);
	}

	/* ----------------
	 *	backend initialization
	 * ----------------
	 */
	SetProcessingMode((override) ? BootstrapProcessing : InitProcessing);
	InitPostgres(dbName);
	LockDisable(true);

	for (i = 0; i < MAXATTR; i++)
	{
		attrtypes[i] = (AttributeTupleForm) NULL;
		Blanks[i] = ' ';
	}
	for (i = 0; i < STRTABLESIZE; ++i)
		strtable[i] = NULL;
	for (i = 0; i < HASHTABLESIZE; ++i)
		hashtable[i] = NULL;

	/* ----------------
	 *	abort processing resumes here
	 * ----------------
	 */
	pqsignal(SIGHUP, handle_warn);

	if (sigsetjmp(Warn_restart, 1) != 0)
	{
		Warnings++;
		AbortCurrentTransaction();
	}

	/* ----------------
	 *	process input.
	 * ----------------
	 */

	/*
	 * the sed script boot.sed renamed yyparse to Int_yyparse for the
	 * bootstrap parser to avoid conflicts with the normal SQL parser
	 */
	Int_yyparse();

	/* clean up processing */
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
	Relation	rdesc;
	HeapScanDesc sdesc;
	HeapTuple	tup;

	if (strlen(relname) >= NAMEDATALEN - 1)
		relname[NAMEDATALEN - 1] = '\0';

	if (Typ == (struct typmap **) NULL)
	{
		StartPortalAllocMode(DefaultAllocMode, 0);
		rdesc = heap_openr(TypeRelationName);
		sdesc = heap_beginscan(rdesc, 0, false, 0, (ScanKey) NULL);
		for (i = 0; PointerIsValid(tup = heap_getnext(sdesc, 0, (Buffer *) NULL)); ++i);
		heap_endscan(sdesc);
		app = Typ = ALLOC(struct typmap *, i + 1);
		while (i-- > 0)
			*app++ = ALLOC(struct typmap, 1);
		*app = (struct typmap *) NULL;
		sdesc = heap_beginscan(rdesc, 0, false, 0, (ScanKey) NULL);
		app = Typ;
		while (PointerIsValid(tup = heap_getnext(sdesc, 0, (Buffer *) NULL)))
		{
			(*app)->am_oid = tup->t_oid;
			memmove((char *) &(*app++)->am_typ,
					(char *) GETSTRUCT(tup),
					sizeof((*app)->am_typ));
		}
		heap_endscan(sdesc);
		heap_close(rdesc);
		EndPortalAllocMode();
	}

	if (reldesc != NULL)
	{
		closerel(NULL);
	}

	if (!Quiet)
		printf("Amopen: relation %s. attrsize %d\n", relname ? relname : "(null)",
			   (int) ATTRIBUTE_TUPLE_SIZE);

	reldesc = heap_openr(relname);
	Assert(reldesc);
	numattr = reldesc->rd_rel->relnatts;
	for (i = 0; i < numattr; i++)
	{
		if (attrtypes[i] == NULL)
		{
			attrtypes[i] = AllocateAttribute();
		}
		memmove((char *) attrtypes[i],
				(char *) reldesc->rd_att->attrs[i],
				ATTRIBUTE_TUPLE_SIZE);

		/* Some old pg_attribute tuples might not have attisset. */

		/*
		 * If the attname is attisset, don't look for it - it may not be
		 * defined yet.
		 */
		if (namestrcmp(&attrtypes[i]->attname, "attisset") == 0)
			attrtypes[i]->attisset = get_attisset(reldesc->rd_id,
											 attrtypes[i]->attname.data);
		else
			attrtypes[i]->attisset = false;

		if (DebugMode)
		{
			AttributeTupleForm at = attrtypes[i];

			printf("create attribute %d name %s len %d num %d type %d\n",
				   i, at->attname.data, at->attlen, at->attnum,
				   at->atttypid
				);
			fflush(stdout);
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
		if (reldesc)
		{
			if (namestrcmp(RelationGetRelationName(reldesc), name) != 0)
				elog(ERROR, "closerel: close of '%s' when '%s' was expected",
					 name, relname ? relname : "(null)");
		}
		else
			elog(ERROR, "closerel: close of '%s' before any relation was opened",
				 name);

	}

	if (reldesc == NULL)
	{
		elog(ERROR, "Warning: no opened relation to close.\n");
	}
	else
	{
		if (!Quiet)
			printf("Amclose: relation %s.\n", relname ? relname : "(null)");
		heap_close(reldesc);
		reldesc = (Relation) NULL;
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
	int			t;

	if (reldesc != NULL)
	{
		fputs("Warning: no open relations allowed with 't' command.\n", stderr);
		closerel(relname);
	}

	t = gettype(type);
	if (attrtypes[attnum] == (AttributeTupleForm) NULL)
		attrtypes[attnum] = AllocateAttribute();
	if (Typ != (struct typmap **) NULL)
	{
		attrtypes[attnum]->atttypid = Ap->am_oid;
		namestrcpy(&attrtypes[attnum]->attname, name);
		if (!Quiet)
			printf("<%s %s> ", attrtypes[attnum]->attname.data, type);
		attrtypes[attnum]->attnum = 1 + attnum; /* fillatt */
		attlen = attrtypes[attnum]->attlen = Ap->am_typ.typlen;
		attrtypes[attnum]->attbyval = Ap->am_typ.typbyval;
	}
	else
	{
		attrtypes[attnum]->atttypid = Procid[t].oid;
		namestrcpy(&attrtypes[attnum]->attname, name);
		if (!Quiet)
			printf("<%s %s> ", attrtypes[attnum]->attname.data, type);
		attrtypes[attnum]->attnum = 1 + attnum; /* fillatt */
		attlen = attrtypes[attnum]->attlen = Procid[t].len;
		attrtypes[attnum]->attbyval = (attlen == 1) || (attlen == 2) || (attlen == 4);
	}
	attrtypes[attnum]->attcacheoff = -1;
	attrtypes[attnum]->atttypmod = -1;
}


/* ----------------
 *		InsertOneTuple
 *		assumes that 'oid' will not be zero.
 * ----------------
 */
void
InsertOneTuple(Oid objectid)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;

	int			i;

	if (DebugMode)
	{
		printf("InsertOneTuple oid %d, %d attrs\n", objectid, numattr);
		fflush(stdout);
	}

	tupDesc = CreateTupleDesc(numattr, attrtypes);
	tuple = heap_formtuple(tupDesc, (Datum *) values, Blanks);
	pfree(tupDesc);				/* just free's tupDesc, not the attrtypes */

	if (objectid != (Oid) 0)
	{
		tuple->t_oid = objectid;
	}
	heap_insert(reldesc, tuple);
	pfree(tuple);
	if (DebugMode)
	{
		printf("End InsertOneTuple, objectid=%d\n", objectid);
		fflush(stdout);
	}

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
InsertOneValue(Oid objectid, char *value, int i)
{
	int			typeindex;
	char	   *prt;
	struct typmap **app;

	if (DebugMode)
		printf("Inserting value: '%s'\n", value);
	if (i < 0 || i >= MAXATTR)
	{
		printf("i out of range: %d\n", i);
		Assert(0);
	}

	if (Typ != (struct typmap **) NULL)
	{
		struct typmap *ap;

		if (DebugMode)
			puts("Typ != NULL");
		app = Typ;
		while (*app && (*app)->am_oid != reldesc->rd_att->attrs[i]->atttypid)
			++app;
		ap = *app;
		if (ap == NULL)
		{
			printf("Unable to find atttypid in Typ list! %d\n",
				   reldesc->rd_att->attrs[i]->atttypid
				);
			Assert(0);
		}
		values[i] = fmgr(ap->am_typ.typinput,
						 value,
						 ap->am_typ.typelem,
						 -1);	/* shouldn't have char() or varchar()
								 * types during boostrapping but just to
								 * be safe */
		prt = fmgr(ap->am_typ.typoutput, values[i],
				   ap->am_typ.typelem);
		if (!Quiet)
			printf("%s ", prt);
		pfree(prt);
	}
	else
	{
		typeindex = attrtypes[i]->atttypid - FIRST_TYPE_OID;
		if (DebugMode)
			printf("Typ == NULL, typeindex = %d idx = %d\n", typeindex, i);
		values[i] = fmgr(Procid[typeindex].inproc, value,
						 Procid[typeindex].elem, -1);
		prt = fmgr(Procid[typeindex].outproc, values[i],
				   Procid[typeindex].elem);
		if (!Quiet)
			printf("%s ", prt);
		pfree(prt);
	}
	if (DebugMode)
	{
		puts("End InsertValue");
		fflush(stdout);
	}
}

/* ----------------
 *		InsertOneNull
 * ----------------
 */
void
InsertOneNull(int i)
{
	if (DebugMode)
		printf("Inserting null\n");
	if (i < 0 || i >= MAXATTR)
	{
		elog(FATAL, "i out of range (too many attrs): %d\n", i);
	}
	values[i] = (char *) NULL;
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
	return (seenthis);
}

/* ----------------
 *		cleanup
 * ----------------
 */
static void
cleanup()
{
	static int	beenhere = 0;

	if (!beenhere)
		beenhere = 1;
	else
	{
		elog(FATAL, "Memory manager fault: cleanup called twice.\n", stderr);
		exitpg(1);
	}
	if (reldesc != (Relation) NULL)
	{
		heap_close(reldesc);
	}
	CommitTransactionCommand();
	exitpg(Warnings);
}

/* ----------------
 *		gettype
 * ----------------
 */
static int
gettype(char *type)
{
	int			i;
	Relation	rdesc;
	HeapScanDesc sdesc;
	HeapTuple	tup;
	struct typmap **app;

	if (Typ != (struct typmap **) NULL)
	{
		for (app = Typ; *app != (struct typmap *) NULL; app++)
		{
			if (strncmp((*app)->am_typ.typname.data, type, NAMEDATALEN) == 0)
			{
				Ap = *app;
				return ((*app)->am_oid);
			}
		}
	}
	else
	{
		for (i = 0; i <= n_types; i++)
		{
			if (strncmp(type, Procid[i].name, NAMEDATALEN) == 0)
			{
				return (i);
			}
		}
		if (DebugMode)
			printf("bootstrap.c: External Type: %s\n", type);
		rdesc = heap_openr(TypeRelationName);
		sdesc = heap_beginscan(rdesc, 0, false, 0, (ScanKey) NULL);
		i = 0;
		while (PointerIsValid(tup = heap_getnext(sdesc, 0, (Buffer *) NULL)))
			++i;
		heap_endscan(sdesc);
		app = Typ = ALLOC(struct typmap *, i + 1);
		while (i-- > 0)
			*app++ = ALLOC(struct typmap, 1);
		*app = (struct typmap *) NULL;
		sdesc = heap_beginscan(rdesc, 0, false, 0, (ScanKey) NULL);
		app = Typ;
		while (PointerIsValid(tup = heap_getnext(sdesc, 0, (Buffer *) NULL)))
		{
			(*app)->am_oid = tup->t_oid;
			memmove((char *) &(*app++)->am_typ,
					(char *) GETSTRUCT(tup),
					sizeof((*app)->am_typ));
		}
		heap_endscan(sdesc);
		heap_close(rdesc);
		return (gettype(type));
	}
	elog(ERROR, "Error: unknown type '%s'.\n", type);
	err_out();
	/* not reached, here to make compiler happy */
	return 0;
}

/* ----------------
 *		AllocateAttribute
 * ----------------
 */
static AttributeTupleForm		/* XXX */
AllocateAttribute()
{
	AttributeTupleForm attribute =
	(AttributeTupleForm) malloc(ATTRIBUTE_TUPLE_SIZE);

	if (!PointerIsValid(attribute))
	{
		elog(FATAL, "AllocateAttribute: malloc failed");
	}
	MemSet(attribute, 0, ATTRIBUTE_TUPLE_SIZE);

	return (attribute);
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
	{
		return (node->strnum);
	}
	else
	{
		node = AddStr(str, len, 0);
		return (node->strnum);
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
	return (strtable[ident_num]);
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

	return (result % HASHTABLESIZE);

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
			return (node);		/* no need to check */
		}
		else
		{
			node = node->next;
		}
	}
	/* Couldn't find it in the list */
	return (NULL);
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

	if (++strtable_end == STRTABLESIZE)
	{
		/* Error, string table overflow, so we Punt */
		elog(FATAL,
			 "There are too many string constants and identifiers for the compiler to handle.");


	}

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
	{
		hashtable[hashresult] = newnode;
	}
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
	return (newnode);
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
index_register(char *heap,
			   char *ind,
			   int natts,
			   AttrNumber *attnos,
			   uint16 nparams,
			   Datum *params,
			   FuncIndexInfo *finfo,
			   PredInfo *predInfo)
{
	Datum	   *v;
	IndexList  *newind;
	int			len;
	MemoryContext oldcxt;

	/*
	 * XXX mao 10/31/92 -- don't gc index reldescs, associated info at
	 * bootstrap time.	we'll declare the indices now, but want to create
	 * them later.
	 */

	if (nogc == (GlobalMemory) NULL)
		nogc = CreateGlobalMemory("BootstrapNoGC");

	oldcxt = MemoryContextSwitchTo((MemoryContext) nogc);

	newind = (IndexList *) palloc(sizeof(IndexList));
	newind->il_heap = pstrdup(heap);
	newind->il_ind = pstrdup(ind);
	newind->il_natts = natts;

	if (PointerIsValid(finfo))
		len = FIgetnArgs(finfo) * sizeof(AttrNumber);
	else
		len = natts * sizeof(AttrNumber);

	newind->il_attnos = (AttrNumber *) palloc(len);
	memmove(newind->il_attnos, attnos, len);

	if ((newind->il_nparams = nparams) > 0)
	{
		v = newind->il_params = (Datum *) palloc(2 * nparams * sizeof(Datum));
		nparams *= 2;
		while (nparams-- > 0)
		{
			*v = (Datum) palloc(strlen((char *) (*params)) + 1);
			strcpy((char *) *v++, (char *) *params++);
		}
	}
	else
	{
		newind->il_params = (Datum *) NULL;
	}

	if (finfo != (FuncIndexInfo *) NULL)
	{
		newind->il_finfo = (FuncIndexInfo *) palloc(sizeof(FuncIndexInfo));
		memmove(newind->il_finfo, finfo, sizeof(FuncIndexInfo));
	}
	else
	{
		newind->il_finfo = (FuncIndexInfo *) NULL;
	}

	if (predInfo != NULL)
	{
		newind->il_predInfo = (PredInfo *) palloc(sizeof(PredInfo));
		newind->il_predInfo->pred = predInfo->pred;
		newind->il_predInfo->oldPred = predInfo->oldPred;
	}
	else
	{
		newind->il_predInfo = NULL;
	}

	newind->il_next = ILHead;

	ILHead = newind;

	MemoryContextSwitchTo(oldcxt);
}

void
build_indices()
{
	Relation	heap;
	Relation	ind;

	for (; ILHead != (IndexList *) NULL; ILHead = ILHead->il_next)
	{
		heap = heap_openr(ILHead->il_heap);
		ind = index_openr(ILHead->il_ind);
		index_build(heap, ind, ILHead->il_natts, ILHead->il_attnos,
				 ILHead->il_nparams, ILHead->il_params, ILHead->il_finfo,
					ILHead->il_predInfo);

		/*
		 * All of the rest of this routine is needed only because in
		 * bootstrap processing we don't increment xact id's.  The normal
		 * DefineIndex code replaces a pg_class tuple with updated info
		 * including the relhasindex flag (which we need to have updated).
		 * Unfortunately, there are always two indices defined on each
		 * catalog causing us to update the same pg_class tuple twice for
		 * each catalog getting an index during bootstrap resulting in the
		 * ghost tuple problem (see heap_replace).	To get around this we
		 * change the relhasindex field ourselves in this routine keeping
		 * track of what catalogs we already changed so that we don't
		 * modify those tuples twice.  The normal mechanism for updating
		 * pg_class is disabled during bootstrap.
		 *
		 * -mer
		 */
		heap = heap_openr(ILHead->il_heap);

		if (!BootstrapAlreadySeen(heap->rd_id))
			UpdateStats(heap->rd_id, 0, true);
	}
}
