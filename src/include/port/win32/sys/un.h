/*
 * src/include/port/win32/sys/un.h
 */
#ifndef WIN32_SYS_UN_H
#define WIN32_SYS_UN_H

/*
 * Windows defines this structure in <afunix.h>, but not all tool chains have
 * the header yet, so we define it here for now.
 */
struct sockaddr_un
{
	unsigned short sun_family;
	char		sun_path[108];
};

#endif
