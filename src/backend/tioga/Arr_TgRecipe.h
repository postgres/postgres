#include "tioga/Varray.h"

/* Modify the following size macros to suit your need. */

#ifndef Arr_TgString_INITIAL_SIZE
#define Arr_TgString_INITIAL_SIZE 32
#endif

#ifndef Arr_TgElementPtr_INITIAL_SIZE
#define Arr_TgElementPtr_INITIAL_SIZE 32
#endif

#ifndef Arr_TgNodePtr_INITIAL_SIZE
#define Arr_TgNodePtr_INITIAL_SIZE 32
#endif
/***************************************************************/
/*			  Do not modify anything below this line.		   */
/***************************************************************/

/* -- Defining types and function for Arr_TgString type -- */
/* -- the following must be supplied by the user:

   void copyTgString(TgString* from, TgString* to); - make a copy of TgString.
*/

#ifndef _ARR_TgString_
#define _ARR_TgString_

#ifndef ARR_TgString_INITIAL_SIZE
#define ARR_TgString_INITIAL_SIZE 32	/* change this size to suit your
										 * need */
#endif   /* ARR_TgString_INITIAL_SIZE */

typedef struct Arr_TgString
{
	size_t		num;
	size_t		size;
	size_t		valSize;
	TgString   *val;
}	Arr_TgString;

#define newArr_TgString() \
  (Arr_TgString *) NewVarray(ARR_TgString_INITIAL_SIZE, sizeof(TgString))

#define enlargeArr_TgString(A, I) \
( \
  (A)->size += (I), \
  (A)->val = (TgString *) realloc((A)->val, (A)->valSize * (A)->size) \
)

#define addArr_TgString(A, V) \
  AppendVarray((Varray *) (A), (void *) (V), (CopyingFunct) copyTgString)

#define deleteArr_TgString(A) FreeVarray(A)
#endif   /* _ARR_TgString_ */

/* -- Defining types and function for Arr_TgElementPtr type -- */
/* -- the following must be supplied by the user:

   void copyTgElementPtr(TgElementPtr* from, TgElementPtr* to); - make a copy of TgElementPtr.
*/

#ifndef _ARR_TgElementPtr_
#define _ARR_TgElementPtr_

#ifndef ARR_TgElementPtr_INITIAL_SIZE
#define ARR_TgElementPtr_INITIAL_SIZE 32		/* change this size to
												 * suit your need */
#endif   /* ARR_TgElementPtr_INITIAL_SIZE */

typedef struct Arr_TgElementPtr
{
	size_t		num;
	size_t		size;
	size_t		valSize;
	TgElementPtr *val;
}	Arr_TgElementPtr;

#define newArr_TgElementPtr() \
  (Arr_TgElementPtr *) NewVarray(ARR_TgElementPtr_INITIAL_SIZE, sizeof(TgElementPtr))

#define enlargeArr_TgElementPtr(A, I) \
( \
  (A)->size += (I), \
  (A)->val = (TgElementPtr *) realloc((A)->val, (A)->valSize * (A)->size) \
)

#define addArr_TgElementPtr(A, V) \
  AppendVarray((Varray *) (A), (void *) (V), (CopyingFunct) copyTgElementPtr)

#define deleteArr_TgElementPtr(A) FreeVarray(A)
#endif   /* _ARR_TgElementPtr_ */

/* -- Defining types and function for Arr_TgNodePtr type -- */
/* -- the following must be supplied by the user:

   void copyTgNodePtr(TgNodePtr* from, TgNodePtr* to); - make a copy of TgNodePtr.
*/

#ifndef _ARR_TgNodePtr_
#define _ARR_TgNodePtr_

#ifndef ARR_TgNodePtr_INITIAL_SIZE
#define ARR_TgNodePtr_INITIAL_SIZE 32	/* change this size to suit your
										 * need */
#endif   /* ARR_TgNodePtr_INITIAL_SIZE */

typedef struct Arr_TgNodePtr
{
	size_t		num;
	size_t		size;
	size_t		valSize;
	TgNodePtr  *val;
}	Arr_TgNodePtr;

#define newArr_TgNodePtr() \
  (Arr_TgNodePtr *) NewVarray(ARR_TgNodePtr_INITIAL_SIZE, sizeof(TgNodePtr))

#define enlargeArr_TgNodePtr(A, I) \
( \
  (A)->size += (I), \
  (A)->val = (TgNodePtr *) realloc((A)->val, (A)->valSize * (A)->size) \
)

#define addArr_TgNodePtr(A, V) \
  AppendVarray((Varray *) (A), (void *) (V), (CopyingFunct) copyTgNodePtr)

#define deleteArr_TgNodePtr(A) FreeVarray(A)

#endif   /* _ARR_TgNodePtr_ */
