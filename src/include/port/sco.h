/* see src/backend/libpq/pqcomm.c */
#define SCO_ACCEPT_BUG

#define HAS_TEST_AND_SET
#define NEED_I386_TAS_ASM

#define USE_UNIVEL_CC

#if defined(__ia64)
typedef unsigned int slock_t;
#else
typedef unsigned char slock_t;
#endif


#ifndef			BIG_ENDIAN
#define			BIG_ENDIAN		4321
#endif
#ifndef			LITTLE_ENDIAN
#define			LITTLE_ENDIAN	1234
#endif
#ifndef			PDP_ENDIAN
#define			PDP_ENDIAN		3412
#endif
#ifndef			BYTE_ORDER
#define			BYTE_ORDER		LITTLE_ENDIAN

#endif
