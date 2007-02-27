/* $PostgreSQL: pgsql/contrib/cube/cubedata.h,v 1.8 2007/02/27 23:48:05 tgl Exp $ */

#define CUBE_MAX_DIM (100)

typedef struct NDBOX
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	unsigned int dim;
	double		x[1];
}	NDBOX;
