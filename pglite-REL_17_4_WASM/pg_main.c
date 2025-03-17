#define PGL_MAIN
#define PGL_INITDB_MAIN
// #define PGDEBUG_STARTUP

// MEMFS files for os pipe simulation
#define IDB_PIPE_BOOT "/tmp/initdb.boot.txt"
#define IDB_PIPE_SINGLE "/tmp/initdb.single.txt"

#include "pgl_os.h"

// ----------------------- pglite ----------------------------
#include "postgres.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "tcop/tcopprot.h"

#include <unistd.h>        /* chdir */
#include <sys/stat.h>      /* mkdir */

// globals

#define MemoryContextResetAndDeleteChildren(...)
// #define SpinLockInit(...)



int g_argc;
char **g_argv;
extern char ** environ;

volatile char *PREFIX;
volatile char *PGDATA;
volatile char *PGUSER;

const char * progname;

volatile bool is_repl = true;
volatile bool is_node = true;
volatile bool is_embed = false;
volatile int pgl_idb_status;

// will backend restart after initdb. defaut is yes.
// TODO: log sync start failures and ask to repair/clean up db.
volatile int async_restart = 1;

#define IDB_OK  0b11111110
#define IDB_FAILED  0b0001
#define IDB_CALLED  0b0010
#define IDB_HASDB   0b0100
#define IDB_HASUSER 0b1000


#define WASM_PGDATA WASM_PREFIX "/base"
#define CMA_FD 1

extern bool IsPostmasterEnvironment;

#define help(name)

#define BREAKV(x) { printf("BREAKV : %d\n",__LINE__);return x; }
#define BREAK { printf("BREAK : %d\n",__LINE__);return; }


extern int pgl_initdb_main(void);
extern void pg_proc_exit(int code);
extern int BootstrapModeMain(int, char **, int);


// PostgresSingleUserMain / PostgresMain

#include "miscadmin.h"
#include "access/xlog.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/timestamp.h"
#include "utils/guc.h"
#include "pgstat.h"
#include "replication/walsender.h"
#include "libpq/pqformat.h"




volatile bool send_ready_for_query = true;
volatile bool idle_in_transaction_timeout_enabled = false;
volatile bool idle_session_timeout_enabled = false;
/*
bool quote_all_identifiers = false;
FILE* SOCKET_FILE = NULL;
int SOCKET_DATA = 0;
*/

void pg_free(void *ptr) {
    free(ptr);
}

#include "../backend/tcop/postgres.c"


// initdb + start on fd (pipe emulation)


static bool force_echo = false;


#include "pgl_mains.c"

#include "pgl_stubs.h"

#include "pgl_tools.h"

#include "pgl_initdb.c"


// interactive_one

#include "./interactive_one.c"


