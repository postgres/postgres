#define PDEBUG(string) puts(string)
#include <unistd.h>  // access, unlink
volatile sigjmp_buf local_sigjmp_buf;


static void pg_prompt() {
    fprintf(stdout,"pg> %c\n", 4);
}

extern void AbortTransaction(void);
extern void CleanupTransaction(void);
extern void ClientAuthentication(Port *port);
extern FILE* SOCKET_FILE;
extern int SOCKET_DATA;

/*
init sequence
___________________________________
SubPostmasterMain / (forkexec)
    InitPostmasterChild
    shm attach
    preload

    BackendInitialize(Port *port) -> collect initial packet

	    pq_init();
	    whereToSendOutput = DestRemote;
	    status = ProcessStartupPacket(port, false, false);
            pq_startmsgread
            pq_getbytes from pq_recvbuf
            TODO: place PqRecvBuffer (8K) in lower mem for zero copy

        PerformAuthentication
        ClientAuthentication(port)
        CheckPasswordAuth SYNC!!!!  ( sendAuthRequest flush -> recv_password_packet )
    InitShmemAccess/InitProcess/CreateSharedMemoryAndSemaphores

    BackendRun(port)
        PostgresMain


-> pq_flush() is synchronous


buffer sizes:

    https://github.com/postgres/postgres/blob/master/src/backend/libpq/pqcomm.c#L118

    https://github.com/postgres/postgres/blob/master/src/common/stringinfo.c#L28



*/
extern int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
extern void pq_recvbuf_fill(FILE* fp, int packetlen);

#define PG_MAX_AUTH_TOKEN_LENGTH	65535
static char *
recv_password_packet(Port *port) {
	StringInfoData buf;
	int			mtype;

	pq_startmsgread();

	/* Expect 'p' message type */
	mtype = pq_getbyte();
	if (mtype != 'p')
	{
		/*
		 * If the client just disconnects without offering a password, don't
		 * make a log entry.  This is legal per protocol spec and in fact
		 * commonly done by psql, so complaining just clutters the log.
		 */
		if (mtype != EOF)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("expected password response, got message type %d",
							mtype)));
		return NULL;			/* EOF or bad message type */
	}

	initStringInfo(&buf);
	if (pq_getmessage(&buf, PG_MAX_AUTH_TOKEN_LENGTH))	/* receive password */
	{
		/* EOF - pq_getmessage already logged a suitable message */
		pfree(buf.data);
		return NULL;
	}

	/*
	 * Apply sanity check: password packet length should agree with length of
	 * contained string.  Note it is safe to use strlen here because
	 * StringInfo is guaranteed to have an appended '\0'.
	 */
	if (strlen(buf.data) + 1 != buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid password packet size")));

	/*
	 * Don't allow an empty password. Libpq treats an empty password the same
	 * as no password at all, and won't even try to authenticate. But other
	 * clients might, so allowing it would be confusing.
	 *
	 * Note that this only catches an empty password sent by the client in
	 * plaintext. There's also a check in CREATE/ALTER USER that prevents an
	 * empty string from being stored as a user's password in the first place.
	 * We rely on that for MD5 and SCRAM authentication, but we still need
	 * this check here, to prevent an empty password from being used with
	 * authentication methods that check the password against an external
	 * system, like PAM, LDAP and RADIUS.
	 */
	if (buf.len == 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PASSWORD),
				 errmsg("empty password returned by client")));

	/* Do not echo password to logs, for security. */
	elog(DEBUG5, "received password packet");
	return buf.data;
}


int md5Salt_len  = 4;
char md5Salt[4];

ClientSocket dummy_sock;

static void io_init(bool in_auth, bool out_auth) {
        ClientAuthInProgress = in_auth;
        MyProcPort = pq_init(&dummy_sock);
        if (!MyProcPort) {
            PDEBUG("# 133: io_init   --------- NO CLIENT (oom) ---------");
            abort();
        }
    	whereToSendOutput = DestRemote; /* now safe to ereport to client */
#ifdef PG16
       MyProcPort->canAcceptConnections = CAC_OK;
#endif
        ClientAuthInProgress = out_auth;

        SOCKET_FILE = NULL;
        SOCKET_DATA = 0;
        PDEBUG("\n\n\n\n# 141: io_init  --------- Ready for CLIENT ---------");


}



