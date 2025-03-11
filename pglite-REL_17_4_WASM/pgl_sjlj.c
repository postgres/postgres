#if defined(__wasi__)
    PDEBUG("# 2:" __FILE__ ": sjlj exception handler off");
#else
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
        error_context_stack = NULL;
        HOLD_INTERRUPTS();

        disable_all_timeouts(false);	/* do first to avoid race condition */
        QueryCancelPending = false;
        idle_in_transaction_timeout_enabled = false;
        idle_session_timeout_enabled = false;
        DoingCommandRead = false;

        pq_comm_reset();
        EmitErrorReport();
        debug_query_string = NULL;

        AbortCurrentTransaction();

        if (am_walsender)
            WalSndErrorCleanup();

        PortalErrorCleanup();
        if (MyReplicationSlot != NULL)
            ReplicationSlotRelease();

        ReplicationSlotCleanup(false);

        MemoryContextSwitchTo(TopMemoryContext);
        FlushErrorState();

        if (doing_extended_query_message)
            ignore_till_sync = true;

        xact_started = false;

        if (pq_is_reading_msg())
            ereport(FATAL,
	                (errcode(ERRCODE_PROTOCOL_VIOLATION),
	                 errmsg("terminating connection because protocol synchronization was lost")));

        RESUME_INTERRUPTS();
#if !defined(INITDB_SINGLE)
        /*
         * If we were handling an extended-query-protocol message, skip till next Sync.
         * This also causes us not to issue ReadyForQuery (until we get Sync).
         */

        if (!ignore_till_sync)
            send_ready_for_query = true;

        if (!is_wire)
            pg_prompt();

        goto wire_flush;
#endif
    }

	PG_exception_stack = &local_sigjmp_buf;
#endif

