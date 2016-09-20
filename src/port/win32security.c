/*-------------------------------------------------------------------------
 *
 * win32security.c
 *	  Microsoft Windows Win32 Security Support Functions
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32security.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif


static BOOL pgwin32_get_dynamic_tokeninfo(HANDLE token,
							  TOKEN_INFORMATION_CLASS class,
							  char **InfoBuffer, char *errbuf, int errsize);


/*
 * Utility wrapper for frontend and backend when reporting an error
 * message.
 */
static
pg_attribute_printf(1, 2)
void
log_error(const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
#ifndef FRONTEND
	write_stderr(fmt, ap);
#else
	fprintf(stderr, fmt, ap);
#endif
	va_end(ap);
}

/*
 * Returns nonzero if the current user has administrative privileges,
 * or zero if not.
 *
 * Note: this cannot use ereport() because it's called too early during
 * startup.
 */
int
pgwin32_is_admin(void)
{
	HANDLE		AccessToken;
	char	   *InfoBuffer = NULL;
	char		errbuf[256];
	PTOKEN_GROUPS Groups;
	PSID		AdministratorsSid;
	PSID		PowerUsersSid;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	UINT		x;
	BOOL		success;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &AccessToken))
	{
		log_error(_("could not open process token: error code %lu\n"),
				  GetLastError());
		exit(1);
	}

	if (!pgwin32_get_dynamic_tokeninfo(AccessToken, TokenGroups,
									   &InfoBuffer, errbuf, sizeof(errbuf)))
	{
		log_error("%s", errbuf);
		exit(1);
	}

	Groups = (PTOKEN_GROUPS) InfoBuffer;

	CloseHandle(AccessToken);

	if (!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID,
								  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
								  0, &AdministratorsSid))
	{
		log_error(_("could not get SID for Administrators group: error code %lu\n"),
				  GetLastError());
		exit(1);
	}

	if (!AllocateAndInitializeSid(&NtAuthority, 2,
								  SECURITY_BUILTIN_DOMAIN_RID,
								  DOMAIN_ALIAS_RID_POWER_USERS, 0, 0, 0, 0, 0,
								  0, &PowerUsersSid))
	{
		log_error(_("could not get SID for PowerUsers group: error code %lu\n"),
				  GetLastError());
		exit(1);
	}

	success = FALSE;

	for (x = 0; x < Groups->GroupCount; x++)
	{
		if ((EqualSid(AdministratorsSid, Groups->Groups[x].Sid) &&
			 (Groups->Groups[x].Attributes & SE_GROUP_ENABLED)) ||
			(EqualSid(PowerUsersSid, Groups->Groups[x].Sid) &&
			 (Groups->Groups[x].Attributes & SE_GROUP_ENABLED)))
		{
			success = TRUE;
			break;
		}
	}

	free(InfoBuffer);
	FreeSid(AdministratorsSid);
	FreeSid(PowerUsersSid);
	return success;
}

/*
 * We consider ourselves running as a service if one of the following is
 * true:
 *
 * 1) We are running as Local System (only used by services)
 * 2) Our token contains SECURITY_SERVICE_RID (automatically added to the
 *	  process token by the SCM when starting a service)
 *
 * Return values:
 *	 0 = Not service
 *	 1 = Service
 *	-1 = Error
 *
 * Note: we can't report errors via either ereport (we're called too early
 * in the backend) or write_stderr (because that calls this).  We are
 * therefore reduced to writing directly on stderr, which sucks, but we
 * have few alternatives.
 */
int
pgwin32_is_service(void)
{
	static int	_is_service = -1;
	HANDLE		AccessToken;
	char	   *InfoBuffer = NULL;
	char		errbuf[256];
	PTOKEN_GROUPS Groups;
	PTOKEN_USER User;
	PSID		ServiceSid;
	PSID		LocalSystemSid;
	SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
	UINT		x;

	/* Only check the first time */
	if (_is_service != -1)
		return _is_service;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_READ, &AccessToken))
	{
		fprintf(stderr, "could not open process token: error code %lu\n",
				GetLastError());
		return -1;
	}

	/* First check for local system */
	if (!pgwin32_get_dynamic_tokeninfo(AccessToken, TokenUser, &InfoBuffer,
									   errbuf, sizeof(errbuf)))
	{
		fprintf(stderr, "%s", errbuf);
		return -1;
	}

	User = (PTOKEN_USER) InfoBuffer;

	if (!AllocateAndInitializeSid(&NtAuthority, 1,
							  SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
								  &LocalSystemSid))
	{
		fprintf(stderr, "could not get SID for local system account\n");
		CloseHandle(AccessToken);
		return -1;
	}

	if (EqualSid(LocalSystemSid, User->User.Sid))
	{
		FreeSid(LocalSystemSid);
		free(InfoBuffer);
		CloseHandle(AccessToken);
		_is_service = 1;
		return _is_service;
	}

	FreeSid(LocalSystemSid);
	free(InfoBuffer);

	/* Now check for group SID */
	if (!pgwin32_get_dynamic_tokeninfo(AccessToken, TokenGroups, &InfoBuffer,
									   errbuf, sizeof(errbuf)))
	{
		fprintf(stderr, "%s", errbuf);
		return -1;
	}

	Groups = (PTOKEN_GROUPS) InfoBuffer;

	if (!AllocateAndInitializeSid(&NtAuthority, 1,
								  SECURITY_SERVICE_RID, 0, 0, 0, 0, 0, 0, 0,
								  &ServiceSid))
	{
		fprintf(stderr, "could not get SID for service group\n");
		free(InfoBuffer);
		CloseHandle(AccessToken);
		return -1;
	}

	_is_service = 0;
	for (x = 0; x < Groups->GroupCount; x++)
	{
		if (EqualSid(ServiceSid, Groups->Groups[x].Sid))
		{
			_is_service = 1;
			break;
		}
	}

	free(InfoBuffer);
	FreeSid(ServiceSid);

	CloseHandle(AccessToken);

	return _is_service;
}


/*
 * Call GetTokenInformation() on a token and return a dynamically sized
 * buffer with the information in it. This buffer must be free():d by
 * the calling function!
 */
static BOOL
pgwin32_get_dynamic_tokeninfo(HANDLE token, TOKEN_INFORMATION_CLASS class,
							  char **InfoBuffer, char *errbuf, int errsize)
{
	DWORD		InfoBufferSize;

	if (GetTokenInformation(token, class, NULL, 0, &InfoBufferSize))
	{
		snprintf(errbuf, errsize,
			 "could not get token information buffer size: got zero size\n");
		return FALSE;
	}

	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
	{
		snprintf(errbuf, errsize,
			 "could not get token information buffer size: error code %lu\n",
				 GetLastError());
		return FALSE;
	}

	*InfoBuffer = malloc(InfoBufferSize);
	if (*InfoBuffer == NULL)
	{
		snprintf(errbuf, errsize,
				 "could not allocate %d bytes for token information\n",
				 (int) InfoBufferSize);
		return FALSE;
	}

	if (!GetTokenInformation(token, class, *InfoBuffer,
							 InfoBufferSize, &InfoBufferSize))
	{
		snprintf(errbuf, errsize,
				 "could not get token information: error code %lu\n",
				 GetLastError());
		return FALSE;
	}

	return TRUE;
}
