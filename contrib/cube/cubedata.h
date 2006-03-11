/* $PostgreSQL: pgsql/contrib/cube/cubedata.h,v 1.7 2006/03/11 04:38:28 momjian Exp $ */

#define CUBE_MAX_DIM (100)
typedef struct NDBOX
{
	unsigned int size;			/* required to be a Postgres varlena type */
	unsigned int dim;
	double		x[1];
}	NDBOX;