EMSCRIPTEN_KEEPALIVE void
interactive_write(int size) {
    cma_rsize = size;
    cma_wsize = 0;
}


volatile int sf_connected = 0;
volatile bool sockfiles = false;


EMSCRIPTEN_KEEPALIVE void
interactive_one() {
	int			firstchar;
	int			c;				/* character read from getc() */
	StringInfoData input_message;
	StringInfoData *inBuf;
    FILE *stream ;
    FILE *fp;
    int packetlen;
    bool is_socket = false;
    bool is_wire = true;

    if (!is_node && is_repl) {


        if (!MyProcPort) {
            io_init(false, false);
        }

        // this could be pg_flush in sync mode.
        // but really we are writing socket data that was piled up previous frame async.
        if (SOCKET_DATA>0)
            goto wire_flush;


        if (!SOCKET_FILE) {
            SOCKET_FILE =  fopen(PGS_OLOCK, "w") ;
            MyProcPort->sock = fileno(SOCKET_FILE);
        }

    } // is_node && is_repl


    doing_extended_query_message = false;
    MemoryContextSwitchTo(MessageContext);
    MemoryContextResetAndDeleteChildren(MessageContext);

    initStringInfo(&input_message);

    inBuf = &input_message;

		InvalidateCatalogSnapshotConditionally();

		if (send_ready_for_query)
		{

            // puts("postgres.c 4538-4624 TODO");
			if (IsAbortedTransactionBlockState())
			{
				puts("@@@@ TODO 231: idle in transaction (aborted)");
			}
			else if (IsTransactionOrTransactionBlock())
			{
				puts("@@@@ TODO 235: idle in transaction");
			}
			else
			{
				if (notifyInterruptPending)
					ProcessNotifyInterrupt(false);
            }
            send_ready_for_query = false;
        }


// postgres.c 4627
    DoingCommandRead = true;


    #define IO ((char *)(1))
//    #define IO cma_port    this would be a temp fix for -O0 but less efficient than a

/*
 * in web mode, client call the wire loop itself waiting synchronously for the results
 * in repl mode, the wire loop polls a pseudo socket made from incoming and outgoing files. aka "socketfiles"
 * always use "socketfiles" when wasi
 *
 */


#if 0 //!defined(__wasi__)
    if (!is_embed || is_repl) {
        // do not try to read when lock/buffer file still there
        if (!access(PGS_ILOCK, R_OK)) {
#endif

            packetlen = 0;

            fp = fopen(PGS_IN, "r");

            // read as a socket.
            if (fp) {
                fseek(fp, 0L, SEEK_END);
                packetlen = ftell(fp);

//printf("# 250 : wire packetlen = %d\n", packetlen);
                if (packetlen) {
                    sockfiles = true;
                    whereToSendOutput = DestRemote;
                    resetStringInfo(inBuf);
                    rewind(fp);
                    /* peek on first char */
                    firstchar = getc(fp);
                    rewind(fp);
#define SOCKFILE 1
                    pq_recvbuf_fill(fp, packetlen);
#if PGDEBUG
                    rewind(fp);
#endif

                    /* is it startup/auth packet ? */
                    if (!firstchar || (firstchar==112)) {
                        /* code is in handshake/auth domain so read whole msg now */
                        //pq_recvbuf_fill(fp, packetlen);

                        if (!firstchar) {
                            if (ProcessStartupPacket(MyProcPort, true, true) != STATUS_OK) {
                                PDEBUG("# 266: ProcessStartupPacket !OK");
                            } else {
                                PDEBUG("# 267: auth request");
                                //ClientAuthentication(MyProcPort);
    ClientAuthInProgress = true;
                                md5Salt[0]=0x01;
                                md5Salt[1]=0x23;
                                md5Salt[2]=0x45;
                                md5Salt[3]=0x56;
                                {
                                    StringInfoData buf;
	                                pq_beginmessage(&buf, 'R');
	                                pq_sendint32(&buf, (int32) AUTH_REQ_MD5);
	                                if (md5Salt_len > 0)
		                                pq_sendbytes(&buf, md5Salt, md5Salt_len);
	                                pq_endmessage(&buf);
                                    pq_flush();
                                }
                            }
                        } // handshake

                        if (firstchar==112) {
                            char *passwd = recv_password_packet(MyProcPort);
                            printf("auth recv password: %s\n", "md5***" );
    ClientAuthInProgress = false;
    /*
                        // TODO: CheckMD5Auth
                            if (passwd == NULL)
                                return STATUS_EOF;
                            if (shadow_pass)
                                result = md5_crypt_verify(port->user_name, shadow_pass, passwd, md5Salt, md5Salt_len, logdetail);
                            else
                                result = STATUS_ERROR;
    */
                            pfree(passwd);
                            {
                                StringInfoData buf;
                                pq_beginmessage(&buf, 'R');
                                pq_sendint32(&buf, (int32) AUTH_REQ_OK);
                                pq_endmessage(&buf);
                            }

                            BeginReportingGUCOptions();
                            pgstat_report_connect(MyDatabaseId);
                            {
	                            StringInfoData buf;
	                            pq_beginmessage(&buf, 'K');
	                            pq_sendint32(&buf, (int32) MyProcPid);
	                            pq_sendint32(&buf, (int32) MyCancelKey);
	                            pq_endmessage(&buf);
                            }

PDEBUG("# 324 : TODO: set a pg_main started flag");
                            sf_connected++;
// CHECK ME see 538 / 563
                            send_ready_for_query = true;
                        } // auth
                    } else {
#if PGDEBUG
                        fprintf(stderr, "# 331: CLI[%d] incoming=%d [%d, ", sf_connected, packetlen, firstchar);
                        for (int i=0;i<packetlen;i++) {
                            int b = getc(fp);
                            /* skip header (size uint32) */
                            if (i>4) {
                                fprintf(stderr, "%d, ", b);
                            }
                        }
                        fprintf(stderr, "]\n");
#endif
                    }
                    // when using locks
                    // ftruncate(filenum(fp), 0);
                }
/* FD CLEANUP */
                fclose(fp);
                unlink(PGS_IN);

                // Check if auth bypass work with socketfiles
                if (packetlen) {
                    if (!firstchar || (firstchar==112)) {
                        PDEBUG("# 351: handshake/auth skip");
                        goto wire_flush;
                    }

                    /* else it is wire msg */
#if PGDEBUG
printf("# 353 : node+repl is_wire/is_socket -> true : %c\n", firstchar);
                    force_echo = true;
#endif
                    is_socket = true;
                    is_wire = true;
                    whereToSendOutput = DestRemote;

                    goto incoming;
                } // wire msg

            } // fp data read
#if 0 //!defined(__wasi__)
        } // ok lck
    } // !is_embed || is_repl
#endif
    if (cma_rsize) {
        PDEBUG("wire message in cma buffer !");
        is_wire = true;
        is_socket = false;
        sockfiles = false;
        whereToSendOutput = DestRemote;

        if (!MyProcPort) {
            io_init(true, false);
        }

        if (!SOCKET_FILE) {
            SOCKET_FILE =  fopen(PGS_OLOCK, "w") ;
            MyProcPort->sock = fileno(SOCKET_FILE);
        }
#if PGDEBUG
        printf("# 391: fd %s: %s fd=%d is_embed=%d\n", PGS_OLOCK, IO, MyProcPort->sock, 1);
#endif
        goto incoming;

    }

    c = IO[0];


// TODO: use a msg queue length
    if (!c)
        return;

    is_repl = true;

    if (is_repl) {
        whereToSendOutput = DestNone;
        is_wire = false;
        is_socket = false;
    } else {
        is_wire = false;
        is_socket = false;
        whereToSendOutput = DestRemote;

        if (!MyProcPort) {
            ClientAuthInProgress = true;

            MyProcPort = pq_init(&dummy_sock);
            if (!MyProcPort) {
                PDEBUG("      --------- NO CLIENT (oom) ---------");
                abort();
            }
#ifdef PG16
            MyProcPort->canAcceptConnections = CAC_OK;
#endif
            ClientAuthInProgress = false;
        }

        if (!SOCKET_FILE) {
            SOCKET_FILE =  fopen(PGS_OLOCK, "w") ;
            MyProcPort->sock = fileno(SOCKET_FILE);
        }
#if PGDEBUG
        printf("# 430: fd %s: %s fd=%d is_embed=%d\n", PGS_OLOCK, IO, MyProcPort->sock, 1);
#endif

    }

    // zero copy buffer ( lower wasm memory segment )
    packetlen = strlen(IO);
    if (packetlen<2) {
        pg_prompt();
        // always free kernel buffer !!!
        IO[0] = 0;
        return;
    }


// buffer query TODO: direct access ?
	resetStringInfo(inBuf);

    for (int i=0; i<packetlen; i++) {
        appendStringInfoChar(inBuf, IO[i]);
    }

    // always free kernel buffer !!!
    IO[0] = 0;

incoming:
#if defined(__wasi__) //PGDEBUG
    PDEBUG("# 484: sjlj exception handler off");
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

        /*
         * If we were handling an extended-query-protocol message, skip till next Sync.
         * This also causes us not to issue ReadyForQuery (until we get Sync).
         */

        if (!ignore_till_sync)
            send_ready_for_query = true;

        if (!is_wire)
            pg_prompt();

        goto wire_flush;
    }

	PG_exception_stack = &local_sigjmp_buf;
