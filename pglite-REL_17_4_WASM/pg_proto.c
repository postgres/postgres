/*
 * this file is used by both interactive_file ( initdb boot/single )
 * and interactive_one()
 *
 */
    switch (firstchar)
    {
	    case 'Q':			/* simple query */
		    {
			    const char *query_string;

			    /* Set statement_timestamp() */
			    SetCurrentStatementStartTimestamp();

			    query_string = pq_getmsgstring(&input_message);
			    pq_getmsgend(&input_message);

			    if (am_walsender)
			    {
				    if (!exec_replication_command(query_string))
					    exec_simple_query(query_string);
			    }
			    else
				    exec_simple_query(query_string);

			    send_ready_for_query = true;
		    }
		    break;

	    case 'P':			/* parse */
		    {
			    const char *stmt_name;
			    const char *query_string;
			    int			numParams;
			    Oid		   *paramTypes = NULL;

			    forbidden_in_wal_sender(firstchar);

			    /* Set statement_timestamp() */
			    SetCurrentStatementStartTimestamp();

			    stmt_name = pq_getmsgstring(&input_message);
			    query_string = pq_getmsgstring(&input_message);
			    numParams = pq_getmsgint(&input_message, 2);
			    if (numParams > 0)
			    {
				    paramTypes = palloc_array(Oid, numParams);
				    for (int i = 0; i < numParams; i++)
					    paramTypes[i] = pq_getmsgint(&input_message, 4);
			    }
			    pq_getmsgend(&input_message);

			    exec_parse_message(query_string, stmt_name,
							       paramTypes, numParams);

			    //valgrind_report_error_query(query_string);
		    }
		    break;

	    case 'B':			/* bind */
		    forbidden_in_wal_sender(firstchar);

		    /* Set statement_timestamp() */
		    SetCurrentStatementStartTimestamp();

		    /*
		     * this message is complex enough that it seems best to put
		     * the field extraction out-of-line
		     */
		    exec_bind_message(&input_message);

		    /* exec_bind_message does valgrind_report_error_query */
		    break;

	    case 'E':			/* execute */
		    {
			    const char *portal_name;
			    int			max_rows;

			    forbidden_in_wal_sender(firstchar);

			    /* Set statement_timestamp() */
			    SetCurrentStatementStartTimestamp();

			    portal_name = pq_getmsgstring(&input_message);
			    max_rows = pq_getmsgint(&input_message, 4);
			    pq_getmsgend(&input_message);

			    exec_execute_message(portal_name, max_rows);

			    /* exec_execute_message does valgrind_report_error_query */
		    }
		    break;

	    case 'F':			/* fastpath function call */
		    forbidden_in_wal_sender(firstchar);

		    /* Set statement_timestamp() */
		    SetCurrentStatementStartTimestamp();

		    /* Report query to various monitoring facilities. */
		    pgstat_report_activity(STATE_FASTPATH, NULL);
		    set_ps_display("<FASTPATH>");

		    /* start an xact for this function invocation */
		    start_xact_command();

		    /*
		     * Note: we may at this point be inside an aborted
		     * transaction.  We can't throw error for that until we've
		     * finished reading the function-call message, so
		     * HandleFunctionRequest() must check for it after doing so.
		     * Be careful not to do anything that assumes we're inside a
		     * valid transaction here.
		     */

		    /* switch back to message context */
		    MemoryContextSwitchTo(MessageContext);

		    HandleFunctionRequest(&input_message);

		    /* commit the function-invocation transaction */
		    finish_xact_command();

	        // valgrind_report_error_query("fastpath function call");

		    send_ready_for_query = true;
		    break;

	    case 'C':			/* close */
		    {
			    int			close_type;
			    const char *close_target;

			    forbidden_in_wal_sender(firstchar);

			    close_type = pq_getmsgbyte(&input_message);
			    close_target = pq_getmsgstring(&input_message);
			    pq_getmsgend(&input_message);

			    switch (close_type)
			    {
				    case 'S':
					    if (close_target[0] != '\0')
						    DropPreparedStatement(close_target, false);
					    else
					    {
						    /* special-case the unnamed statement */
						    drop_unnamed_stmt();
					    }
					    break;
				    case 'P':
					    {
						    Portal		portal;

						    portal = GetPortalByName(close_target);
						    if (PortalIsValid(portal))
							    PortalDrop(portal, false);
					    }
					    break;
				    default:
					    ereport(ERROR,
							    (errcode(ERRCODE_PROTOCOL_VIOLATION),
							     errmsg("invalid CLOSE message subtype %d",
									    close_type)));
					    break;
			    }

			    if (whereToSendOutput == DestRemote)
				    pq_putemptymessage('3');	/* CloseComplete */

			    //valgrind_report_error_query("CLOSE message");
		    }
		    break;

	    case 'D':			/* describe */
		    {
			    int			describe_type;
			    const char *describe_target;

			    forbidden_in_wal_sender(firstchar);

			    /* Set statement_timestamp() (needed for xact) */
			    SetCurrentStatementStartTimestamp();

			    describe_type = pq_getmsgbyte(&input_message);
			    describe_target = pq_getmsgstring(&input_message);
			    pq_getmsgend(&input_message);

			    switch (describe_type)
			    {
				    case 'S':
					    exec_describe_statement_message(describe_target);
					    break;
				    case 'P':
					    exec_describe_portal_message(describe_target);
					    break;
				    default:
					    ereport(ERROR,
							    (errcode(ERRCODE_PROTOCOL_VIOLATION),
							     errmsg("invalid DESCRIBE message subtype %d",
									    describe_type)));
					    break;
			    }

			    // valgrind_report_error_query("DESCRIBE message");
		    }
		    break;

	    case 'H':			/* flush */
		    pq_getmsgend(&input_message);
		    if (whereToSendOutput == DestRemote)
			    pq_flush();
		    break;

	    case 'S':			/* sync */
		    pq_getmsgend(&input_message);
		    finish_xact_command();
		    //valgrind_report_error_query("SYNC message");
		    send_ready_for_query = true;
		    break;

		    /*
		     * 'X' means that the frontend is closing down the socket. EOF
		     * means unexpected loss of frontend connection. Either way,
		     * perform normal shutdown.
		     */
	    case EOF:

		    /* for the cumulative statistics system */
		    pgStatSessionEndCause = DISCONNECT_CLIENT_EOF;

		    /* FALLTHROUGH */

	    case 'X':

		    /*
		     * Reset whereToSendOutput to prevent ereport from attempting
		     * to send any more messages to client.
		     */
		    if (whereToSendOutput == DestRemote)
			    whereToSendOutput = DestNone;

		    /*
		     * NOTE: if you are tempted to add more code here, DON'T!
		     * Whatever you had in mind to do should be set up as an
		     * on_proc_exit or on_shmem_exit callback, instead. Otherwise
		     * it will fail to be called during other backend-shutdown
		     * scenarios.
		     */
// puts("# 697:proc_exit/repl/skip"); //proc_exit(0);
            is_repl = false;
            return;

	    case 'd':			/* copy data */
	    case 'c':			/* copy done */
	    case 'f':			/* copy fail */

		    /*
		     * Accept but ignore these messages, per protocol spec; we
		     * probably got here because a COPY failed, and the frontend
		     * is still sending data.
		     */
		    break;

	    default:
		    ereport(FATAL,
				    (errcode(ERRCODE_PROTOCOL_VIOLATION),
				     errmsg("invalid frontend message type %d",
						    firstchar)));
    } // end switch

