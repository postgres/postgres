/*-------------------------------------------------------------------------
 *
 * s_lock.c--
 *     This file contains the implementation (if any) for spinlocks.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/storage/ipc/Attic/s_lock.c,v 1.7 1996/11/06 06:49:04 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *   DESCRIPTION
 *	The following code fragment should be written (in assembly 
 *	language) on machines that have a native test-and-set instruction:
 *
 *	void
 *	S_LOCK(char_address)
 *	    char *char_address;
 *	{
 *	    while (test_and_set(char_address))
 *		;
 *	}
 *
 *	If this is not done, POSTGRES will default to using System V
 *	semaphores (and take a large performance hit -- around 40% of
 *	its time on a DS5000/240 is spent in semop(3)...).
 *
 *   NOTES
 *	AIX has a test-and-set but the recommended interface is the cs(3)
 *	system call.  This provides an 8-instruction (plus system call 
 *	overhead) uninterruptible compare-and-set operation.  True 
 *	spinlocks might be faster but using cs(3) still speeds up the 
 *	regression test suite by about 25%.  I don't have an assembler
 *	manual for POWER in any case.
 *
 */
#ifdef WIN32
#include <windows.h>
#endif /* WIN32 */

#include "postgres.h"

#include "storage/ipc.h"


#if defined(HAS_TEST_AND_SET)

#if defined (next)
/*
 * NEXTSTEP (mach)
 * slock_t is defined as a struct mutex.
 */
void
S_LOCK(slock_t *lock)
{
	mutex_lock(lock);
}
void
S_UNLOCK(slock_t *lock)
{
	mutex_unlock(lock);
}
void
S_INIT_LOCK(slock_t *lock)
{
 	mutex_init(lock);	
}

 /* S_LOCK_FREE should return 1 if lock is free; 0 if lock is locked */
int
 S_LOCK_FREE(slock_t *lock)
{
/* For Mach, we have to delve inside the entrails of `struct mutex'.  Ick! */
 	return (lock->lock == 0);
}

#endif /* next */



#if defined(irix5)
/*
 * SGI IRIX 5
 * slock_t is defined as a struct abilock_t, which has a single unsigned long 
 * member.
 * 
 * This stuff may be supplemented in the future with Masato Kataoka's MIPS-II
 * assembly from his NECEWS SVR4 port, but we probably ought to retain this
 * for the R3000 chips out there.
 */
void
S_LOCK(slock_t *lock)
{
	/* spin_lock(lock); */
	while (!acquire_lock(lock))
	    ;
}

void
S_UNLOCK(slock_t *lock)
{
	(void)release_lock(lock);
}

void
S_INIT_LOCK(slock_t *lock)
{
	(void)init_lock(lock);	
}

/* S_LOCK_FREE should return 1 if lock is free; 0 if lock is locked */
int
S_LOCK_FREE(slock_t *lock)
{
	return(stat_lock(lock)==UNLOCKED); 
}

#endif /* irix5 */


/*
 * OSF/1 (Alpha AXP)
 *
 * Note that slock_t on the Alpha AXP is msemaphore instead of char
 * (see storage/ipc.h).
 */

#if defined(alpha)

void
S_LOCK(slock_t *lock)
{
    while (msem_lock(lock, MSEM_IF_NOWAIT) < 0)
	;
}

void
S_UNLOCK(slock_t *lock)
{
    (void) msem_unlock(lock, 0);
}

void
S_INIT_LOCK(slock_t *lock)
{
    (void) msem_init(lock, MSEM_UNLOCKED);
}

int
S_LOCK_FREE(slock_t *lock)
{
    return(lock->msem_state ? 0 : 1);
}

#endif /* alpha */

/*
 * Solaris 2
 */

#if defined(i386_solaris) || \
    defined(sparc_solaris)

/* for xxxxx_solaris, this is defined in port/.../tas.s */
extern int tas(slock_t *lock);

void
S_LOCK(slock_t *lock)
{
    while (tas(lock))
	;
}

void
S_UNLOCK(slock_t *lock)
{
    *lock = 0;
}

void
S_INIT_LOCK(slock_t *lock)
{
    S_UNLOCK(lock);
}

#endif /* i86pc_solaris || sparc_solaris */

/*
 * AIX (POWER)
 *
 * Note that slock_t on POWER/POWER2/PowerPC is int instead of char
 * (see storage/ipc.h).
 */

#if defined(aix)

