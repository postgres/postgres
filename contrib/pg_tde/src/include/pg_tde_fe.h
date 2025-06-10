/*
 * TDE redefinitions for frontend included code
 */

#ifndef PG_TDE_EREPORT_H
#define PG_TDE_EREPORT_H

#ifdef FRONTEND

#include "postgres_fe.h"
#include "common/logging.h"
#include "common/file_perm.h"
#include "utils/elog.h"

#pragma GCC diagnostic ignored "-Wunused-macros"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wextra"

/*
 * Errors handling
 * ----------------------------------------
 */

#define tde_fe_errlog(_type, ...) \
	({		\
		if (tde_fe_error_level >= ERROR)		\
			pg_log_error##_type(__VA_ARGS__);		\
		else if (tde_fe_error_level >= WARNING)		\
			pg_log_warning##_type(__VA_ARGS__);		\
		else if (tde_fe_error_level >= LOG)		\
			pg_log_info##_type(__VA_ARGS__);		\
		else		\
			pg_log_debug##_type(__VA_ARGS__);		\
	})

#define errmsg(...) tde_fe_errlog(, __VA_ARGS__)
#define errhint(...) tde_fe_errlog(_hint, __VA_ARGS__)
#define errdetail(...) tde_fe_errlog(_detail, __VA_ARGS__)

#define errcode_for_file_access() NULL
#define errcode(e) NULL

#define tde_error_handle_exit(elevel) \
	do {							\
		if (elevel >= PANIC)		\
			pg_unreachable();		\
		else if (elevel >= ERROR)	\
			exit(1);				\
	} while(0)

#undef elog
#define elog(elevel, fmt, ...) \
	do {							\
		tde_fe_error_level = elevel;	\
		errmsg(fmt, ##__VA_ARGS__);		\
		tde_error_handle_exit(elevel);	\
	} while(0)

#undef ereport
#define ereport(elevel,...)		\
	do {							\
		tde_fe_error_level = elevel;	\
		__VA_ARGS__;					\
		tde_error_handle_exit(elevel);	\
	} while(0)

static int	tde_fe_error_level = 0;

/*
 * -------------
 */

#define LWLockAcquire(lock, mode) NULL
#define LWLockRelease(lock_files) NULL
#define LWLockHeldByMeInMode(lock, mode) true
#define LWLock void
#define LWLockMode void*
#define LW_SHARED NULL
#define LW_EXCLUSIVE NULL
#define tde_lwlock_enc_keys() NULL

#define OpenTransientFile(fileName, fileFlags) open(fileName, fileFlags, PG_FILE_MODE_OWNER)
#define CloseTransientFile(fd) close(fd)
#define AllocateFile(name, mode) fopen(name, mode)
#define FreeFile(file) fclose(file)

#define pg_fsync(fd) fsync(fd)
#endif							/* FRONTEND */

#endif							/* PG_TDE_EREPORT_H */