static void
main_pre(int argc, char *argv[]) {


    char key[256];
    int i=0;
// extra env is always after normal args
    PDEBUG("# ============= extra argv dump ==================");
    {
        for (;i<argc;i++) {
            const char *kv = argv[i];
            for (int sk=0;sk<strlen(kv);sk++)
                if(kv[sk]=='=')
                    goto extra_env;
#if PGDEBUG
            printf("%s ", kv);
#endif
        }
    }
extra_env:;
    PDEBUG("\n# ============= arg->env dump ==================");
    {
        for (;i<argc;i++) {
            const char *kv = argv[i];
            for (int sk=0;sk<strlen(kv);sk++) {
                if (sk>255) {
                    puts("buffer overrun on extra env at:");
                    puts(kv);
                    continue;
                }
                if (kv[sk]=='=') {
                    memcpy(key, kv, sk);
                    key[sk] = 0;
#if PGDEBUG
                    printf("%s='%s'\n", &(key[0]), &(kv[sk+1]));
#endif
                    setenv(key, &kv[sk+1], 1);
                }
            }
        }
    }

    // get default or set default if not set
    PREFIX = setdefault("PREFIX", WASM_PREFIX);
    argv[0] = strcat_alloc( PREFIX, "/bin/postgres");



#if defined(__EMSCRIPTEN__)
    EM_ASM({
        Module.is_worker = (typeof WorkerGlobalScope !== 'undefined') && self instanceof WorkerGlobalScope;
        Module.FD_BUFFER_MAX = $0;
        Module.emscripten_copy_to = console.warn;
    }, (CMA_MB*1024*1024) / CMA_FD);  /* ( global mem start / num fd max ) */

    if (is_node) {
    	setenv("ENVIRONMENT", "node" , 1);
        EM_ASM({
#if defined(PGDEBUG_STARTUP)
            console.warn("prerun(C-node) worker=", Module.is_worker);
#endif
            Module['postMessage'] = function custom_postMessage(event) {
                console.log("# pg_main_emsdk.c:544: onCustomMessage:", event);
            };
        });

    } else {
    	setenv("ENVIRONMENT", "web" , 1);
#if defined(PGDEBUG_STARTUP)
        EM_ASM({
            console.warn("prerun(C-web) worker=", Module.is_worker);
        });
#endif
        is_repl = true;
    }

    EM_ASM({
        if (Module.is_worker) {
#if defined(PGDEBUG_STARTUP)
            console.log("Main: running in a worker, setting onCustomMessage");
#endif
            function onCustomMessage(event) {
                console.log("onCustomMessage:", event);
            };
            Module['onCustomMessage'] = onCustomMessage;
        } else {
#if defined(PGDEBUG_STARTUP)
            console.log("Running in main thread, faking onCustomMessage");
#endif
            Module['postMessage'] = function custom_postMessage(event) {
                switch (event.type) {
                    case "raw" :  {
                        //stringToUTF8( event.data, shm_rawinput, Module.FD_BUFFER_MAX);
                        break;
                    }

                    case "stdin" :  {
                        stringToUTF8( event.data, 1, Module.FD_BUFFER_MAX);
                        break;
                    }
                    case "rcon" :  {
                        //stringToUTF8( event.data, shm_rcon, Module.FD_BUFFER_MAX);
                        break;
                    }
                    default : console.warn("custom_postMessage?", event);
                }
            };
            //if (!window.vm)
              //  window.vm = Module;
        };
    });

#endif // __EMSCRIPTEN__
	chdir("/");
    mkdirp("/tmp");
    mkdirp(PREFIX);

	// postgres does not know where to find the server configuration file.
    // also we store the fake locale file there.
	// postgres.js:1605 You must specify the --config-file or -D invocation option or set the PGDATA environment variable.

    /* enforce ? */
	setenv("PGSYSCONFDIR", PREFIX, 1);
	setenv("PGCLIENTENCODING", "UTF8", 1);

    // default is to run a repl loop
    setenv("REPL", "Y", 0);
/*
 * we cannot run "locale -a" either from web or node. the file getenv("PGSYSCONFDIR") / "locale"
 * serves as popen output
 */

	setenv("LC_CTYPE", "en_US.UTF-8" , 1);

    /* defaults */

    setenv("TZ", "UTC", 0);
    setenv("PGTZ", "UTC", 0);
	setenv("PGDATABASE", "template1" , 0);
    setenv("PG_COLOR", "always", 0);


    /* defaults with possible user setup */
    PGUSER = setdefault("PGUSER", WASM_USERNAME);

    /* temp override for inidb */
    setenv("PGUSER",  WASM_USERNAME, 1);

    strconcat(tmpstr, PREFIX, "/base" );
    PGDATA = setdefault("PGDATA", tmpstr);


#if PGDEBUG
    puts("# ============= env dump ==================");
    for (char **env = environ; *env != 0; env++) {
        char *drefp = *env;
        printf("# %s\n", drefp);
    }
    puts("# =========================================");
#endif
} // main_pre



