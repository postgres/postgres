/*-------------------------------------------------------------------------
 *
 * tstshm.c
 *	  Test of System V Shared Memory Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/tstshm.c,v 1.4 2002/11/08 20:23:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/shm.h>


int
main(int argc, char **argv)
{
	int			c,
				errflg = 0;
	char		s[80];
	key_t		key = 0x1000;
	size_t		size = 256;
	int			shmid = -1;
	caddr_t		addr = NULL;

	optarg = NULL;
	while (!errflg && (c = getopt(argc, argv, "k:s:")) != -1)
	{
		switch (c)
		{
			case 'k':
				key = atoi(optarg);
				break;
			case 's':
				size = atoi(optarg);
				break;
			default:
				errflg++;
		}
	}
	if (errflg)
	{
		printf("usage: tstshm [-k key] [-s size]\n");
		exit(1);
	}

	do
	{
		printf("shm(g)et, shm(a)t, shm(d)t, shm(c)tl, (w)rite, (r)ead, e(x)it: ");
		scanf("%s", s);
		switch (s[0])
		{
			case 'g':
				shmid = shmget(key, size, IPC_CREAT | SHM_R | SHM_W);
				if (shmid == -1)
					perror("shmget");
				break;

			case 'a':
				addr = shmat(shmid, NULL, 0);
				if (addr == (void *) -1)
					perror("shmat");
				break;

			case 'd':
				if (shmdt(addr) == -1)
					perror("shmdt");
				else
					addr = NULL;
				break;

			case 'c':
				if (shmctl(shmid, IPC_RMID, NULL) == -1)
					perror("shmctl");
				else
					shmid = -1;
				break;

			case 'w':
				printf("String to write: ");
				scanf("%s", addr);
				break;

			case 'r':
				puts(addr);
				break;
		}
	}
	while (s[0] != 'x');

	return 0;
}
