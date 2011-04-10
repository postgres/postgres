#ifndef PL_PERL_HELPERS_H
#define PL_PERL_HELPERS_H

/*
 * convert from utf8 to database encoding
 */
static inline char *
utf_u2e(const char *utf8_str, size_t len)
{
	char	   *ret = (char *) pg_do_encoding_conversion((unsigned char *) utf8_str, len, PG_UTF8, GetDatabaseEncoding());

	if (ret == utf8_str)
		ret = pstrdup(ret);
	return ret;
}

/*
 * convert from database encoding to utf8
 */
static inline char *
utf_e2u(const char *str)
{
	char	   *ret = (char *) pg_do_encoding_conversion((unsigned char *) str, strlen(str), GetDatabaseEncoding(), PG_UTF8);

	if (ret == str)
		ret = pstrdup(ret);
	return ret;
}


/*
 * Convert an SV to a char * in the current database encoding
 */
static inline char *
sv2cstr(SV *sv)
{
	char	   *val;
	STRLEN		len;

	/*
	 * get a utf8 encoded char * out of perl. *note* it may not be valid utf8!
	 */
	val = SvPVutf8(sv, len);

	/*
	 * we use perls length in the event we had an embedded null byte to ensure
	 * we error out properly
	 */
	return utf_u2e(val, len);
}

/*
 * Create a new SV from a string assumed to be in the current database's
 * encoding.
 */

static inline SV *
cstr2sv(const char *str)
{
	SV		   *sv;
	char	   *utf8_str = utf_e2u(str);

	sv = newSVpv(utf8_str, 0);
	SvUTF8_on(sv);

	pfree(utf8_str);

	return sv;
}

#endif   /* PL_PERL_HELPERS_H */
