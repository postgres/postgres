/* GetPrivateProfileString
 * for UNIX use
 */
#ifndef GPPS_H
#define GPPS_H

#include "psqlodbc.h"

#ifndef WIN32
#include <sys/types.h>
#endif

#define SQLGetPrivateProfileString(a,b,c,d,e,f) GetPrivateProfileString(a,b,c,d,e,f)
#define SQLWritePrivateProfileString(a,b,c,d) WritePrivateProfileString(a,b,c,d)

#ifdef __cplusplus
extern		"C"
{
#endif

DWORD
GetPrivateProfileString(const char *theSection, /* section name */
						const char *theKey,		/* search key name */
						const char *theDefault, /* default value if not
												 * found */
						char *theReturnBuffer,	/* return valuse stored
												 * here */
						size_t theBufferLength, /* byte length of return
												 * buffer */
						const char *theIniFileName);	/* pathname of ini file
														 * to search */

DWORD
WritePrivateProfileString(const char *theSection,		/* section name */
						  const char *theKey,	/* write key name */
						  const char *theBuffer,		/* input buffer */
						  const char *theIniFileName);	/* pathname of ini file
														 * to write */

#ifdef __cplusplus
}
#endif

#ifndef WIN32
#undef DWORD
#endif

#endif
