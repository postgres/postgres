#include <setjmp.h>

FILE * single_mode_feed = NULL;
volatile bool inloop = false;
volatile sigjmp_buf local_sigjmp_buf;
bool repl = false;

__attribute__((export_name("pgl_shutdown")))
void
pg_shutdown() {
    PDEBUG("# 11:" __FILE__": pg_shutdown");
    proc_exit(66);
}


void
interactive_file() {
	int			firstchar = 0;
	int			c = 0;				/* character read from getc() */
	StringInfoData input_message;
	StringInfoData *inBuf;
    FILE *stream ;

	/*
	 * At top of loop, reset extended-query-message flag, so that any
	 * errors encountered in "idle" state don't provoke skip.
	 */
	doing_extended_query_message = false;

	/*
	 * Release storage left over from prior query cycle, and create a new
	 * query input buffer in the cleared MessageContext.
	 */
	MemoryContextSwitchTo(MessageContext);
	MemoryContextResetAndDeleteChildren(MessageContext);

	initStringInfo(&input_message);
    inBuf = &input_message;
	DoingCommandRead = true;

    stream = single_mode_feed;

    while (c!=EOF) {
    	resetStringInfo(inBuf);
	    while ((c = getc(stream)) != EOF) {
		    if (c == '\n')
		    {
			    if (UseSemiNewlineNewline)
			    {
				    /*
				        * In -j mode, semicolon followed by two newlines ends the
				        * command; otherwise treat newline as regular character.
				        */
				    if (inBuf->len > 1 &&
					    inBuf->data[inBuf->len - 1] == '\n' &&
					    inBuf->data[inBuf->len - 2] == ';')
				    {
					    /* might as well drop the second newline */
					    break;
				    }
			    }
			    else
			    {
				    /*
				        * In plain mode, newline ends the command unless preceded by
				        * backslash.
				        */
				    if (inBuf->len > 0 &&
					    inBuf->data[inBuf->len - 1] == '\\')
				    {
					    /* discard backslash from inBuf */
					    inBuf->data[--inBuf->len] = '\0';
					    /* discard newline too */
					    continue;
				    }
				    else
				    {
					    /* keep the newline character, but end the command */
					    appendStringInfoChar(inBuf, '\n');
					    break;
				    }
			    }
		    }

		    /* Not newline, or newline treated as regular character */
		    appendStringInfoChar(inBuf, (char) c);
        }


        if (c == EOF && inBuf->len == 0)
            return;

        /* Add '\0' to make it look the same as message case. */
        appendStringInfoChar(inBuf, (char) '\0');
        firstchar = 'Q';
PDEBUG(inBuf->data);

// ???
        if (ignore_till_sync && firstchar != EOF)
            continue;

        #include "pg_proto.c"
    }
}

