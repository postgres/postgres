/*-------------------------------------------------------------------------
 *
 * sem.c
 *	  System V Semaphore Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/sem.c,v 1.1 1999/12/16 16:52:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <errno.h>
#include <semaphore.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "postgres.h"
#include "storage/ipc.h"
#include <sys/sem.h>


#define SETMAX    32
#define SEMMAX    16

#define MODE    0777
#define SHM_INFO_NAME   "SysV_Sem_Info"


struct sem_info {
  sem_t sem;
  struct {
    key_t  key;
    int    nsems;
    sem_t  sem[SEMMAX];  /* array of semaphores */
    pid_t  pid[SEMMAX];  /* array of PIDs */
  } set[SETMAX];
};

static struct sem_info  *SemInfo = ( struct sem_info * )-1;


int semctl( int semid, int semnum, int cmd, /*...*/union semun arg )
{
  int r;

  sem_wait( &SemInfo->sem );

  if( semid < 0 || semid >= SETMAX ||
      semnum < 0 || semnum >= SemInfo->set[semid].nsems )  {
    sem_post( &SemInfo->sem );
    errno = EINVAL;
    return -1;
  }

  switch( cmd )  {
    case GETPID:
      r = SemInfo->set[semid].pid[semnum];
      break;

    case GETVAL:
      r = SemInfo->set[semid].sem[semnum].value;
      break;

    case GETALL:
      for( semnum = 0; semnum < SemInfo->set[semid].nsems; semnum++ )  {
        arg.array[semnum] = SemInfo->set[semid].sem[semnum].value;
      }
      break;

    case SETVAL:
      SemInfo->set[semid].sem[semnum].value = arg.val;
      break;

    case SETALL:
      for( semnum = 0; semnum < SemInfo->set[semid].nsems; semnum++ )  {
        SemInfo->set[semid].sem[semnum].value = arg.array[semnum];
      }
      break;

    case IPC_RMID:
      for( semnum = 0; semnum < SemInfo->set[semid].nsems; semnum++ )  {
        if( sem_destroy( &SemInfo->set[semid].sem[semnum] ) == -1 )  {
          r = -1;
        }
      }
      SemInfo->set[semid].key   = -1;
      SemInfo->set[semid].nsems = 0;
      break;

    default:
      sem_post( &SemInfo->sem );
      errno = EINVAL;
      return -1;
  }

  sem_post( &SemInfo->sem );

  return r;
}

int semget( key_t key, int nsems, int semflg )
{
  int fd, semid, semnum/*, semnum1*/;
  int exist = 0;

  if( nsems < 0 || nsems > SEMMAX )  {
    errno = EINVAL;
    return -1;
  }

  /* open and map shared memory */
  if( SemInfo == ( struct sem_info * )-1 )  {
    /* test if the shared memory already exists */
    fd = shm_open( SHM_INFO_NAME, O_RDWR | O_CREAT | O_EXCL, MODE );
    if( fd == -1 && errno == EEXIST )  {
      exist = 1;
      fd = shm_open( SHM_INFO_NAME, O_RDWR | O_CREAT, MODE );
    }
    if( fd == -1 ) return fd;
    /* The size may only be set once. Ignore errors. */
    ltrunc( fd, sizeof( struct sem_info ), SEEK_SET );
    SemInfo = mmap( NULL, sizeof( struct sem_info ),
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if( SemInfo == MAP_FAILED ) return -1;
    if( !exist )  {
      /* create semaphore for locking */
      sem_init( &SemInfo->sem, 1, 1 );
      sem_wait( &SemInfo->sem );
      /* initilize shared memory */
      memset( SemInfo->set, 0, sizeof( SemInfo->set ) );
      for( semid = 0; semid < SETMAX; semid++ )  {
        SemInfo->set[semid].key = -1;
      }
      sem_post( &SemInfo->sem );
    }
  }

  sem_wait( &SemInfo->sem );

  if( key != IPC_PRIVATE )   {
    /* search existing element */
    semid = 0;
    while( semid < SETMAX && SemInfo->set[semid].key != key ) semid++;
    if( !( semflg & IPC_CREAT ) && semid >= SETMAX )  {
      sem_post( &SemInfo->sem );
      errno = ENOENT;
      return -1;
    }
    else if( semid < SETMAX )  {
      if( semflg & IPC_CREAT && semflg & IPC_EXCL )  {
        sem_post( &SemInfo->sem );
        errno = EEXIST;
        return -1;
      }
      else  {
        if( nsems != 0 && SemInfo->set[semid].nsems < nsems )  {
          sem_post( &SemInfo->sem );
          errno = EINVAL;
          return -1;
        }
        sem_post( &SemInfo->sem );
        return semid;
      }
    }
  }

  /* search first free element */
  semid = 0;
  while( semid < SETMAX && SemInfo->set[semid].key != -1 ) semid++;
  if( semid >= SETMAX )  {
    sem_post( &SemInfo->sem );
    errno = ENOSPC;
    return -1;
  }

  for( semnum = 0; semnum < nsems; semnum++ )  {
    sem_init( &SemInfo->set[semid].sem[semnum], 1, 0 );
/* Currently sem_init always returns -1.
    if( sem_init( &SemInfo->set[semid].sem[semnum], 1, 0 ) == -1 )  {
      for( semnum1 = 0; semnum1 < semnum; semnum1++ )  {
        sem_destroy( &SemInfo->set[semid].sem[semnum1] );
      }
      sem_post( &SemInfo->sem );
      return -1;
    }
*/
  }

  SemInfo->set[semid].key   = key;
  SemInfo->set[semid].nsems = nsems;

  sem_post( &SemInfo->sem );

  return 0;
}

int semop( int semid, struct sembuf *sops, size_t nsops )
{
  int i, j, r = 0, r1, errno1 = 0;

  sem_wait( &SemInfo->sem );

  if( semid < 0 || semid >= SETMAX )  {
    sem_post( &SemInfo->sem );
    errno = EINVAL;
    return -1;
  }
  for( i = 0; i < nsops; i++ )  {
    if( /*sops[i].sem_num < 0 ||*/ sops[i].sem_num >= SemInfo->set[semid].nsems )  {
      sem_post( &SemInfo->sem );
      errno = EFBIG;
      return -1;
    }
  }

  for( i = 0; i < nsops; i++ )  {
    if( sops[i].sem_op < 0 )  {
      if( sops[i].sem_flg & IPC_NOWAIT )  {
        for( j = 0; j < -sops[i].sem_op; j++ )  {
          if( sem_trywait( &SemInfo->set[semid].sem[sops[i].sem_num] ) )  {
            errno1 = errno;
            r = -1;
          }
        }
      }
      else  {
        for( j = 0; j < -sops[i].sem_op; j++ )  {
          sem_post( &SemInfo->sem ); /* avoid deadlock */
          r1 = sem_wait( &SemInfo->set[semid].sem[sops[i].sem_num] );
          sem_wait( &SemInfo->sem );
          if( r1 )  {
            errno1 = errno;
            r = r1;
          }
        }
      }
    }
    else if( sops[i].sem_op > 0 )  {
      for( j = 0; j < sops[i].sem_op; j++ )  {
        if( sem_post( &SemInfo->set[semid].sem[sops[i].sem_num] ) )  {
          errno1 = errno;
          r = -1;
        }
      }
    }
    else /* sops[i].sem_op == 0 */  {
      /* not supported */
    }
    SemInfo->set[semid].pid[sops[i].sem_num] = getpid( );
  }

  sem_post( &SemInfo->sem );

  errno = errno1;
  return r;
}
