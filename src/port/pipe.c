/*-------------------------------------------------------------------------
 *
 * pipe.c
 *	  pipe()
 *
 * Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 *	This is a replacement version of pipe for Win32 which allows
 *	returned handles to be used in select(). Note that read/write calls
 *	must be replaced with recv/send.
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/pipe.c,v 1.3 2004/05/11 21:57:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/wait.h>

#define _(x) gettext((x))

#ifdef WIN32
int
pgpipe(int handles[2])
{
	SOCKET		s;
	struct sockaddr_in serv_addr;
	int			len = sizeof(serv_addr);

	handles[0] = handles[1] = INVALID_SOCKET;

	if ((s = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
		return -1;

	memset((void *) &serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(0);
	serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (SOCKADDR *) & serv_addr, len) == SOCKET_ERROR ||
		listen(s, 1) == SOCKET_ERROR ||
		getsockname(s, (SOCKADDR *) & serv_addr, &len) == SOCKET_ERROR ||
		(handles[1] = socket(PF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
	{
		closesocket(s);
		return -1;
	}

	if (connect(handles[1], (SOCKADDR *) & serv_addr, len) == SOCKET_ERROR ||
		(handles[0] = accept(s, (SOCKADDR *) & serv_addr, &len)) == INVALID_SOCKET)
	{
		closesocket(handles[1]);
		handles[1] = INVALID_SOCKET;
		closesocket(s);
		return -1;
	}
	closesocket(s);
	return 0;
}


int piperead(int s, char* buf, int len)
{
	int ret = recv(s,buf,len,0);
	if (ret < 0 && WSAGetLastError() == WSAECONNRESET)
		/* EOF on the pipe! (win32 socket based implementation) */
		ret = 0;
	return ret;
}
#endif

/*
 * pclose() plus useful error reporting
 * Is this necessary?  bjm 2004-05-11
 */
int
pclose_check(FILE *stream)
{
	int		exitstatus;

	exitstatus = pclose(stream);

	if (exitstatus == 0)
		return 0;					/* all is well */

	if (exitstatus == -1)
	{
		/* pclose() itself failed, and hopefully set errno */
		perror("pclose failed");
	}
	else if (WIFEXITED(exitstatus))
	{
		fprintf(stderr, _("child process exited with exit code %d\n"),
				WEXITSTATUS(exitstatus));
	}
	else if (WIFSIGNALED(exitstatus))
	{
		fprintf(stderr, _("child process was terminated by signal %d\n"),
				WTERMSIG(exitstatus));
	}
	else
	{
		fprintf(stderr, _("child process exited with unrecognized status %d\n"),
				exitstatus);
	}

	return -1;
}
