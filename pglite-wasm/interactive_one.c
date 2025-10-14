#include <unistd.h>  // access, unlink
#define PGL_LOOP
#if defined(__wasi__)
// volatile sigjmp_buf void*;
#else
volatile sigjmp_buf local_sigjmp_buf;
#endif

// track back how many ex raised in steps of the loop until sucessfull clear_error
volatile int canary_ex = 0;

// read FROM JS
// (i guess return number of bytes written)
// ssize_t pglite_read(/* ignored */ int socket, void *buffer, size_t length,/* ignored */ int flags,/* ignored */ void *address,/* ignored */ socklen_t *address_len);
//typedef ssize_t (*pglite_read_t)(/* ignored */ int socket, void *buffer, size_t length,/* ignored */ int flags,/* ignored */ void *address,/* ignored */ unsigned int *address_len);
typedef ssize_t (*pglite_read_t)(void *buffer, size_t max_length);
extern pglite_read_t pglite_read;

// write TO JS
// (i guess return number of bytes read)
// ssize_t pglite_write(/* ignored */ int sockfd, const void *buf, size_t len, /* ignored */ int flags);
// typedef ssize_t (*pglite_write_t)(/* ignored */ int sockfd, const void *buf, size_t len, /* ignored */ int flags);
typedef ssize_t (*pglite_write_t)(void *buffer, size_t length);
extern pglite_write_t pglite_write;

__attribute__((export_name("set_read_write_cbs")))
void
set_read_write_cbs(pglite_read_t read_cb, pglite_write_t write_cb) {
    pglite_read = read_cb;
    pglite_write = write_cb;
}

extern void AbortTransaction(void);
extern void CleanupTransaction(void);
extern void ClientAuthentication(Port *port);

extern int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);

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
    PDEBUG("\n\n\n# 165: io_init  --------- Ready for CLIENT ---------");
}


volatile bool is_wire = true;
extern void pq_startmsgread(void);

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
    send_ready_for_query = false;

    if (ProcessStartupPacket(MyProcPort, true, true) != STATUS_OK) {
        PDEBUG("# 271: ProcessStartupPacket !OK");
    } else {

        sf_connected++;
        PDEBUG("# 273: sending auth request");
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

__attribute__((export_name("interactive_one"))) void
interactive_one(int packetlen, int peek) {
    // int	peek = -1;  /* preview of firstchar with no pos change */
	int firstchar = 0;  /* character read from getc() */
    bool pipelining = true;
	StringInfoData input_message;
	StringInfoData *inBuf;
    FILE *stream ;
    FILE *fp = NULL;
    // int packetlen;

    bool had_notification = notifyInterruptPending;
    bool notified = false;
    // send_ready_for_query = false;

    if (!MyProcPort) {
        PDEBUG("# 353: client created");
        io_init(is_wire, false);
    }

#if PGDEBUG
    puts("\n\n# 369: interactive_one");
    if (notifyInterruptPending)
        PDEBUG("# 371: has notification !");
#endif

    MemoryContextSwitchTo(MessageContext);
    MemoryContextReset(MessageContext);

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

    DoingCommandRead = true;

    whereToSendOutput = DestRemote;

#if PGDEBUG
    if (packetlen)
        IO[packetlen]=0; // wire blocks are not zero terminated
    printf("\n# 524: fd=%d is_embed=%d is_repl=%d is_wire=%d fd %s,len=%d peek=%d [%s]\n", MyProcPort->sock, is_embed, is_repl, is_wire, PGS_OLOCK, packetlen, peek, IO);
#endif

    resetStringInfo(inBuf);

    if (packetlen<2) {
        puts("# 536: WARNING: empty packet");
        goto return_early;
    }

incoming:
#if defined(__EMSCRIPTEN__) || defined(__wasi__) //PGDEBUG
#   include "pgl_sjlj.c"
#else
    #error "sigsetjmp unsupported"
#endif // wasi


    while (pipelining) {
        DoingCommandRead = true;
        if (is_wire) {
            /* wire on a socket may auth */
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
#if PGDEBUG
            if (!pipelining) {
                printf("# 556: end of wire, rfq=%d\n", send_ready_for_query);
            } else {
                printf("# 558: no end of wire -> pipelining, rfq=%d\n", send_ready_for_query);
            }
#endif
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
puts("# 631:  PIPELINING + rfq");
                ReadyForQuery(whereToSendOutput);
                send_ready_for_query = false;
            }
        }
    }

wire_flush:
    if (!ClientAuthInProgress) {
        /* process notifications (SYNC) */
        if (notifyInterruptPending)
           ProcessNotifyInterrupt(false);
        if (send_ready_for_query) {
            PDEBUG("# 602: end packet - sending rfq\n");
            ReadyForQuery(DestRemote);
            //done at postgres.c 4623
            send_ready_for_query = false;
        } else {
            PDEBUG("# 606: end packet - with no rfq\n");
        }
    } else {
        PDEBUG("# 609: end packet (ClientAuthInProgress - no rfq)\n");
    }

return_early:;
    // reset EX counter
    canary_ex = 0;
    pq_flush();
}

#undef PGL_LOOP