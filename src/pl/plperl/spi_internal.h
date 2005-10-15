#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"
#include "ppport.h"

int			spi_DEBUG(void);

int			spi_LOG(void);

int			spi_INFO(void);

int			spi_NOTICE(void);

int			spi_WARNING(void);

int			spi_ERROR(void);

/* this is actually in plperl.c */
HV		   *plperl_spi_exec(char *, int);
void		plperl_return_next(SV *);
SV		   *plperl_spi_query(char *);
SV		   *plperl_spi_fetchrow(char *);
