/* This is a hack that lets us use the -Wmissing-prototypes compile option.

   A bug or weakness in Linux's asm/bitops.h file makes it define a bunch
   of inline functions without first declaring a prototype.  This causes
   -Wmissing-prototypes to generate warnings.  These warnings are distracting
   and, in the case of -Werror, fatal.

   asm/bitops.h gets included by the Linux C library sem.h, which is included
   in several Postgres backend source files.

   Until Linux is fixed, we have our own version of asm/bitops.h that gets
   included first because it is in a directory mentioned in a -I option,
   whereas the Linux asm/bitops.h is in a standard include directory.  (This
   is GNU C preprocessor function).

   Our asm/bitops.h declares prototypes and then includes the Linux 
   asm/bitops.h.  If Linux changes these functions, our asm/bitops.h will
   stop compiling and will have to be updated.

   -Bryan 1996.11.17
*/

#ifndef POSTGRES_BITOPS_H
#define POSTGRES_BITOPS_H

#ifdef __SMP__
#define PG_BITOPS_VOLATILE volatile
#else
#define PG_BITOPS_VOLATILE 
#endif

extern __inline__ int set_bit(int nr, PG_BITOPS_VOLATILE void * addr);
extern __inline__ int clear_bit(int nr, PG_BITOPS_VOLATILE void * addr);
extern __inline__ int change_bit(int nr, PG_BITOPS_VOLATILE void * addr);
extern __inline__ int test_bit(int nr, const PG_BITOPS_VOLATILE void * addr);
extern __inline__ int find_first_zero_bit(void * addr, unsigned size);
extern __inline__ int find_next_zero_bit (void * addr, int size, int offset);
extern __inline__ unsigned long ffz(unsigned long word);

#include_next <asm/bitops.h>
#endif
