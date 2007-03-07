/* $PostgreSQL: pgsql/contrib/cube/cubedata.h,v 1.7.2.1 2007/03/07 21:25:18 teodor Exp $ */

#define CUBE_MAX_DIM (100)
typedef struct NDBOX
{
	unsigned int size;			/* required to be a Postgres varlena type */
	unsigned int dim;
	double		x[1];
}	NDBOX;

#define DatumGetNDBOX(x)	((NDBOX*)DatumGetPointer(x))
#define PG_GETARG_NDBOX(x)	DatumGetNDBOX( PG_DETOAST_DATUM(PG_GETARG_DATUM(x)) )
#define PG_RETURN_NDBOX(x)	PG_RETURN_POINTER(x)
