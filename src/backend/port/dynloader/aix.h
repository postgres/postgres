/*
 * $Id: aix.h,v 1.1 1997/12/19 22:58:26 scrappy Exp $
 *
 * @(#)dlfcn.h	1.4 revision of 95/04/25  09:36:52
 * This is an unpublished work copyright (c) 1992 HELIOS Software GmbH
 * 30159 Hannover, Germany
 */

#ifndef __dlfcn_h__
#define __dlfcn_h__

#ifdef __cplusplus
extern		"C"
{
#endif

/*
 * Mode flags for the dlopen routine.
 */
#define RTLD_LAZY		1		/* lazy function call binding */
#define RTLD_NOW		2		/* immediate function call binding */
#define RTLD_GLOBAL		0x100	/* allow symbols to be global */

/*
 * To be able to intialize, a library may provide a dl_info structure
 * that contains functions to be called to initialize and terminate.
 */
	struct dl_info
	{
		void		(*init) (void);
		void		(*fini) (void);
	};

#if __STDC__ || defined(_IBMR2)
	void	   *dlopen(const char *path, int mode);
	void	   *dlsym(void *handle, const char *symbol);
	char	   *dlerror(void);
	int			dlclose(void *handle);
#else
	void	   *dlopen();
	void	   *dlsym();
	char	   *dlerror();
	int			dlclose();
#endif

#ifdef __cplusplus
}

#endif

#define  pg_dlopen(f)	dlopen(filename, RTLD_LAZY)
#define  pg_dlsym(h,f)	dlsym(h, f)
#define  pg_dlclose(h)	dlclose(h)
#define  pg_dlerror()	dlerror()

#endif							/* __dlfcn_h__ */
