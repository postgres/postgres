#define JMP_BUF
#define USE_POSIX_TIME
#define HAS_TEST_AND_SET
typedef struct
{
	int			sema[4];
} slock_t;

/* HPUX 9 has snprintf in the library, so configure will set HAVE_SNPRINTF;
 * but it doesn't provide a prototype for it.  To suppress warning messages
 * from gcc, do this to make c.h provide the prototype:
 */
#ifndef HAVE_VSNPRINTF
#undef HAVE_SNPRINTF
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
#define			BYTE_ORDER		BIG_ENDIAN
#endif