void main_post() {
PDEBUG("# 306: main_post()");
        /*
         * Fire up essential subsystems: error and memory management
         *
         * Code after this point is allowed to use elog/ereport, though
         * localization of messages may not work right away, and messages won't go
         * anywhere but stderr until GUC settings get loaded.
         */
        MemoryContextInit();

        /*
         * Set up locale information
         */
        set_pglocale_pgservice(g_argv[0], PG_TEXTDOMAIN("postgres"));

        /*
         * In the postmaster, absorb the environment values for LC_COLLATE and
         * LC_CTYPE.  Individual backends will change these later to settings
         * taken from pg_database, but the postmaster cannot do that.  If we leave
         * these set to "C" then message localization might not work well in the
         * postmaster.
         */
        init_locale("LC_COLLATE", LC_COLLATE, "");
        init_locale("LC_CTYPE", LC_CTYPE, "");

        /*
         * LC_MESSAGES will get set later during GUC option processing, but we set
         * it here to allow startup error messages to be localized.
         */
    #ifdef LC_MESSAGES
        init_locale("LC_MESSAGES", LC_MESSAGES, "");
    #endif

        /*
         * We keep these set to "C" always, except transiently in pg_locale.c; see
         * that file for explanations.
         */
        init_locale("LC_MONETARY", LC_MONETARY, "C");
        init_locale("LC_NUMERIC", LC_NUMERIC, "C");
        init_locale("LC_TIME", LC_TIME, "C");

        /*
         * Now that we have absorbed as much as we wish to from the locale
         * environment, remove any LC_ALL setting, so that the environment
         * variables installed by pg_perm_setlocale have force.
         */
        unsetenv("LC_ALL");
} // main_post


__attribute__((export_name("pgl_backend")))
void pgl_backend() {
#if PGDEBUG
    print_bits(sizeof(pgl_idb_status), &pgl_idb_status);
#endif
    if (!(pgl_idb_status&IDB_CALLED)) {
        puts("# 349: initdb must be called before starting/resuming backend");
        //abort();
    }

    if (async_restart) {
// old 487
    {
#if PGDEBUG
        fprintf(stdout, "\n\n\n# 483: restarting in single mode after initdb with user '%s' instead of %s\n", getenv("PGUSER"), PGUSER);
        setenv("PGUSER", PGUSER, 1);
#endif
        char *single_argv[] = {
            WASM_PREFIX "/bin/postgres",
            "--single",
            "-d", "1", "-B", "16", "-S", "512", "-f", "siobtnmh",
            "-D", PGDATA,
            "-F", "-O", "-j",
            WASM_PGOPTS,
            "template1",
            NULL
        };
        int single_argc = sizeof(single_argv) / sizeof(char*) - 1;
        optind = 1;
        RePostgresSingleUserMain(single_argc, single_argv, PGUSER);
PDEBUG("# 384: initdb faking shutdown to complete WAL/OID states in single mode");
    }
        goto backend_started;

    }

    main_post();

    char *single_argv[] = {
        g_argv[0],
        "--single",
        "-d", "1", "-B", "16", "-S", "512", "-f", "siobtnmh",
        "-D", PGDATA,
        "-F", "-O", "-j",
        WASM_PGOPTS,
        getenv("PGDATABASE"),
        NULL
    };
    int single_argc = sizeof(single_argv) / sizeof(char*) - 1;
    optind = 1;
#if PGDEBUG
        fprintf(stdout, "\n\n\n# 405: resuming db with user '%s' instead of %s\n", PGUSER, getenv("PGUSER"));
#endif
    setenv("PGUSER", PGUSER, 1);

    AsyncPostgresSingleUserMain(single_argc, single_argv, PGUSER, async_restart);


backend_started:;
    IsPostmasterEnvironment = true;
    if (TransamVariables->nextOid < ((Oid) FirstNormalObjectId)) {
        /* IsPostmasterEnvironment is now true
         these will be executed when required in varsup.c/GetNewObjectId
    	 TransamVariables->nextOid = FirstNormalObjectId;
	     TransamVariables->oidCount = 0;
        */
#if PGDEBUG
        puts("# 382: initdb done, oid base too low but OID range will be set because IsPostmasterEnvironment");
#endif
    }
}