void
RePostgresSingleUserMain(int single_argc, char *single_argv[], const char *username)
{
#if PGDEBUG
printf("# 123: RePostgresSingleUserMain progname=%s for %s feed=%s\n", progname, single_argv[0], IDB_PIPE_SINGLE);
#endif
    single_mode_feed = fopen(IDB_PIPE_SINGLE, "r");

    // should be template1.
    const char *dbname = NULL;


    /* Parse command-line options. */
    process_postgres_switches(single_argc, single_argv, PGC_POSTMASTER, &dbname);
#if PGDEBUG
printf("# 134: dbname=%s\n", dbname);
#endif
    LocalProcessControlFile(false);

    process_shared_preload_libraries();

//	                InitializeMaxBackends();

// ? IgnoreSystemIndexes = true;
IgnoreSystemIndexes = false;
    process_shmem_requests();

    InitializeShmemGUCs();

    InitializeWalConsistencyChecking();

    PgStartTime = GetCurrentTimestamp();

    SetProcessingMode(InitProcessing);
PDEBUG("# 153: Re-InitPostgres");
if (am_walsender)
    PDEBUG("# 155: am_walsender == true");
//      BaseInit();

    InitPostgres(dbname, InvalidOid,	/* database to connect to */
                 username, InvalidOid,	/* role to connect as */
                 (!am_walsender) ? INIT_PG_LOAD_SESSION_LIBS : 0,
                 NULL);			/* no out_dbname */

PDEBUG("# 164:" __FILE__);

    SetProcessingMode(NormalProcessing);

    BeginReportingGUCOptions();

    if (IsUnderPostmaster && Log_disconnections)
        on_proc_exit(log_disconnections, 0);

    pgstat_report_connect(MyDatabaseId);

    /* Perform initialization specific to a WAL sender process. */
    if (am_walsender)
        InitWalSender();

#if PGDEBUG
    whereToSendOutput = DestDebug;
#endif

    if (whereToSendOutput == DestDebug)
        printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

    /*
     * Create the memory context we will use in the main loop.
     *
     * MessageContext is reset once per iteration of the main loop, ie, upon
     * completion of processing of each command message from the client.
     */
    MessageContext = AllocSetContextCreate(TopMemoryContext,
						                   "MessageContext",
						                   ALLOCSET_DEFAULT_SIZES);

    /*
     * Create memory context and buffer used for RowDescription messages. As
     * SendRowDescriptionMessage(), via exec_describe_statement_message(), is
     * frequently executed for ever single statement, we don't want to
     * allocate a separate buffer every time.
     */
    row_description_context = AllocSetContextCreate(TopMemoryContext,
									                "RowDescriptionContext",
									                ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(row_description_context);
    initStringInfo(&row_description_buf);
    MemoryContextSwitchTo(TopMemoryContext);

#if defined(__wasi__)
    puts("# 210: sjlj exception handler off in initdb-wasi");
#else
#   define INITDB_SINGLE
#   include "pgl_sjlj.c"
#   undef INITDB_SINGLE
#endif // sjlj

    if (!ignore_till_sync)
        send_ready_for_query = true;	/* initially, or after error */
/*
    if (!inloop) {
        inloop = true;
        PDEBUG("# 335: REPL(initdb-single):Begin " __FILE__ );

        while (repl) { interactive_file(); }
    } else {
        // signal error
        optind = -1;
    }
*/

  interactive_file();
  fclose(single_mode_feed);
  single_mode_feed = NULL;

/*
    while (repl) { interactive_file(); }
    PDEBUG("# 232: REPL:End Raising a 'RuntimeError Exception' to halt program NOW");
    {
        void (*npe)() = NULL;
        npe();
    }
    // unreachable.
*/

    PDEBUG("# 240: no line-repl requested, exiting and keeping runtime alive");
}




void
AsyncPostgresSingleUserMain(int argc, char *argv[], const char *username, int async_restart)
{
	const char *dbname = NULL;
PDEBUG("# 254:"__FILE__);

	/* Initialize startup process environment. */
	InitStandaloneProcess(argv[0]);
PDEBUG("# 254:"__FILE__);
	/* Set default values for command-line options.	 */
	InitializeGUCOptions();
PDEBUG("# 257:"__FILE__);
	/* Parse command-line options. */
	process_postgres_switches(argc, argv, PGC_POSTMASTER, &dbname);
PDEBUG("# 260:"__FILE__);
	/* Must have gotten a database name, or have a default (the username) */
	if (dbname == NULL)
	{
		dbname = username;
		if (dbname == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s: no database nor user name specified",
							progname)));
	}

if (async_restart) goto async_db_change;
PDEBUG("# 273:SelectConfigFiles "__FILE__);
	/* Acquire configuration parameters */
	if (!SelectConfigFiles(userDoption, progname)) {
        proc_exit(1);
    }
