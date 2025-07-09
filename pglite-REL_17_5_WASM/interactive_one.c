#include <unistd.h>  // access, unlink
#define PGL_LOOP
#if defined(__wasi__)
// volatile sigjmp_buf void*;
#else
volatile sigjmp_buf local_sigjmp_buf;
#endif

// track back how many ex raised in steps of the loop until sucessfull clear_error
volatile int canary_ex = 0;

// track back mode used for last reply   <0 socketfiles , 0== repl , > 0 cma addr
volatile int channel = 0;

/* TODO : prevent multiple write and write while reading ? */
volatile int cma_wsize = 0;
volatile int cma_rsize = 0;  // also defined in postgres.c for pqcomm
volatile bool sockfiles = false; // also defined in postgres.c for pqcomm

__attribute__((export_name("get_buffer_size")))
int
get_buffer_size(int fd) {
    return (CMA_MB * 1024 * 1024) / CMA_FD;
}

// TODO add query size
__attribute__((export_name("get_buffer_addr")))
int
get_buffer_addr(int fd) {
    return 1 + ( get_buffer_size(fd) *fd);
}

__attribute__((export_name("get_channel")))
int
get_channel() {
    return channel;
}


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
        PDEBUG("# 155: io_init   --------- NO CLIENT (oom) ---------");
        abort();
    }
#ifdef PG16
    MyProcPort->canAcceptConnections = CAC_OK;
#endif
    ClientAuthInProgress = out_auth;
    SOCKET_FILE = NULL;
    SOCKET_DATA = 0;
    PDEBUG("\n\n\n# 165: io_init  --------- Ready for CLIENT ---------");
}


volatile bool is_wire = true;
extern char * cma_port;
extern void pq_startmsgread(void);

__attribute__((export_name("interactive_write")))
void
interactive_write(int size) {
    cma_rsize = size;
    cma_wsize = 0;
}

