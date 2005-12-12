#include "ts_locale.h"

#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"


#ifdef TS_USE_WIDE

#ifdef WIN32

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

#endif /* WIN32 */

int
_t_isalpha( char *ptr ) {
	wchar_t	character;

	char2wchar(&character, ptr, 1);

	return iswalpha( (wint_t)character );	
}

int
_t_isprint( char *ptr ) {
	wchar_t	character;

	char2wchar(&character, ptr, 1);

	return iswprint( (wint_t)character );	
}

#endif /* TS_USE_WIDE */

char *
lowerstr(char *str)
{
	char       *ptr = str;

#ifdef TS_USE_WIDE
	/*
	 * Use wide char code only when max encoding length > 1 and ctype != C.
	 * Some operating systems fail with multi-byte encodings and a C locale.
	 * Also, for a C locale there is no need to process as multibyte. From
	 * backend/utils/adt/oracle_compat.c Teodor
	 */
	if (pg_database_encoding_max_length() > 1 && !lc_ctype_is_c()) {
			wchar_t *wstr, *wptr;
			int len = strlen(str);

			wptr = wstr = (wchar_t *) palloc(sizeof(wchar_t) * (len+1));
			char2wchar(wstr, str, len+1);
			while (*wptr) {
				*wptr = towlower((wint_t) *wptr);
				wptr++;
			}
			wchar2char(str, wstr, len);
			pfree( wstr );
	} else
#endif
		while (*ptr)
		{
			*ptr = tolower(*(unsigned char *) ptr);
			ptr++;
		}
	return str;
}