PDEBUG("# 278:SelectConfigFiles "__FILE__);
	checkDataDir();
	ChangeToDataDir();

	/*
	 * Create lockfile for data directory.
	 */
	CreateDataDirLockFile(false);

	/* read control file (error checking and contains config ) */
	LocalProcessControlFile(false);

	/*
	 * process any libraries that should be preloaded at postmaster start
	 */
	process_shared_preload_libraries();

	/* Initialize MaxBackends */
	InitializeMaxBackends();
PDEBUG("# 127"); /* on_shmem_exit stubs call start here */
	/*
	 * Give preloaded libraries a chance to request additional shared memory.
	 */
	process_shmem_requests();

	/*
	 * Now that loadable modules have had their chance to request additional
	 * shared memory, determine the value of any runtime-computed GUCs that
	 * depend on the amount of shared memory required.
	 */
	InitializeShmemGUCs();

	/*
	 * Now that modules have been loaded, we can process any custom resource
	 * managers specified in the wal_consistency_checking GUC.
	 */
	InitializeWalConsistencyChecking();

	CreateSharedMemoryAndSemaphores();

	/*
	 * Remember stand-alone backend startup time,roughly at the same point
	 * during startup that postmaster does so.
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * Create a per-backend PGPROC struct in shared memory. We must do this
	 * before we can use LWLocks.
	 */
	InitProcess();

// main
	SetProcessingMode(InitProcessing);

	/* Early initialization */
	BaseInit();
async_db_change:;

PDEBUG("# 167");
	/*
	 * General initialization.
	 *
	 * NOTE: if you are tempted to add code in this vicinity, consider putting
	 * it inside InitPostgres() instead.  In particular, anything that
	 * involves database access should be there, not here.
	 */
	InitPostgres(dbname, InvalidOid,	/* database to connect to */
				 username, InvalidOid,	/* role to connect as */
                 (!am_walsender) ? INIT_PG_LOAD_SESSION_LIBS : 0,
				 NULL);			/* no out_dbname */

	/*
	 * If the PostmasterContext is still around, recycle the space; we don't
	 * need it anymore after InitPostgres completes.  Note this does not trash
	 * *MyProcPort, because ConnCreate() allocated that space with malloc()
	 * ... else we'd need to copy the Port data first.  Also, subsidiary data
	 * such as the username isn't lost either; see ProcessStartupPacket().
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	SetProcessingMode(NormalProcessing);

	/*
	 * Now all GUC states are fully set up.  Report them to client if
	 * appropriate.
	 */
	BeginReportingGUCOptions();

	/*
	 * Also set up handler to log session end; we have to wait till now to be
	 * sure Log_disconnections has its final value.
	 */
	if (IsUnderPostmaster && Log_disconnections)
		on_proc_exit(log_disconnections, 0);

	pgstat_report_connect(MyDatabaseId);

	/* Perform initialization specific to a WAL sender process. */
	if (am_walsender)
		InitWalSender();

	/*
	 * Send this backend's cancellation info to the frontend.
	 */
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'K');
		pq_sendint32(&buf, (int32) MyProcPid);
		pq_sendint32(&buf, (int32) MyCancelKey);
		pq_endmessage(&buf);
		/* Need not flush since ReadyForQuery will do it. */
	}

	/* Welcome banner for standalone case */
	if (whereToSendOutput == DestDebug)
		printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

	/*
	 * Create the memory context we will use in the main loop.
	 *
	 * MessageContext is reset once per iteration of the main loop, ie, upon
	 * completion of processing of each command message from the client.
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext, "MessageContext", ALLOCSET_DEFAULT_SIZES);

	/*
	 * Create memory context and buffer used for RowDescription messages. As
	 * SendRowDescriptionMessage(), via exec_describe_statement_message(), is
	 * frequently executed for ever single statement, we don't want to
	 * allocate a separate buffer every time.
	 */
	row_description_context = AllocSetContextCreate(TopMemoryContext, "RowDescriptionContext", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(row_description_context);
	initStringInfo(&row_description_buf);
	MemoryContextSwitchTo(TopMemoryContext);
} // AsyncPostgresSingleUserMain


