#pragma once

static void
init_locale(const char *categoryname, int category, const char *locale)
{
	if (pg_perm_setlocale(category, locale) == NULL &&
		pg_perm_setlocale(category, "C") == NULL)
		elog(FATAL, "could not adopt \"%s\" locale nor C locale for %s",
			 locale, categoryname);
}


void
PostgresMain(const char *dbname, const char *username) {
    // unused
}


void
startup_hacks(const char *progname) {
#ifdef PG16
    SpinLockInit(&dummy_spinlock);
#endif
}


void pg_repl_raf() {
    puts("pg_repl_raf: STUB");
}



// embedded initdb requirements

void
get_restricted_token(void) {
    // stub
}

void *
pg_malloc(size_t size) {
	return malloc(size);
}

void *
pg_malloc_extended(size_t size, int flags) {
    return malloc(size);
}

void *
pg_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

char *
pg_strdup(const char *in) {
	char	   *tmp;

	if (!in)
	{
		fprintf(stderr,
				_("cannot duplicate null pointer (internal error)\n"));
		exit(EXIT_FAILURE);
	}
	tmp = strdup(in);
	if (!tmp)
	{
		fprintf(stderr, _("out of memory\n"));
		exit(EXIT_FAILURE);
	}
	return tmp;
}


char *
simple_prompt(const char *prompt, bool echo) {
    return pg_strdup("");
}


#ifndef PG16
int
ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done) {
    puts("# 89:"__FILE__" ProcessStartupPacket: STUB");
    return STATUS_OK;
}

const char *
select_default_timezone(const char *share_path) {
    fprintf(stderr, "# 95:" __FILE__ " select_default_timezone(%s): STUB\n", share_path);
	return getenv("TZ");
}

#include "../src/interfaces/libpq/pqexpbuffer.h"
#include "../src/fe_utils/option_utils.c"

bool
appendShellStringNoError(PQExpBuffer buf, const char *str)
{
	bool		ok = true;

	const char *p;

	if (*str != '\0' &&
		strspn(str, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./:") == strlen(str))
	{
		appendPQExpBufferStr(buf, str);
		return ok;
	}
	appendPQExpBufferChar(buf, '\'');
	for (p = str; *p; p++)
	{
		if (*p == '\n' || *p == '\r')
		{
			ok = false;
			continue;
		}

		if (*p == '\'')
			appendPQExpBufferStr(buf, "'\"'\"'");
		else
			appendPQExpBufferChar(buf, *p);
	}
	appendPQExpBufferChar(buf, '\'');
	return ok;
}

void
appendShellString(PQExpBuffer buf, const char *str)
{
	if (!appendShellStringNoError(buf, str))
	{
		fprintf(stderr,
				_("shell command argument contains a newline or carriage return: \"%s\"\n"),
				str);
		exit(EXIT_FAILURE);
	}
}
#endif
