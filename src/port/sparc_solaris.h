#  define USE_POSIX_TIME 
#  define USE_POSIX_SIGNALS
#  define NO_EMPTY_STMTS
#  define SYSV_DIRENT
#  define HAS_TEST_AND_SET
   typedef unsigned char slock_t;

#ifndef		BYTE_ORDER
#define		BYTE_ORDER	BIG_ENDIAN
#endif
