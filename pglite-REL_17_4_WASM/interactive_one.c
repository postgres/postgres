#include <unistd.h>  // access, unlink

#if !defined(__wasi__)
volatile sigjmp_buf local_sigjmp_buf;
#else
volatile sigjmp_buf void*;
#endif

/* TODO : prevent multiple write and write while reading ? */
volatile int cma_wsize = 0;
volatile int cma_rsize = 0;  // defined in postgres.c


__attribute__((export_name("interactive_read")))
int
interactive_read() {
    /* should cma_rsize should be reset here ? */
    return cma_wsize;
}


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
#ifdef PG16
	pq_init();					/* initialize libpq to talk to client */
    MyProcPort = (Port *) calloc(1, sizeof(Port));
#else
    MyProcPort = pq_init(&dummy_sock);
#endif
	whereToSendOutput = DestRemote; /* now safe to ereport to client */

    if (!MyProcPort) {
        PDEBUG("# 137: io_init   --------- NO CLIENT (oom) ---------");
        abort();
    }
#ifdef PG16
    MyProcPort->canAcceptConnections = CAC_OK;
#endif
    ClientAuthInProgress = out_auth;

    SOCKET_FILE = NULL;
    SOCKET_DATA = 0;
    PDEBUG("\n\n\n\n# 147: io_init  --------- Ready for CLIENT ---------");
}



volatile int sf_connected = 0;
volatile bool sockfiles = false;
volatile bool is_wire = true;
extern char * cma_port;


__attribute__((export_name("interactive_write"))) // EMSCRIPTEN_KEEPALIVE
void
interactive_write(int size) {
    cma_rsize = size;
    cma_wsize = 0;
}

__attribute__((export_name("ping")))
void
ping(void) {
    puts("pong");
}

__attribute__((export_name("use_wire")))
void
use_wire(int state) {
    if (state>0) {
        force_echo=true;
        printf("176: wire mode, repl off, echo %d\n", force_echo);
        is_wire = true;
        is_repl = false;
    } else {
        force_echo=true;
        printf("181: repl mode, no wire, echo %d\n", force_echo);
        is_wire = false;
        is_repl = true;
    }
}

__attribute__((export_name("clear_error")))
void
clear_error() {
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
#ifdef PG16
    ReplicationSlotCleanup();
#else
    ReplicationSlotCleanup(false);
#endif

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
}

