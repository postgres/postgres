#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include "win32.h"

BOOL		WINAPI
DllMain(HINSTANCE hinstDLL, DWORD fdwReason,
		LPVOID lpReserved)
{
	WSADATA		wsaData;

	switch (fdwReason)
	{
		case DLL_PROCESS_ATTACH:
			if (WSAStartup(MAKEWORD(1, 1), &wsaData))
			{

				/*
				 * No really good way to do error handling here, since we
				 * don't know how we were loaded
				 */
				return FALSE;
			}
		    if (netmsgModule == NULL){
				netmsgModule=LoadLibraryEx("netmsg.dll",NULL,LOAD_LIBRARY_AS_DATAFILE);
			}
			break;
		case DLL_PROCESS_DETACH:
		    if (netmsgModule != NULL)
				FreeLibrary(netmsgModule);
			WSACleanup();
			break;
	}

	return TRUE;
}
