/*-------------------------------------------------------------------------
 * Logging framework for frontend programs
 *
 * Copyright (c) 2018-2020, PostgreSQL Global Development Group
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
	 * Not initialized yet
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
	 * Severe errors that cause program termination.  (One-shot programs may
	 * chose to label even fatal errors as merely "errors".  The distinction
	 * is up to the program.)
	 */
	PG_LOG_FATAL,

	/*
	 * Turn all logging off.
	 */
	PG_LOG_OFF,
};

extern enum pg_log_level __pg_log_level;

/*
 * Kind of a hack to be able to produce the psql output exactly as required by
 * the regression tests.
 */
#define PG_LOG_FLAG_TERSE	1

void		pg_logging_init(const char *argv0);
void		pg_logging_config(int new_flags);
void		pg_logging_set_level(enum pg_log_level new_level);
void		pg_logging_set_pre_callback(void (*cb) (void));
void		pg_logging_set_locus_callback(void (*cb) (const char **filename, uint64 *lineno));

void		pg_log_generic(enum pg_log_level level, const char *pg_restrict fmt,...) pg_attribute_printf(2, 3);
void		pg_log_generic_v(enum pg_log_level level, const char *pg_restrict fmt, va_list ap) pg_attribute_printf(2, 0);

#define pg_log_fatal(...) do { \
		if (likely(__pg_log_level <= PG_LOG_FATAL)) pg_log_generic(PG_LOG_FATAL, __VA_ARGS__); \
	} while(0)

#define pg_log_error(...) do { \
		if (likely(__pg_log_level <= PG_LOG_ERROR)) pg_log_generic(PG_LOG_ERROR, __VA_ARGS__); \
	} while(0)

#define pg_log_warning(...) do { \
		if (likely(__pg_log_level <= PG_LOG_WARNING)) pg_log_generic(PG_LOG_WARNING, __VA_ARGS__); \
	} while(0)

#define pg_log_info(...) do { \
		if (likely(__pg_log_level <= PG_LOG_INFO)) pg_log_generic(PG_LOG_INFO, __VA_ARGS__); \
	} while(0)

#define pg_log_debug(...) do { \
		if (unlikely(__pg_log_level <= PG_LOG_DEBUG)) pg_log_generic(PG_LOG_DEBUG, __VA_ARGS__); \
	} while(0)

#endif							/* COMMON_LOGGING_H */
