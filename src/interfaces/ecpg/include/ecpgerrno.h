#ifndef _ECPG_ERROR_H
#define _ECPG_ERROR_H

/* This is a list of all error codes the embedded SQL program can return */
#define	ECPG_NO_ERROR		0
#define ECPG_NOT_FOUND		100

#define ECPG_PGSQL		-1
#define ECPG_UNSUPPORTED	-2
#define ECPG_TOO_MANY_ARGUMENTS	-3
#define ECPG_TOO_FEW_ARGUMENTS	-4
#define ECPG_TRANS		-5
#define ECPG_TOO_MANY_MATCHES	-6
#define ECPG_INT_FORMAT		-7
#define ECPG_UINT_FORMAT	-8
#define ECPG_FLOAT_FORMAT	-9
#define ECPG_CONVERT_BOOL	-10
#define ECPG_EMPTY		-11
#define ECPG_CONNECT		-12
#define ECPG_DISCONNECT		-13

#endif /* !_ECPG_ERROR_H */