#endif // wasi

    if (!ignore_till_sync)
        send_ready_for_query = true;


    if (force_echo) {
        printf("# 549: wire=%d socket=%d 1stchar=%c Q: %s", is_wire, is_socket, firstchar, inBuf->data);
    }


        if (is_wire) {
            /* wire on a socket or cma */
            firstchar = SocketBackend(inBuf);

        } else {
            /* nowire */
            if (c == EOF && inBuf->len == 0) {
                firstchar = EOF;

            } else {
                appendStringInfoChar(inBuf, (char) '\0');
            	firstchar = 'Q';
            }

            /* stdio node repl */
            if (is_repl)
                whereToSendOutput = DestDebug;
        }
    while (1) {
	    if (ignore_till_sync && firstchar != EOF) {
	        puts("@@@@@@@@@@@@@ 573 TODO: postgres.c 	4684 :	continue");
        } else {
            #include "pg_proto.c"

            /* process notifications */
            ProcessClientReadInterrupt(true);
        }
        if (is_wire && pq_buffer_remaining_data()) {
            firstchar = SocketBackend(inBuf);
#if PGDEBUG
            printf("583: PIPELINING [%c]!\n", firstchar);
#endif
        } else {
            break;
        }
    }

    if (is_wire) {
wire_flush:
        if (!ClientAuthInProgress) {
            if (send_ready_for_query) {
                PDEBUG("# 594: end packet - sending rfq");
                ReadyForQuery(DestRemote);
                //done at postgres.c 4623 send_ready_for_query = false;
            } else {
                PDEBUG("# 598: end packet - with no rfq");
            }
        } else {
            PDEBUG("# 601: end packet (ClientAuthInProgress - no rfq) ");
        }

        if (SOCKET_DATA>0) {
            if (sockfiles) {
                if (cma_wsize)
                    puts("ERROR: cma was not flushed before socketfile interface");
            } else {
                /* wsize may have increased with previous rfq so assign here */
                cma_wsize = SOCKET_DATA;
            }
            if (SOCKET_FILE) {
                int outb = SOCKET_DATA;
                fclose(SOCKET_FILE);
                SOCKET_FILE = NULL;
                SOCKET_DATA = 0;
                if (cma_wsize)
                    PDEBUG("# 618: cma and sockfile ???");
                if (sockfiles) {
#if PGDEBUG
                    printf("# 621: client:ready -> read(%d) " PGS_OLOCK "->" PGS_OUT"\n", outb);
#endif
                    rename(PGS_OLOCK, PGS_OUT);
                }
            }

        } else {
            cma_wsize = 0;
        }
    }

    // always free kernel buffer !!!
    cma_rsize = 0;
    IO[0] = 0;

    #undef IO
}



