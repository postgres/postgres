#include "postgres.h"

#include "access/gist.h"
#include "access/itup.h"
#include "access/nbtree.h"
#include "utils/geo_decls.h"

typedef int (*CMPFUNC) (const void *a, const void *b);
typedef void (*BINARY_UNION) (Datum *, char *);


/* used for sorting */

typedef struct rix
{
	int			index;
	char	   *r;
}	RIX;

/*
** Common btree-function (for all ops)
*/

extern GIST_SPLITVEC *btree_picksplit(bytea *entryvec, GIST_SPLITVEC *v,
				BINARY_UNION bu, CMPFUNC cmp);
