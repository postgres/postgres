/*------------------------------------------------------------------------
 * PostgreSQL manual configuration settings
 *
 * This file contains various configuration symbols and limits.  In
 * all cases, changing them is only useful in very rare situations or
 * for developers.	If you edit any of these, be sure to do a *full*
 * rebuild (and an initdb if noted).
 *
 * $PostgreSQL: pgsql/src/include/pg_config_manual.h,v 1.27 2007/06/08 18:23:53 tgl Exp $
 *------------------------------------------------------------------------
 */

/*
 * Size of a disk block --- this also limits the size of a tuple.  You
 * can set it bigger if you need bigger tuples (although TOAST should
 * reduce the need to have large tuples, since fields can be spread
 * across multiple tuples).
 *
 * BLCKSZ must be a power of 2.  The maximum possible value of BLCKSZ
 * is currently 2^15 (32768).  This is determined by the 15-bit widths
 * of the lp_off and lp_len fields in ItemIdData (see
 * include/storage/itemid.h).
 *
 * Changing BLCKSZ requires an initdb.
 */
#define BLCKSZ	8192

/*
 * RELSEG_SIZE is the maximum number of blocks allowed in one disk
 * file.  Thus, the maximum size of a single file is RELSEG_SIZE *
 * BLCKSZ; relations bigger than that are divided into multiple files.
 *
 * RELSEG_SIZE * BLCKSZ must be less than your OS' limit on file size.
 * This is often 2 GB or 4GB in a 32-bit operating system, unless you
 * have large file support enabled.  By default, we make the limit 1
 * GB to avoid any possible integer-overflow problems within the OS.
 * A limit smaller than necessary only means we divide a large
 * relation into more chunks than necessary, so it seems best to err
 * in the direction of a small limit.  (Besides, a power-of-2 value
 * saves a few cycles in md.c.)
 *
 * Changing RELSEG_SIZE requires an initdb.
 */
#define RELSEG_SIZE (0x40000000 / BLCKSZ)

/*
 * Size of a WAL file block.  This need have no particular relation to BLCKSZ.
 * XLOG_BLCKSZ must be a power of 2, and if your system supports O_DIRECT I/O,
 * XLOG_BLCKSZ must be a multiple of the alignment requirement for direct-I/O
 * buffers, else direct I/O may fail.
 *
 * Changing XLOG_BLCKSZ requires an initdb.
 */
#define XLOG_BLCKSZ		8192

/*
 * XLOG_SEG_SIZE is the size of a single WAL file.	This must be a power of 2
 * and larger than XLOG_BLCKSZ (preferably, a great deal larger than
 * XLOG_BLCKSZ).
 *
 * Changing XLOG_SEG_SIZE requires an initdb.
 */
#define XLOG_SEG_SIZE	(16*1024*1024)

/*
 * Maximum length for identifiers (e.g. table names, column names,
 * function names).  It must be a multiple of sizeof(int) (typically
 * 4).
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
 * Number of spare LWLocks to allocate for user-defined add-on code.
 */
#define NUM_USER_DEFINED_LWLOCKS	4

/*
 * Define this to make libpgtcl's "pg_result -assign" command process
 * C-style backslash sequences in returned tuple data and convert
 * PostgreSQL array values into Tcl lists.	CAUTION: This conversion
 * is *wrong* unless you install the routines in
 * contrib/string/string_io to make the server produce C-style
 * backslash sequences in the first place.
 */
/* #define TCL_ARRAYS */

/*
 * Define this if you want psql to _always_ ask for a username and a
 * password for password authentication.
 */
/* #define PSQL_ALWAYS_GET_PASSWORDS */

/*
 * Define this if you want to allow the lo_import and lo_export SQL
 * functions to be executed by ordinary users.	By default these
 * functions are only available to the Postgres superuser.	CAUTION:
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
 * Disable UNIX sockets for those operating system.
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
 * This is the default directory in which AF_UNIX socket files are
 * placed.	Caution: changing this risks breaking your existing client
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
 * replace this with RAND_MAX.	RAND_MAX defines the maximum value of
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
 * facilitate catching bugs that refer to already-freed values.  XXX
 * Right now, this gets defined automatically if --enable-cassert.	In
 * the long term it probably doesn't need to be on by default.
 */
#ifdef USE_ASSERT_CHECKING
#define CLOBBER_FREED_MEMORY
#endif

/*
 * Define this to check memory allocation errors (scribbling on more
 * bytes than were allocated).	Right now, this gets defined
 * automatically if --enable-cassert.  In the long term it probably
 * doesn't need to be on by default.
 */
#ifdef USE_ASSERT_CHECKING
#define MEMORY_CONTEXT_CHECKING
#endif

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
