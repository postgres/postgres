#include "ts_locale.h"

#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"


#if defined(TS_USE_WIDE) && defined(WIN32)

size_t
wchar2char(char *to, const wchar_t *from, size_t len)
{
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		int			r,
					nbytes;

		if (len == 0)
			return 0;

		/* in any case, *to should be allocated with enough space */
		nbytes = WideCharToMultiByte(CP_UTF8, 0, from, len, NULL, 0, NULL, NULL);
		if (nbytes == 0)
			ereport(ERROR,
					(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("UTF-16 to UTF-8 translation failed: %lu",
							GetLastError())));

		r = WideCharToMultiByte(CP_UTF8, 0, from, len, to, nbytes,
								NULL, NULL);

		if (r == 0)
			ereport(ERROR,
					(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("UTF-16 to UTF-8 translation failed: %lu",
							GetLastError())));
		return r;
	}

	return wcstombs(to, from, len);
}

size_t
char2wchar(wchar_t *to, const char *from, size_t len)
{
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		int			r;

		if (len == 0)
			return 0;

		r = MultiByteToWideChar(CP_UTF8, 0, from, len, to, len);

		if (!r)
		{
			pg_verifymbstr(from, len, false);
			ereport(ERROR,
					(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("invalid multibyte character for locale"),
					 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
		}

		Assert(r <= len);

		return r;
	}

	return mbstowcs(to, from, len);
}

#endif
