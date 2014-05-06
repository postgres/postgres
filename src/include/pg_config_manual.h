/*------------------------------------------------------------------------
 * PostgreSQL manual configuration settings
 *
 * This file contains various configuration symbols and limits.  In
 * all cases, changing them is only useful in very rare situations or
 * for developers.  If you edit any of these, be sure to do a *full*
 * rebuild (and an initdb if noted).
 *
 * $PostgreSQL: pgsql/src/include/pg_config_manual.h,v 1.40 2010/01/07 04:53:35 tgl Exp $
 *------------------------------------------------------------------------
 */

/*
 * Maximum length for identifiers (e.g. table names, column names,
 * function names).  Names actually are limited to one less byte than this,
 * because the length must include a trailing zero byte.
 *
 * Changing this requires an initdb.
 */
#define NAMEDATALEN 64

/*
 * Maximum number of arguments to a function.
 *
 * The minimum value is 8 (index cost estimation uses 8-argument functions).
 * The maximum possible value is around 600 (limited by index tuple size in
 * pg_proc's index; BLCKSZ larger than 8K would allow more).  Values larger
 * than needed will waste memory and processing time, but do not directly
 * cost disk space.
 *
 * Changing this does not require an initdb, but it does require a full
 * backend recompile (including any user-defined C functions).
 */
#define FUNC_MAX_ARGS		100

/*
 * Maximum number of columns in an index.  There is little point in making
 * this anything but a multiple of 32, because the main cost is associated
 * with index tuple header size (see access/itup.h).
 *
 * Changing this requires an initdb.
 */
#define INDEX_MAX_KEYS		32

/*
 * Set the upper and lower bounds of sequence values.
 */
#define SEQ_MAXVALUE	INT64CONST(0x7FFFFFFFFFFFFFFF)
#define SEQ_MINVALUE	(-SEQ_MAXVALUE)

/*
 * Number of spare LWLocks to allocate for user-defined add-on code.
 */
#define NUM_USER_DEFINED_LWLOCKS	4

/*
 * Define this if you want to allow the lo_import and lo_export SQL
 * functions to be executed by ordinary users.  By default these
 * functions are only available to the Postgres superuser.  CAUTION:
 * These functions are SECURITY HOLES since they can read and write
 * any file that the PostgreSQL server has permission to access.  If
 * you turn this on, don't say we didn't warn you.
 */
/* #define ALLOW_DANGEROUS_LO_FUNCTIONS */

/*
 * MAXPGPATH: standard size of a pathname buffer in PostgreSQL (hence,
 * maximum usable pathname length is one less).
 *
 * We'd use a standard system header symbol for this, if there weren't
 * so many to choose from: MAXPATHLEN, MAX_PATH, PATH_MAX are all
 * defined by different "standards", and often have different values
 * on the same platform!  So we just punt and use a reasonably
 * generous setting here.
 */
#define MAXPGPATH		1024

/*
 * PG_SOMAXCONN: maximum accept-queue length limit passed to
 * listen(2).  You'd think we should use SOMAXCONN from
 * <sys/socket.h>, but on many systems that symbol is much smaller
 * than the kernel's actual limit.  In any case, this symbol need be
 * twiddled only if you have a kernel that refuses large limit values,
 * rather than silently reducing the value to what it can handle
 * (which is what most if not all Unixen do).
 */
#define PG_SOMAXCONN	10000

/*
 * You can try changing this if you have a machine with bytes of
 * another size, but no guarantee...
 */
#define BITS_PER_BYTE		8

/*
 * Preferred alignment for disk I/O buffers.  On some CPUs, copies between
 * user space and kernel space are significantly faster if the user buffer
 * is aligned on a larger-than-MAXALIGN boundary.  Ideally this should be
 * a platform-dependent value, but for now we just hard-wire it.
 */
#define ALIGNOF_BUFFER	32

/*
 * Disable UNIX sockets for certain operating systems.
 */
