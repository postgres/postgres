#define USE_POSIX_TIME
#define USE_POSIX_SIGNALS
#define NO_EMPTY_STMTS
#define SYSV_DIRENT

#ifndef			BYTE_ORDER
#ifdef			MIPSEB
#define			BYTE_ORDER		BIG_ENDIAN
#endif
#endif