#if defined(__EMSCRIPTEN__)
EMSCRIPTEN_KEEPALIVE
#else
__attribute__((export_name("pgl_initdb")))
#endif
int
pgl_initdb() {
    PDEBUG("# 433: pg_initdb()");
    optind = 1;
    pgl_idb_status |= IDB_FAILED;

    if (!chdir(PGDATA)){
        if (access("PG_VERSION", F_OK) == 0) {
        	chdir("/");

            pgl_idb_status |= IDB_HASDB;

            /* assume auth success for now */
            pgl_idb_status |= IDB_HASUSER;
#if PGDEBUG
            fprintf(stdout, "# 446: pg_initdb: db exists at : %s TODO: test for db name : %s \n", PGDATA, getenv("PGDATABASE"));
#endif // PGDEBUG

            async_restart = 0;
            goto initdb_done;
        }
    	chdir("/");
#if PGDEBUG
        fprintf(stderr, "# 454: pg_initdb no db found at : %s\n", PGDATA );
#endif // PGDEBUG
    } else {
#if PGDEBUG
        fprintf(stderr, "# 458: pg_initdb db folder not found at : %s\n", PGDATA );
#endif // PGDEBUG
    }

    int initdb_rc = pgl_initdb_main();

#if PGDEBUG
    fprintf(stderr, "\n\n# 465: " __FILE__ "pgl_initdb_main = %d\n", initdb_rc );
#endif // PGDEBUG
    PDEBUG("# 467:" __FILE__);
    /* save stdin and use previous initdb output to feed boot mode */
    int saved_stdin = dup(STDIN_FILENO);
    {
        PDEBUG("# 471: restarting in boot mode for initdb");
        freopen(IDB_PIPE_BOOT, "r", stdin);

        char *boot_argv[] = {
            g_argv[0],
            "--boot",
            "-D", PGDATA,
            "-d","3",
            WASM_PGOPTS,
            "-X", "1048576",
            NULL
        };
        int boot_argc = sizeof(boot_argv) / sizeof(char*) - 1;

	    set_pglocale_pgservice(boot_argv[0], PG_TEXTDOMAIN("initdb"));

        optind = 1;
        BootstrapModeMain(boot_argc, boot_argv, false);
        fclose(stdin);
        remove(IDB_PIPE_BOOT);
        stdin = fdopen(saved_stdin, "r");

        PDEBUG("# 493: initdb faking shutdown to complete WAL/OID states");
        pg_proc_exit(66);
    }

    /* use previous initdb output to feed single mode */

    /* or resume a previous db */
    //IsPostmasterEnvironment = true;
    if (TransamVariables->nextOid < ((Oid) FirstNormalObjectId)) {
#if PGDEBUG
        puts("# 503: warning oid base too low, will need to set OID range after initdb(bootstrap/single)");
#endif
    }
/*
    {
#if PGDEBUG
        fprintf(stdout, "\n\n\n# 483: restarting in single mode for initdb with user '%s' instead of %s\n", getenv("PGUSER"), PGUSER);
#endif
        char *single_argv[] = {
            WASM_PREFIX "/bin/postgres",
            "--single",
            "-d", "1", "-B", "16", "-S", "512", "-f", "siobtnmh",
            "-D", PGDATA,
            "-F", "-O", "-j",
            WASM_PGOPTS,
            "template1",
            NULL
        };
        int single_argc = sizeof(single_argv) / sizeof(char*) - 1;
        optind = 1;
        RePostgresSingleUserMain(single_argc, single_argv, WASM_USERNAME);
PDEBUG("# 498: initdb faking shutdown to complete WAL/OID states in single mode");
        async_restart = 1;
    }
*/
        async_restart = 1;
initdb_done:;
    pgl_idb_status |= IDB_CALLED;

    if (optind>0) {
        /* RESET getopt */
        optind = 1;
        /* we did not fail, clear the default failed state */
        pgl_idb_status &= IDB_OK;
    } else {
        PDEBUG("# 511: exiting on initdb-single error");
        // TODO raise js exception
    }
    return pgl_idb_status;
} // pg_initdb





