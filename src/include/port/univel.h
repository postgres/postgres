#  define USE_POSIX_TIME 
#  define NO_EMPTY_STMTS
#  define USE_POSIX_SIGNALS
#  define SYSV_DIRENT

#if 0
#  define HAS_TEST_AND_SET
   typedef unsigned char slock_t;
#endif

extern long random(void);
extern void srandom(int seed);
extern int strcasecmp(char *s1,char *s2);
extern int gethostname(char *name,int namelen);

#ifndef		BYTE_ORDER
#define		BYTE_ORDER	LITTLE_ENDIAN
#endif

