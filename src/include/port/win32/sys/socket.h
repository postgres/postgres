/*
 * Unfortunately, <wingdi.h> of VC++ also defines ERROR.
 * To avoid the conflict, we include <windows.h> here and undefine ERROR
 * immediately.
 *
 * Note: Don't include <wingdi.h> directly.  It causes compile errors.
 */
#include <winsock2.h>
#undef ERROR
#undef small

/* Restore old ERROR value */
#ifdef PGERROR
#define ERROR PGERROR

#endif
