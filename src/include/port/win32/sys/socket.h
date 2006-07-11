/*
 * Unfortunately, <wingdi.h> of VC++ also defines ERROR.
 * To avoid the conflict, we include <windows.h> here and undefine ERROR
 * immediately.
 *
 * Note: Don't include <wingdi.h> directly.  It causes compile errors.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR
#undef small

/* Restore old ERROR value */
#ifdef PGERROR
#define ERROR PGERROR

/*
 * we can't use the windows gai_strerror{AW} functions because
 * they are defined inline in the MS header files. So we'll use our
 * own
 */
#undef gai_strerror

#endif