__attribute__((export_name("use_wire")))
void
use_wire(int state) {
#if PGDEBUG
    force_echo=true;
#endif
    if (state>0) {
#if PGDEBUG
        printf("\n\n# 194: PACKET START: wire mode, repl off, echo %d\n", force_echo);
#endif
        is_wire = true;
        is_repl = false;
    } else {
#if PGDEBUG
        printf("\n\n# 200: PACKET START: repl mode, no wire, echo %d\n", force_echo);
#endif
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

void discard_input(){
    if (!cma_rsize)
        return;
    pq_startmsgread();
    for (int i = 0; i < cma_rsize; i++) {
        pq_getbyte();
    }
    pq_endmsgread();
}

void
startup_auth() {
    /* code is in handshake/auth domain so read whole msg now */
    send_ready_for_query = false;

    if (ProcessStartupPacket(MyProcPort, true, true) != STATUS_OK) {
        PDEBUG("# 271: ProcessStartupPacket !OK");
    } else {

        sf_connected++;
        PDEBUG("# 273: sending auth request");
        //ClientAuthentication(MyProcPort);
        discard_input();

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
        PDEBUG("# 223: auth recv password: md5***");
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
        PDEBUG("# 310: auth skip");
        discard_input();
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
PDEBUG("# 330: TODO: set a pgl started flag");
    send_ready_for_query = true;
    ignore_till_sync = false;
    volatile int sf_connected = 0;
}

extern void pg_startcma();

__attribute__((export_name("interactive_one"))) void
interactive_one() {
    int	peek = -1;  /* preview of firstchar with no pos change */
	int firstchar = 0;  /* character read from getc() */
    bool pipelining = true;
	StringInfoData input_message;
	StringInfoData *inBuf;
    FILE *stream ;
    FILE *fp = NULL;
    int packetlen;

    bool had_notification = notifyInterruptPending;
    bool notified = false;
    // send_ready_for_query = false;

    if (!MyProcPort) {
        PDEBUG("# 353: client created");
        io_init(is_wire, false);
    }

#if PGDEBUG
    if (notifyInterruptPending)
        PDEBUG("# 371: has notification !");
#endif

    // this could be pg_flush in sync mode.
    // but in fact we are writing socket data that was piled up previous frame async.
    if (SOCKET_DATA>0) {
        puts("# 361: ERROR flush after frame");
        goto wire_flush;
    }

    if (!cma_rsize) {
        // no cma : reading from file. writing to file.
        if (!SOCKET_FILE) {
            SOCKET_FILE =  fopen(PGS_OLOCK, "w") ;
            MyProcPort->sock = fileno(SOCKET_FILE);
        }
    } else {
        // prepare file reply queue, just in case of cma overflow
        // if unused the file will be kept open till next query.
        if (!SOCKET_FILE) {
            SOCKET_FILE =  fopen(PGS_OLOCK, "w") ;
        }
    }

    MemoryContextSwitchTo(MessageContext);
    MemoryContextResetAndDeleteChildren(MessageContext);

    initStringInfo(&input_message);

    inBuf = &input_message;

	InvalidateCatalogSnapshotConditionally();

	if (send_ready_for_query) {

		if (IsAbortedTransactionBlockState()) {
			PDEBUG("@@@@ TODO 403: idle in transaction (aborted)");
		}
		else if (IsTransactionOrTransactionBlock()) {
			PDEBUG("@@@@ TODO 406: idle in transaction");
		} else {
			if (notifyInterruptPending) {
				ProcessNotifyInterrupt(false);
                notified = true;
            }
        }
        send_ready_for_query = false;
    }
// postgres.c 4627
    DoingCommandRead = true;

#if defined(EMUL_CMA)
    //  temp fix for -O0 but less efficient than literal
    #define IO ((char *)(1+(int)cma_port))
#else
    #define IO ((char *)(1))
#endif

/*
 * in cma mode (cma_rsize>0), client call the wire loop itself waiting synchronously for the results
 * in socketfiles mode, the wire loop polls a pseudo socket made from incoming and outgoing files.
 * in repl mode (cma_rsize==0) output is on stdout not cma/socketfiles wire.
 * repl mode is the simpleset mode where stdin is just copied into input buffer (limited by CMA size).
 * TODO: allow to redirect stdout for fully external repl.
 */

    peek = IO[0];
    packetlen = cma_rsize;

    if (packetlen) {
        sockfiles = false;
        if (!is_repl) {
            whereToSendOutput = DestRemote;
            if (!is_wire)
                PDEBUG("# 439: repl message in cma buffer !");
        } else {
            if (is_wire)
                PDEBUG("# 442: wire message in cma buffer for REPL !");
            whereToSendOutput = DestDebug;
        }
    } else {
        fp = fopen(PGS_IN, "r");
puts("# 475:" PGS_IN "\r\n");
        // read file in socket buffer for SocketBackend to consumme.
        if (fp) {
            fseek(fp, 0L, SEEK_END);
            packetlen = ftell(fp);
            if (packetlen) {
                // set to always true if no REPL.
//                is_wire = true;
                resetStringInfo(inBuf);
                rewind(fp);
                /* peek on first char */
                peek = getc(fp);
                rewind(fp);
                if (is_repl && !is_wire) {
                    // sql in buffer
                    for (int i=0; i<packetlen; i++) {
                        appendStringInfoChar(inBuf, fgetc(fp));
                    }
                    sockfiles = false;
                } else {
                    // auth won't go to REPL, ever.
                    whereToSendOutput = DestRemote;
                    // wire in socket reader
                    pq_recvbuf_fill(fp, packetlen);
                    sockfiles = true;
                }

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

            /* do not forget FD CLEANUP in all cases */
//            fclose(fp);
//            unlink(PGS_IN);

            if (packetlen) {
                // it was startup/auth , write and return fast.
                if (peek<0) {
                    PDEBUG("# 492: handshake/auth/pass skip");
                    goto wire_flush;
                }

                /* else it was wire msg or sql */
#if PGDEBUG
                if (is_wire) {
                    force_echo = true;
                }

#endif
                firstchar = peek;
                goto incoming;
            } // wire msg
PDEBUG("# 507: NO DATA:" PGS_IN  "\n");
        } // fp data read

        // is it REPL in cma ?
        if (!peek)
            goto return_early;

        firstchar = peek ;

        //REPL mode  in zero copy buffer ( lowest wasm memory segment )
        packetlen = strlen(IO);

    } // !cma_rsize -> socketfiles -> repl

#if PGDEBUG
    if (packetlen)
        IO[packetlen]=0; // wire blocks are not zero terminated
    printf("\n# 524: fd=%d is_embed=%d is_repl=%d is_wire=%d fd %s,len=%d cma=%d peek=%d [%s]\n", MyProcPort->sock, is_embed, is_repl, is_wire, PGS_OLOCK, packetlen,cma_rsize, peek, IO);
#endif

    resetStringInfo(inBuf);
    // when cma buffer is used to fake stdin, data is not read by socket/wire backend.
    if (is_repl) {
        for (int i=0; i<packetlen; i++) {
            appendStringInfoChar(inBuf, IO[i]);
        }
    }

    if (packetlen<2) {
        puts("# 536: WARNING: empty packet");
        //cma_rsize= 0;
        if (is_repl)
            pg_prompt();
        // always free cma buffer !!!
        // IO[0] = 0;
        goto return_early;
    }

incoming:
#if defined(__EMSCRIPTEN__) || defined(__wasi__) //PGDEBUG
#   include "pgl_sjlj.c"
#else
    #error "sigsetjmp unsupported"
#endif // wasi


    while (pipelining) {

        if (is_repl) {
            // are we sure repl could not pipeline ?
            pipelining = false;
            /* stdio node repl */
#if PGDEBUG
            printf("\n# 533: enforcing REPL mode, wire off, echo %d\n", force_echo);
#endif
            whereToSendOutput = DestDebug;
        }

        DoingCommandRead = true;
        if (is_wire) {
            /* wire on a socket or cma may auth */
            /* would be handled as error by pg_proto block */
            if (peek==0) {
                PDEBUG("# 540: handshake/auth");
                startup_auth();
                PDEBUG("# 542: auth request");
                break;
            }

            if (peek==112) {
                PDEBUG("# 547: password");
                startup_pass(true);
                break;
            }

            firstchar = SocketBackend(inBuf);

            pipelining = pq_buffer_remaining_data()>0;
        } else {
            /* nowire */
            // pipelining = false;
            if (firstchar == EOF && inBuf->len == 0) {
                firstchar = EOF;
            } else {
                appendStringInfoChar(inBuf, (char) '\0');
            	firstchar = 'Q';
            }
        }
        DoingCommandRead = false;

        if (!ignore_till_sync) {
            /* initially, or after error */
            // send_ready_for_query = true;
            if (notifyInterruptPending)
               ProcessClientReadInterrupt(true);
        } else {
            /* ignoring till sync will skip all pipeline */
            if (firstchar != EOF) {
                if (firstchar != 'S') {
                    continue;
                }
            }
        }

        #include "pg_proto.c"

        if (pipelining) {
            pipelining = pq_buffer_remaining_data()>0;
            if (pipelining && send_ready_for_query) {
                ReadyForQuery(whereToSendOutput);
                send_ready_for_query = false;
            }
        }
    }

    if (!is_repl) {
wire_flush:
        if (!ClientAuthInProgress) {
            /* process notifications (SYNC) */
            if (notifyInterruptPending)
               ProcessNotifyInterrupt(false);

            if (send_ready_for_query) {
//                PDEBUG("# 602: end packet - sending rfq\n");
                ReadyForQuery(DestRemote);
                //done at postgres.c 4623
                send_ready_for_query = false;
            } else {
                PDEBUG("# 606: end packet - with no rfq\n");
            }
        } else {
            PDEBUG("# 609: end packet (ClientAuthInProgress - no rfq)\n");
        }

        if (SOCKET_DATA>0) {
            if (sockfiles) {
                channel = -1;
                if (cma_wsize) {
                    puts("ERROR: cma was not flushed before socketfile interface");
                }
            } else {
                /* wsize may have increased with previous rfq so assign here */
                cma_wsize = SOCKET_DATA;
                channel = cma_rsize + 2;
            }
            if (SOCKET_FILE) {
                int outb = SOCKET_DATA;
                fclose(SOCKET_FILE);
                SOCKET_FILE = NULL;
                SOCKET_DATA = 0;

                if (cma_wsize) {
                    PDEBUG("# 672: cma and sockfile ???\n");
                }

                if (sockfiles) {
#if PGDEBUG
                    printf("# 675: client:ready -> read(%d) " PGS_OLOCK "->" PGS_OUT"\n", outb);
#endif
                    rename(PGS_OLOCK, PGS_OUT);
                }
            } else {
#if PGDEBUG
                printf("\n# 681: in[%d] out[%d] flushed\n", cma_rsize, cma_wsize);
#endif
                SOCKET_DATA = 0;
            }

        } else {
            cma_wsize = 0;
            PDEBUG("# 698: no data, send empty ?");
// TODO: dedup 739
            if (sockfiles) {
                fclose(SOCKET_FILE);
                SOCKET_FILE = NULL;
                rename(PGS_OLOCK, PGS_OUT);
            }
        }
    } else {
        pg_prompt();
#if PGDEBUG
        puts("# 683: repl output");
        if (SOCKET_DATA>0) {
                puts("# 686: socket has data");
            if (sockfiles)
                printf("# 688: socket file not flushed -> read(%d) " PGS_OLOCK "->" PGS_OUT"\n", SOCKET_DATA);
        } else {
// TODO: dedup 723
            if (sockfiles) {
                fclose(SOCKET_FILE);
                SOCKET_FILE = NULL;
                rename(PGS_OLOCK, PGS_OUT);
            }
        }
        if (cma_wsize)
            puts("ERROR: cma was not flushed before socketfile interface");
#endif
    }
return_early:;
    /* always FD CLEANUP */
    if (fp) {
        fclose(fp);
        unlink(PGS_IN);
    }


    // always free kernel buffer !!!
    cma_rsize = 0;
    IO[0] = 0;

    #undef IO

    // reset EX counter
    canary_ex = 0;
}

#undef PGL_LOOP
