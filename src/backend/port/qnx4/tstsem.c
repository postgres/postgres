/*-------------------------------------------------------------------------
 *
 * tstsem.c
 *	  Test of System V Semaphore Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/tstsem.c,v 1.1 1999/12/16 16:52:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "postgres.h"
#include "storage/ipc.h"
#include <sys/mman.h>
#include <sys/sem.h>


#define SEMMAX  1
#define OPSMAX  1


int main( int argc, char **argv )
{
  int     c, errflg = 0;
  char    s[80];
  key_t   key = IPC_PRIVATE;
  int     nsems = SEMMAX;
  int     semflg = 0;
  int     unlink = 0;
  int     semid, i;
  struct sembuf sops[OPSMAX];
  u_short array[SEMMAX];
  union semun arg;

  optarg = NULL;
  while( !errflg && ( c = getopt( argc, argv, "k:n:cxu" ) ) != -1 )  {
    switch( c )  {
      case 'k': key  = atoi( optarg ); break;
      case 'n': nsems = atoi( optarg ); break;
      case 'c': semflg |= IPC_CREAT; break;
      case 'x': semflg |= IPC_EXCL; break;
      case 'u': unlink = 1; break;
      default: errflg++;
    }
  }
  if( errflg )  {
    printf( "usage: tstsem [-k key] [-n nsems] [-cxu]\n" );
    exit( 1 );
  }

  if( unlink )  {
    i = shm_unlink( "SysV_Sem_Info" );
    if( i == -1 ) perror( "shm_unlink" );
    exit( i );
  }

  semid = semget( key, nsems, semflg );
  if( semid == -1 )  {
    perror( "semget" );
    exit( semid );
  }

  do  {
    printf( "(-)sem_op, (+)sem_op, (G)ETVAL, (S)ETVAL, GET(P)ID, GET(A)LL, SETA(L)L, e(x)it: " );
    scanf( "%s", s );
    switch( s[0] )  {
      case '-': 
      case '+': 
        sops[0].sem_num = 0;
        sops[0].sem_op  = atoi( s );
        if( sops[0].sem_op == 0 ) sops[0].sem_op = s[0] == '+' ? +1 : -1;
        sops[0].sem_flg = 0;
        if( semop( semid, sops, 1 ) == -1 ) perror( "semop" );
        break;

      case 'G': 
      case 'g': 
        i = semctl( semid, 0, GETVAL, arg );
        if( i == -1 ) perror( "semctl" );
        else printf( "semval = %d\n", i );
        break;

      case 'S': 
      case 's': 
        printf( "semval = " );
        scanf( "%d", &arg.val );
        if( semctl( semid, 0, SETVAL, arg ) == -1 ) perror( "semctl" );
        break;

      case 'P': 
      case 'p': 
        i = semctl( semid, 0, GETPID, arg );
        if( i == -1 ) perror( "semctl" );
        else printf( "PID = %d\n", i );
        break;

      case 'A': 
      case 'a': 
        arg.array = array;
        i = semctl( semid, 0, GETALL, arg );
        if( i == -1 ) perror( "semctl" );
        else  {
          for( i = 0; i < nsems; i++ )  {
            printf( "semval[%d] = %hu\n", i, arg.array[i] );
          }
        }
        break;

      case 'L': 
      case 'l': 
        arg.array = array;
        for( i = 0; i < nsems; i++ )  {
          printf( "semval[%d] = ", i );
          scanf( "%hu", &arg.array[i] );
        }
        if( semctl( semid, 0, SETALL, arg ) == -1 )perror( "semctl" );
        break;
    }
  }
  while( s[0] != 'x' );

  if( semctl( semid, 0, IPC_RMID, arg ) == -1 )  {
    perror( "semctl" );
    exit( -1 );
  }

  exit( 0 );
}
