#define JMP_BUF
#define USE_POSIX_TIME
/* HPUX 10 has full POSIX signal functionality.
 * On HPUX 9, we rely on BSD-compatible signal() in libBSD.
 */
#ifdef HPUX_10
#define USE_POSIX_SIGNALS
#endif
#define HAS_TEST_AND_SET
typedef struct
{
	int			sema[4];
} slock_t;

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
