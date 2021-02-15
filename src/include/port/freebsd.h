/* src/include/port/freebsd.h */

/*
 * Set the default wal_sync_method to fdatasync.  xlogdefs.h's normal rules
 * would prefer open_datasync on FreeBSD 13+, but that is not a good choice on
 * many systems.
 */
#ifdef HAVE_FDATASYNC
#define PLATFORM_DEFAULT_SYNC_METHOD	SYNC_METHOD_FDATASYNC
#endif
