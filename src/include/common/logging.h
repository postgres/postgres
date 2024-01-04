/*-------------------------------------------------------------------------
 * Logging framework for frontend programs
 *
 * Copyright (c) 2018-2024, PostgreSQL Global Development Group
 *
 * src/include/common/logging.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_LOGGING_H
#define COMMON_LOGGING_H

/*
 * Log levels are informational only.  They do not affect program flow.
 */
enum pg_log_level
{
	/*
	 * Not initialized yet (not to be used as an actual message log level).
	 */
	PG_LOG_NOTSET = 0,

	/*
	 * Low level messages that are normally off by default.
	 */
	PG_LOG_DEBUG,

	/*
	 * Any program messages that go to stderr, shown by default.  (The
	 * program's normal output should go to stdout and not use the logging
	 * system.)
	 */
	PG_LOG_INFO,

	/*
	 * Warnings and "almost" errors, depends on the program
	 */
	PG_LOG_WARNING,

	/*
	 * Errors
	 */
	PG_LOG_ERROR,

	/*
	 * Turn all logging off (not to be used as an actual message log level).
	 */
	PG_LOG_OFF,
};

/*
 * __pg_log_level is the minimum log level that will actually be shown.
 */
extern enum pg_log_level __pg_log_level;

/*
 * A log message can have several parts.  The primary message is required,
 * others are optional.  When emitting multiple parts, do so in the order of
 * this enum, for consistency.
 */
enum pg_log_part
{
	/*
	 * The primary message.  Try to keep it to one line; follow the backend's
	 * style guideline for primary messages.
	 */
	PG_LOG_PRIMARY,

	/*
	 * Additional detail.  Follow the backend's style guideline for detail
	 * messages.
	 */
	PG_LOG_DETAIL,

	/*
	 * Hint (not guaranteed correct) about how to fix the problem.  Follow the
	 * backend's style guideline for hint messages.
	 */
	PG_LOG_HINT,
};

/*
 * Kind of a hack to be able to produce the psql output exactly as required by
 * the regression tests.
 */
#define PG_LOG_FLAG_TERSE	1

void		pg_logging_init(const char *argv0);
void		pg_logging_config(int new_flags);
void		pg_logging_set_level(enum pg_log_level new_level);
void		pg_logging_increase_verbosity(void);
void		pg_logging_set_pre_callback(void (*cb) (void));
void		pg_logging_set_locus_callback(void (*cb) (const char **filename, uint64 *lineno));

void		pg_log_generic(enum pg_log_level level, enum pg_log_part part,
						   const char *pg_restrict fmt,...)
			pg_attribute_printf(3, 4);
void		pg_log_generic_v(enum pg_log_level level, enum pg_log_part part,
							 const char *pg_restrict fmt, va_list ap)
			pg_attribute_printf(3, 0);

/*
 * Preferred style is to use these macros to perform logging; don't call
 * pg_log_generic[_v] directly, except perhaps in error interface code.
 */
#define pg_log_error(...) \
	pg_log_generic(PG_LOG_ERROR, PG_LOG_PRIMARY, __VA_ARGS__)

#define pg_log_error_detail(...) \
	pg_log_generic(PG_LOG_ERROR, PG_LOG_DETAIL, __VA_ARGS__)

#define pg_log_error_hint(...) \
	pg_log_generic(PG_LOG_ERROR, PG_LOG_HINT, __VA_ARGS__)

#define pg_log_warning(...) \
	pg_log_generic(PG_LOG_WARNING, PG_LOG_PRIMARY, __VA_ARGS__)

#define pg_log_warning_detail(...) \
	pg_log_generic(PG_LOG_WARNING, PG_LOG_DETAIL, __VA_ARGS__)

#define pg_log_warning_hint(...) \
	pg_log_generic(PG_LOG_WARNING, PG_LOG_HINT, __VA_ARGS__)

#define pg_log_info(...) \
	pg_log_generic(PG_LOG_INFO, PG_LOG_PRIMARY, __VA_ARGS__)

#define pg_log_info_detail(...) \
	pg_log_generic(PG_LOG_INFO, PG_LOG_DETAIL, __VA_ARGS__)

#define pg_log_info_hint(...) \
	pg_log_generic(PG_LOG_INFO, PG_LOG_HINT, __VA_ARGS__)

#define pg_log_debug(...) do { \
		if (unlikely(__pg_log_level <= PG_LOG_DEBUG)) \
			pg_log_generic(PG_LOG_DEBUG, PG_LOG_PRIMARY, __VA_ARGS__); \
	} while(0)

#define pg_log_debug_detail(...) do { \
		if (unlikely(__pg_log_level <= PG_LOG_DEBUG)) \
			pg_log_generic(PG_LOG_DEBUG, PG_LOG_DETAIL, __VA_ARGS__); \
	} while(0)

#define pg_log_debug_hint(...) do { \
		if (unlikely(__pg_log_level <= PG_LOG_DEBUG)) \
			pg_log_generic(PG_LOG_DEBUG, PG_LOG_HINT, __VA_ARGS__); \
	} while(0)

/*
 * A common shortcut: pg_log_error() and immediately exit(1).
 */
#define pg_fatal(...) do { \
		pg_log_generic(PG_LOG_ERROR, PG_LOG_PRIMARY, __VA_ARGS__); \
		exit(1); \
	} while(0)

#endif							/* COMMON_LOGGING_H */
