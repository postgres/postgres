/* ********************************************************************
 *
 * Varray.h -- header file for varray.c which provides a generic
 *			   set of functions to handle variable sized arrays.
 *
 *			   originally by Jiang Wu
 * ********************************************************************/

#ifndef _VARRAY_H_
#define _VARRAY_H_

typedef struct _varray
{
	size_t		nobj;			/* number of objects in this array */
	size_t		maxObj;			/* max. number of objects in this array */
	size_t		size;			/* size of each element in the array */
	void	   *val;			/* array of elements */
}	Varray;

/* type for custom copying function */
typedef void (*CopyingFunct) (void *from, void *to);

#define VARRAY_INITIAL_SIZE 32

#define ENLARGE_VARRAY(ARRAY, INC) \
( \
  (ARRAY)->maxObj += (INC), \
  (ARRAY)->val = (void *) realloc((ARRAY)->val, \
								  (ARRAY)->size * (ARRAY)->maxObj) \
)

#define VARRAY_NTH(VAL, SIZE, N) (((char *) (VAL)) + (SIZE) * (N))

#define FreeVarray(ARRAY) \
  if ((ARRAY) != NULL) { free((ARRAY)->val); free((ARRAY)); (ARRAY) = NULL ; }

#define ModifyVarray(ARRAY, N, NEW, COPY) \
  if ((N) < (ARRAY)->nobj) \
	(COPY)(VARRAY_NTH((ARRAY)->val, (ARRAY)->size, (N)), (NEW))

#define GetVarray(ARRAY, N) \
  ((N) < (ARRAY)->nobj ? VARRAY_NTH((ARRAY)->val, (ARRAY)->size, (N)) \
					   : NULL)

extern Varray *NewVarray(size_t nobj, size_t size);
extern int	AppendVarray(Varray * array, void *value, CopyingFunct copy);

#endif   /* _VARRAY_H_ */
