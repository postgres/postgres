#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

int			spi_DEBUG(void);

int			spi_LOG(void);

int			spi_INFO(void);

int			spi_NOTICE(void);

int			spi_WARNING(void);

int			spi_ERROR(void);

HV*		plperl_spi_exec(char*, int);


