#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifdef __cplusplus
extern "C" {
#endif

struct sqlca
{
	int			sqlcode;
	struct
	{
		int		sqlerrml;
		char		sqlerrmc[1000];
	}			sqlerrm;
	long			sqlerrd[6];
} sqlca;

#endif

#ifdef __cplusplus
}
#endif
 