void
S_LOCK(slock_t *lock)
{
    while (cs((int *) lock, 0, 1))
	;
}

void
S_UNLOCK(slock_t *lock)
{
    *lock = 0;
}

void
S_INIT_LOCK(slock_t *lock)
{
    S_UNLOCK(lock);
}

#endif /* aix */

/*
 * HP-UX (PA-RISC)
 *
 * Note that slock_t on PA-RISC is a structure instead of char
 * (see storage/ipc.h).
 */

#if defined(hpux)

/* defined in port/.../tas.s */
extern int tas(slock_t *lock);

/*
* a "set" slock_t has a single word cleared.  a "clear" slock_t has 
* all words set to non-zero.
*/
static slock_t clear_lock = { -1, -1, -1, -1 };

void
S_LOCK(slock_t *lock)
{
    while (tas(lock))
	;
}

void
S_UNLOCK(slock_t *lock)
{
    *lock = clear_lock;	/* struct assignment */
}

void
S_INIT_LOCK(slock_t *lock)
{
    S_UNLOCK(lock);
}

int
S_LOCK_FREE(slock_t *lock)
{
    register int *lock_word = (int *) (((long) lock + 15) & ~15);

    return(*lock_word != 0);
}

#endif /* hpux */

/*
 * sun3
 */
 
#if (defined(sun3) && ! defined(sparc))

void    
S_LOCK(slock_t *lock)
{
    while (tas(lock));
}

void
S_UNLOCK(slock_t *lock)
{
    *lock = 0;
}

void
S_INIT_LOCK(slock_t *lock)
{
    S_UNLOCK(lock);
}

static int
tas_dummy()
{
    asm("LLA0:");
    asm("	.data");
    asm("	.text");
    asm("|#PROC# 04");
    asm("	.globl	_tas");
    asm("_tas:");
    asm("|#PROLOGUE# 1");
    asm("	movel   sp@(0x4),a0");
    asm("	tas	a0@");
    asm("	beq	LLA1");
    asm("	moveq   #-128,d0");
    asm("	rts");
    asm("LLA1:");
    asm("	moveq   #0,d0");
    asm("	rts");
    asm("	.data");
}

#endif

/*
 * SPARC (SunOS 4)
 */

#if defined(sparc) && !defined(sparc_solaris)

/* if we're using -ansi w/ gcc, use __asm__ instead of asm */
#if defined(__STRICT_ANSI__)
#define asm(x)  __asm__(x)
#endif 

static int
tas_dummy()
{
    asm(".seg \"data\"");
    asm(".seg \"text\"");
    asm(".global _tas");
    asm("_tas:");
    
    /*
     * Sparc atomic test and set (sparc calls it "atomic load-store")
     */
    
    asm("ldstub [%r8], %r8");
    
    /*
     * Did test and set actually do the set?
     */
    
    asm("tst %r8");
    
    asm("be,a ReturnZero");
    
    /*
     * otherwise, just return.
     */
    
    asm("clr %r8");
    asm("mov 0x1, %r8");
    asm("ReturnZero:");
    asm("retl");
    asm("nop");
}

void
S_LOCK(unsigned char *addr)
{
    while (tas(addr));
}


/*
 * addr should be as in the above S_LOCK routine
 */
void
S_UNLOCK(unsigned char *addr)
{
    *addr = 0;
}

void
S_INIT_LOCK(unsigned char *addr)
{
    *addr = 0;
}

#endif /* sparc */

/*
 * Linux and friends
 */

#if defined(BSD44_derived) || \
    defined(bsdi) || \
    defined(bsdi_2_1) || \
    defined(linux)

int
tas(slock_t *m)
{
    slock_t res;
    __asm__("xchgb %0,%1":"=q" (res),"=m" (*m):"0" (0x1));
    return(res);
}

void
S_LOCK(slock_t *lock)
{
    while (tas(lock))
	;
}

void
S_UNLOCK(slock_t *lock)
{
    *lock = 0;
}

void
S_INIT_LOCK(slock_t *lock)
{
    S_UNLOCK(lock);
}

#endif /* linux and friends */


#endif /* HAS_TEST_AND_SET */


#ifdef WIN32
void
S_LOCK(HANDLE *lock)
{
      int x = 0;
      x = x / x;
}

void
S_UNLOCK(HANDLE *lock)
{
      int x = 0;
      x = x / x;
}

void
S_INIT_LOCK(HANDLE *lock)
{
      int x = 0;
      x = x / x;
}
#endif /*WIN32*/
