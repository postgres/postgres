/* see src/backend/libpq/pqcomm.c */
#define SCO_ACCEPT_BUG

/***************************************
 * Define this if you are compiling with
 * the native UNIXWARE C compiler.
 ***************************************/
#define USE_UNIVEL_CC

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