#if defined(WIN32)
#undef HAVE_UNIX_SOCKETS
#endif

/*
 * Define this if your operating system supports link()
 */
#if !defined(WIN32) && !defined(__CYGWIN__)
#define HAVE_WORKING_LINK 1
#endif

/*
 * USE_POSIX_FADVISE controls whether Postgres will attempt to use the
 * posix_fadvise() kernel call.  Usually the automatic configure tests are
 * sufficient, but some older Linux distributions had broken versions of
 * posix_fadvise().  If necessary you can remove the #define here.
 */
#if HAVE_DECL_POSIX_FADVISE && defined(HAVE_POSIX_FADVISE)
#define USE_POSIX_FADVISE
#endif

/*
 * USE_PREFETCH code should be compiled only if we have a way to implement
 * prefetching.  (This is decoupled from USE_POSIX_FADVISE because there
 * might in future be support for alternative low-level prefetch APIs.)
 */
#ifdef USE_POSIX_FADVISE
#define USE_PREFETCH
#endif

/*
 * This is the default directory in which AF_UNIX socket files are
 * placed.  Caution: changing this risks breaking your existing client
 * applications, which are likely to continue to look in the old
 * directory.  But if you just hate the idea of sockets in /tmp,
 * here's where to twiddle it.  You can also override this at runtime
 * with the postmaster's -k switch.
 */
#define DEFAULT_PGSOCKET_DIR  "/tmp"

/*
 * The random() function is expected to yield values between 0 and
 * MAX_RANDOM_VALUE.  Currently, all known implementations yield
 * 0..2^31-1, so we just hardwire this constant.  We could do a
 * configure test if it proves to be necessary.  CAUTION: Think not to
 * replace this with RAND_MAX.  RAND_MAX defines the maximum value of
 * the older rand() function, which is often different from --- and
 * considerably inferior to --- random().
 */
#define MAX_RANDOM_VALUE  (0x7FFFFFFF)


/*
 *------------------------------------------------------------------------
 * The following symbols are for enabling debugging code, not for
 * controlling user-visible features or resource limits.
 *------------------------------------------------------------------------
 */

/*
 * Define this to cause pfree()'d memory to be cleared immediately, to
 * facilitate catching bugs that refer to already-freed values.
 * Right now, this gets defined automatically if --enable-cassert.
 */
#ifdef USE_ASSERT_CHECKING
#define CLOBBER_FREED_MEMORY
#endif

/*
 * Define this to check memory allocation errors (scribbling on more
 * bytes than were allocated).  Right now, this gets defined
 * automatically if --enable-cassert.
 */
#ifdef USE_ASSERT_CHECKING
#define MEMORY_CONTEXT_CHECKING
#endif

/*
 * Define this to cause palloc()'d memory to be filled with random data, to
 * facilitate catching code that depends on the contents of uninitialized
 * memory.  Caution: this is horrendously expensive.
 */
/* #define RANDOMIZE_ALLOCATED_MEMORY */

/*
 * Define this to force all parse and plan trees to be passed through
 * copyObject(), to facilitate catching errors and omissions in
 * copyObject().
 */
/* #define COPY_PARSE_PLAN_TREES */

/*
 * Enable debugging print statements for lock-related operations.
 */
/* #define LOCK_DEBUG */

/*
 * Enable debugging print statements for WAL-related operations; see
 * also the wal_debug GUC var.
 */
/* #define WAL_DEBUG */

/*
 * Enable tracing of resource consumption during sort operations;
 * see also the trace_sort GUC var.  For 8.1 this is enabled by default.
 */
#define TRACE_SORT 1

/*
 * Enable tracing of syncscan operations (see also the trace_syncscan GUC var).
 */
/* #define TRACE_SYNCSCAN */

/*
 * Other debug #defines (documentation, anyone?)
 */
/* #define HEAPDEBUGALL */
/* #define ACLDEBUG */
/* #define RTDEBUG */
