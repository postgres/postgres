/*-------------------------------------------------------------------------
 *
 * s_lock.c--
 *	  buffer manager interface routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/buffer/Attic/s_lock.c,v 1.3 1998/02/26 04:35:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * S_LOCK() -- Implements the S_LOCK function for the Linux/Alpha platform.
 *		   This function is usually an inlined macro for all other platforms,
 *		   but must be a seperate function for the Linux/Alpha platform, due
 *		   to the assembly code involved.
 */


#include <sys/types.h>
#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <signal.h>

#include "postgres.h"

/* declarations split between these three files */
#include "storage/buf.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"

#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/s_lock.h"

#if defined(__alpha__) && defined(linux)
void
S_LOCK(slock_t *lock)
{
	do
	{
		slock_t		_res;

		do
		{
	__asm__("    ldq   $0, %0              \n\
                   bne   $0, already_set     \n\
                   ldq_l $0, %0	             \n\
                   bne   $0, already_set     \n\
                   or    $31, 1, $0          \n\
                   stq_c $0, %0	             \n\
                   beq   $0, stqc_fail       \n\
          success: bis   $31, $31, %1        \n\
                   mb		             \n\
                   jmp   $31, end	     \n\
        stqc_fail: or    $31, 1, $0	     \n\
      already_set: bis   $0, $0, %1	     \n\
              end: nop      ": "=m"(*lock), "=r"(_res): :"0");
		} while (_res != 0);
	} while (0);
}

#endif