void
startup_auth() {
    /* code is in handshake/auth domain so read whole msg now */

    if (ProcessStartupPacket(MyProcPort, true, true) != STATUS_OK) {
        PDEBUG("# 246: ProcessStartupPacket !OK");
    } else {
        PDEBUG("# 248: sending auth request");
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
}


void
startup_pass(bool check) {
    // auth 'p'
    if (check) {
        char *passwd = recv_password_packet(MyProcPort);
        printf("# 223: auth recv password: %s\n", "md5***" );
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
    } else {
        PDEBUG("# 285: auth skip");
    }
    ClientAuthInProgress = false;

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
PDEBUG("# 305: TODO: set a pg_main started flag");
    sf_connected++;
    send_ready_for_query = true;

}

extern void pg_startcma();

EMSCRIPTEN_KEEPALIVE void
interactive_one() {
    int	peek = -1;  /* preview of firstchar with no pos change */
	int firstchar = 0;  /* character read from getc() */
    bool pipelining = true;
	StringInfoData input_message;
	StringInfoData *inBuf;
    FILE *stream ;
    FILE *fp;
    int packetlen;

    bool had_notification = notifyInterruptPending;
    bool notified = false;
if (notifyInterruptPending)
    PDEBUG("# 327: has notification !");

    if (!MyProcPort) {
        io_init(is_wire, false);
    }

    // this could be pg_flush in sync mode.
    // but in fact we are writing socket data that was piled up previous frame async.
    if (SOCKET_DATA>0) {
        puts("331: ERROR flush after frame");
        goto wire_flush;
    }

    if (!cma_rsize) {
        // prepare reply queue
        if (!SOCKET_FILE) {
            SOCKET_FILE =  fopen(PGS_OLOCK, "w") ;
            MyProcPort->sock = fileno(SOCKET_FILE);
        }
    }

    doing_extended_query_message = false;
    MemoryContextSwitchTo(MessageContext);
    MemoryContextResetAndDeleteChildren(MessageContext);

    initStringInfo(&input_message);

    inBuf = &input_message;

	InvalidateCatalogSnapshotConditionally();

	if (send_ready_for_query) {

		if (IsAbortedTransactionBlockState()) {
			puts("@@@@ TODO 356: idle in transaction (aborted)");
		}
		else if (IsTransactionOrTransactionBlock()) {
			puts("@@@@ TODO 359: idle in transaction");
		} else {
			if (notifyInterruptPending) {
				ProcessNotifyInterrupt(false);
PDEBUG("# 367: notified ?");
                notified = true;
            }
        }
        send_ready_for_query = false;
    }


// postgres.c 4627
    DoingCommandRead = true;

#if defined(EMUL_CMA)
    #define IO ((char *)(1+(int)cma_port))  //  temp fix for -O0 but less efficient than literal
    #error "inefficient"
#else
    #define IO ((char *)(1))
#endif


/*
 * in cma mode (cma_rsize>0), client call the wire loop itself waiting synchronously for the results
 * in socketfiles mode, the wire loop polls a pseudo socket made from incoming and outgoing files.
 * in repl mode (cma_rsize==0) output is on stdout not cma/socketfiles wire. repl mode is default.
 */

    peek = IO[0];
    packetlen = cma_rsize;

    if (cma_rsize) {
        sockfiles = false;
        if (!is_repl) {
            whereToSendOutput = DestRemote;
            if (!is_wire)
                PDEBUG("repl message in cma buffer !");
        } else {
            if (is_wire)
                PDEBUG("wire message in cma buffer for REPL !");
            whereToSendOutput = DestDebug;
        }
    } else {
        fp = fopen(PGS_IN, "r");

        // read as a socket.
        if (fp) {
            fseek(fp, 0L, SEEK_END);
            packetlen = ftell(fp);
            if (packetlen) {
                // always.
                is_wire = true;
                sockfiles = true;
                whereToSendOutput = DestRemote;
                resetStringInfo(inBuf);
                rewind(fp);
                /* peek on first char */
                peek = getc(fp);
                rewind(fp);
                pq_recvbuf_fill(fp, packetlen);
#if PGDEBUG
                rewind(fp);
#endif
                /* is it startup/auth packet ? */
                if (!peek) {
                    startup_auth();
                    peek = -1;
                }
                if (peek==112) {
                    startup_pass(true);
                    peek = -1;
                }
            }

            /* FD CLEANUP, all cases */
            fclose(fp);
            unlink(PGS_IN);

            if (packetlen) {
                // it was startup/auth , write and return fast.
                if (peek<0) {
                    PDEBUG("# 438: handshake/auth/pass skip");
                    goto wire_flush;
                }

                /* else it was wire msg */
#if PGDEBUG
                printf("# 444: node+repl is_wire -> true : %c\n", peek);
                force_echo = true;
#endif
                firstchar = peek;
                goto incoming;
            } // wire msg

        } // fp data read

        // is it REPL in cma ?
        if (!peek)
            return;

        firstchar = peek ;

        //REPL mode  in zero copy buffer ( lowest wasm memory segment )
        packetlen = strlen(IO);

    } // !cma_rsize -> socketfiles -> repl

#if PGDEBUG
    if (packetlen)
        IO[packetlen]=0;
    printf("# 479: fd=%d is_embed=%d is_repl=%d is_wire=%d fd %s,len=%d   peek=%d [%s]\n", MyProcPort->sock, is_embed, is_repl, is_wire, PGS_OLOCK, packetlen, peek, IO);
#endif

    // buffer query TODO: direct access ?
    // CMA wire mode. -> packetlen was set to cma_rsize
    resetStringInfo(inBuf);

    for (int i=0; i<packetlen; i++) {
        appendStringInfoChar(inBuf, IO[i]);
    }

    if (packetlen<2) {
        puts("# 491: WARNING: empty packet");
        cma_rsize= 0;
        if (is_repl)
            pg_prompt();
        // always free cma buffer !!!
        IO[0] = 0;
        return;
    }

incoming:
#if defined(__EMSCRIPTEN__) || defined(__wasi__) //PGDEBUG
#   include "pgl_sjlj.c"
#else
    #error "sigsetjmp unsupported"
#endif // wasi

    while (pipelining) {
        if (is_repl) {
            // TODO: are we sure repl could not pipeline ?
            pipelining = false;
            /* stdio node repl */
            printf("# 512: enforcing REPL mode, wire off, echo %d\n", force_echo);
            whereToSendOutput = DestDebug;
        }

        if (is_wire) {
            /* wire on a socket or cma may auth, not handled by pg_proto block */
            if (peek==0) {
                PDEBUG("# 519: handshake/auth");
                startup_auth();
                PDEBUG("# 521: auth request");
                break;
            }

            if (peek==112) {
                PDEBUG("# 525: password");
                startup_pass(true);
                break;
            }

            firstchar = SocketBackend(inBuf);

            pipelining = pq_buffer_remaining_data()>0;
            if (!pipelining) {
                printf("# 535: end of wire, rfq=%d\n", send_ready_for_query);
            } else {
                printf("# 537: no end of wire -> pipelining, rfq=%d\n", send_ready_for_query);
            }
        } else {
            /* nowire */
            pipelining = false;
            if (firstchar == EOF && inBuf->len == 0) {
                firstchar = EOF;
            } else {
                appendStringInfoChar(inBuf, (char) '\0');
            	firstchar = 'Q';
            }
        }

        #if PGDEBUG
        if (!pipelining) {
            printf("# 552: wire=%d 1stchar=%c Q: %s\n", is_wire,  firstchar, inBuf->data);
            force_echo = false;
        } else {
            printf("# 555: PIPELINING [%c]!\n", firstchar);
        }
        #endif

        if (!ignore_till_sync)
            send_ready_for_query = true;

        if (ignore_till_sync && firstchar != EOF) {
            puts("@@@@@@@@@@@@@ 562 TODO: postgres.c 	4684 :	continue");
        } else { /* process notifications (ASYNC) */
            if (notifyInterruptPending) {               PDEBUG("# 565: @@@ has notification @@@@\n");
               ProcessClientReadInterrupt(true);
            }
        }

        #include "pg_proto.c"
    }

    if (!is_repl) {
wire_flush:
        if (!ClientAuthInProgress) {
            /* process notifications (SYNC) */
            if (notifyInterruptPending)
               ProcessNotifyInterrupt(false);

            if (send_ready_for_query) {
                PDEBUG("# 581: end packet - sending rfq");
                ReadyForQuery(DestRemote);
                //done at postgres.c 4623 send_ready_for_query = false;
            } else {
                PDEBUG("# 585: end packet - with no rfq");
            }
        } else {
            PDEBUG("# 588: end packet (ClientAuthInProgress - no rfq) ");
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
                    PDEBUG("# 605: cma and sockfile ???");
                if (sockfiles) {
#if PGDEBUG
                    printf("# 608: client:ready -> read(%d) " PGS_OLOCK "->" PGS_OUT"\n", outb);
#endif
                    rename(PGS_OLOCK, PGS_OUT);
                }
            } else {
#if PGDEBUG
                printf("# 614: out queue : %d flushed\n", cma_wsize);
#endif
                SOCKET_DATA = 0;
            }

        } else {
            cma_wsize = 0;
        }
    } else {
        pg_prompt();
    }



    // always free kernel buffer !!!
    cma_rsize = 0;
    IO[0] = 0;

    #undef IO
}


