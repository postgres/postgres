// GetPrivateProfileString
// for UNIX use
#ifndef GPPS_H
#define GPPS_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef UNIX
#include <sys/types.h>
#include "iodbc.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

DWORD
GetPrivateProfileString(char *theSection,   // section name
    char *theKey,                           // search key name
    char *theDefault,                       // default value if not found
    char *theReturnBuffer,                  // return valuse stored here
    size_t theBufferLength,                 // byte length of return buffer
    char *theIniFileName);                  // pathname of ini file to search

DWORD
WritePrivateProfileString(char *theSection,	// section name
	char *theKey,		// write key name
	char *theBuffer,	// input buffer
	char *theIniFileName);	// pathname of ini file to write

#ifdef __cplusplus
}
#endif

#ifdef UNIX
#undef DWORD
#endif
#endif