int
main_repl() {
    bool hadloop_error = false;

    whereToSendOutput = DestNone;

    if (!mkdir(PGDATA, 0700)) {
        /* no db : run initdb now. */
#if defined(PGDEBUG_STARTUP)
        fprintf(stderr, "PGDATA=%s not found, running initdb with default=%s\n",PGDATA, WASM_PGDATA );
#endif
        #if defined(PG_INITDB_MAIN)
            #warning "web build"
            hadloop_error = pg_initdb() & IDB_FAILED;
        #else
            #warning "node build"
            #if defined(__wasi__)
                hadloop_error = pg_initdb() & IDB_FAILED;
            #endif
        #endif

    } else {
        // download a db case ?
    	mkdirp(WASM_PGDATA);

        // db fixup because empty dirs may not be packaged in git case
	    mksub_dir(WASM_PGDATA, "/pg_wal");
	    mksub_dir(WASM_PGDATA, "/pg_wal/archive_status");
	    mksub_dir(WASM_PGDATA, "/pg_wal/summaries");

	    mksub_dir(WASM_PGDATA, "/pg_tblspc");
	    mksub_dir(WASM_PGDATA, "/pg_snapshots");
	    mksub_dir(WASM_PGDATA, "/pg_commit_ts");
	    mksub_dir(WASM_PGDATA, "/pg_notify");
	    mksub_dir(WASM_PGDATA, "/pg_replslot");
	    mksub_dir(WASM_PGDATA, "/pg_twophase");

	    mksub_dir(WASM_PGDATA, "/pg_logical");
	    mksub_dir(WASM_PGDATA, "/pg_logical/snapshots");
	    mksub_dir(WASM_PGDATA, "/pg_logical/mappings");

    }

    if (!hadloop_error) {
        main_post();

        /*
         * Catch standard options before doing much else, in particular before we
         * insist on not being root.
         */
        if (g_argc > 1) {
	        if (strcmp(g_argv[1], "--help") == 0 || strcmp(g_argv[1], "-?") == 0)
	        {
		        help(progname);
		        exit(0);
	        }
	        if (strcmp(g_argv[1], "--version") == 0 || strcmp(g_argv[1], "-V") == 0)
	        {
		        fputs(PG_BACKEND_VERSIONSTR, stdout);
		        exit(0);
	        }

        }

        if (g_argc > 1 && strcmp(g_argv[1], "--check") == 0) {
	        BootstrapModeMain(g_argc, g_argv, true);
            return 0;
        }

        if (g_argc > 1 && strcmp(g_argv[1], "--boot") == 0) {
            PDEBUG("# 1410: boot: " __FILE__ );
            BootstrapModeMain(g_argc, g_argv, false);
            return 0;
        }

        PDEBUG("# 570: single: " __FILE__ );
        AsyncPostgresSingleUserMain(g_argc, g_argv, PGUSER, 0);
    }
    return 0;
}







/*
    PGDATESTYLE
    TZ
    PG_SHMEM_ADDR

    PGCTLTIMEOUT
    PG_TEST_USE_UNIX_SOCKETS
    INITDB_TEMPLATE
    PSQL_HISTORY
    TMPDIR
    PGOPTIONS
*/




// ???? __attribute__((export_name("_main")))
int
main(int argc, char **argv)
{
    int exit_code = 0;
    main_pre(argc,argv);
#if PGDEBUG
    printf("# 616: argv0 (%s) PGUSER=%s PGDATA=%s\n PGDATABASE=%s REPL=%s\n",
        argv[0], PGUSER, PGDATA,  getenv("PGDATABASE"), getenv("REPL") );
#endif
	progname = get_progname(argv[0]);
    startup_hacks(progname);
    g_argv = argv;
    g_argc = argc;

    is_repl = strlen(getenv("REPL")) && getenv("REPL")[0]!='N';
    is_embed = true;

    if (!is_repl) {
        PDEBUG("# 628: exit with live runtime (nodb)");
        return 0;
    }
/*
    main_post();

    PDEBUG("# 634: repl");
    // so it is repl
    main_repl();

    if (is_node) {
        PDEBUG("# 639: node repl");
        pg_repl_raf();
    }
*/
    emscripten_force_exit(exit_code);
	return exit_code;
}
















