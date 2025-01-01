/*-------------------------------------------------------------------------
 *
 * unaccent.c
 *	  Text search unaccent dictionary
 *
 * Copyright (c) 2009-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/unaccent/unaccent.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_ts_dict.h"
#include "commands/defrem.h"
#include "lib/stringinfo.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

/*
 * An unaccent dictionary uses a trie to find a string to replace.  Each node
 * of the trie is an array of 256 TrieChar structs; the N-th element of the
 * array corresponds to next byte value N.  That element can contain both a
 * replacement string (to be used if the source string ends with this byte)
 * and a link to another trie node (to be followed if there are more bytes).
 *
 * Note that the trie search logic pays no attention to multibyte character
 * boundaries.  This is OK as long as both the data entered into the trie and
 * the data we're trying to look up are validly encoded; no partial-character
 * matches will occur.
 */
typedef struct TrieChar
{
	struct TrieChar *nextChar;
	char	   *replaceTo;
	int			replacelen;
} TrieChar;

/*
 * placeChar - put str into trie's structure, byte by byte.
 *
 * If node is NULL, we need to make a new node, which will be returned;
 * otherwise the return value is the same as node.
 */
static TrieChar *
placeChar(TrieChar *node, const unsigned char *str, int lenstr,
		  const char *replaceTo, int replacelen)
{
	TrieChar   *curnode;

	if (!node)
		node = (TrieChar *) palloc0(sizeof(TrieChar) * 256);

	Assert(lenstr > 0);			/* else str[0] doesn't exist */

	curnode = node + *str;

	if (lenstr <= 1)
	{
		if (curnode->replaceTo)
			ereport(WARNING,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("duplicate source strings, first one will be used")));
		else
		{
			curnode->replacelen = replacelen;
			curnode->replaceTo = (char *) palloc(replacelen);
			memcpy(curnode->replaceTo, replaceTo, replacelen);
		}
	}
	else
	{
		curnode->nextChar = placeChar(curnode->nextChar, str + 1, lenstr - 1,
									  replaceTo, replacelen);
	}

	return node;
}

/*
 * initTrie  - create trie from file.
 *
 * Function converts UTF8-encoded file into current encoding.
 */
static TrieChar *
initTrie(const char *filename)
{
	TrieChar   *volatile rootTrie = NULL;
	MemoryContext ccxt = CurrentMemoryContext;
	tsearch_readline_state trst;
	volatile bool skip;

	filename = get_tsearch_config_filename(filename, "rules");
	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open unaccent file \"%s\": %m",
						filename)));

	do
	{
		/*
		 * pg_do_encoding_conversion() (called by tsearch_readline()) will
		 * emit exception if it finds untranslatable characters in current
		 * locale. We just skip such lines, continuing with the next.
		 */
		skip = true;

		PG_TRY();
		{
			char	   *line;

			while ((line = tsearch_readline(&trst)) != NULL)
			{
				/*----------
				 * The format of each line must be "src" or "src trg", where
				 * src and trg are sequences of one or more non-whitespace
				 * characters, separated by whitespace.  Whitespace at start
				 * or end of line is ignored.  If trg is omitted, an empty
				 * string is used as the replacement.  trg can be optionally
				 * quoted, in which case whitespaces are included in it.
				 *
				 * We use a simple state machine, with states
				 *	0	initial (before src)
				 *	1	in src
				 *	2	in whitespace after src
				 *	3	in trg (non-quoted)
				 *	4	in trg (quoted)
				 *	5	in whitespace after trg
				 *	-1	syntax error detected (two strings)
				 *	-2	syntax error detected (unfinished quoted string)
				 *----------
				 */
				int			state;
				char	   *ptr;
				char	   *src = NULL;
				char	   *trg = NULL;
				char	   *trgstore = NULL;
				int			ptrlen;
				int			srclen = 0;
				int			trglen = 0;
				int			trgstorelen = 0;
				bool		trgquoted = false;

				state = 0;
				for (ptr = line; *ptr; ptr += ptrlen)
				{
					ptrlen = pg_mblen(ptr);
					/* ignore whitespace, but end src or trg */
					if (isspace((unsigned char) *ptr))
					{
						if (state == 1)
							state = 2;
						else if (state == 3)
							state = 5;
						/* whitespaces are OK in quoted area */
						if (state != 4)
							continue;
					}
					switch (state)
					{
						case 0:
							/* start of src */
							src = ptr;
							srclen = ptrlen;
							state = 1;
							break;
						case 1:
							/* continue src */
							srclen += ptrlen;
							break;
						case 2:
							/* start of trg */
							if (*ptr == '"')
							{
								trgquoted = true;
								state = 4;
							}
							else
								state = 3;

							trg = ptr;
							trglen = ptrlen;
							break;
						case 3:
							/* continue non-quoted trg */
							trglen += ptrlen;
							break;
						case 4:
							/* continue quoted trg */
							trglen += ptrlen;

							/*
							 * If this is a quote, consider it as the end of
							 * trg except if the follow-up character is itself
							 * a quote.
							 */
							if (*ptr == '"')
							{
								if (*(ptr + 1) == '"')
								{
									ptr++;
									trglen += 1;
								}
								else
									state = 5;
							}
							break;
						default:
							/* bogus line format */
							state = -1;
							break;
					}
				}

				if (state == 1 || state == 2)
				{
					/* trg was omitted, so use "" */
					trg = "";
					trglen = 0;
				}

				/* If still in a quoted area, fallback to an error */
				if (state == 4)
					state = -2;

				/* If trg was quoted, remove its quotes and unescape it */
				if (trgquoted && state > 0)
				{
					/* Ignore first and end quotes */
					trgstore = (char *) palloc(sizeof(char) * (trglen - 2));
					trgstorelen = 0;
					for (int i = 1; i < trglen - 1; i++)
					{
						trgstore[trgstorelen] = trg[i];
						trgstorelen++;
						/* skip second double quotes */
						if (trg[i] == '"' && trg[i + 1] == '"')
							i++;
					}
				}
				else
				{
					trgstore = (char *) palloc(sizeof(char) * trglen);
					trgstorelen = trglen;
					memcpy(trgstore, trg, trgstorelen);
				}

				if (state > 0)
					rootTrie = placeChar(rootTrie,
										 (unsigned char *) src, srclen,
										 trgstore, trgstorelen);
				else if (state == -1)
					ereport(WARNING,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid syntax: more than two strings in unaccent rule")));
				else if (state == -2)
					ereport(WARNING,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid syntax: unfinished quoted string in unaccent rule")));

				pfree(trgstore);
				pfree(line);
			}
			skip = false;
		}
		PG_CATCH();
		{
			ErrorData  *errdata;
			MemoryContext ecxt;

			ecxt = MemoryContextSwitchTo(ccxt);
			errdata = CopyErrorData();
			if (errdata->sqlerrcode == ERRCODE_UNTRANSLATABLE_CHARACTER)
			{
				FlushErrorState();
			}
			else
			{
				MemoryContextSwitchTo(ecxt);
				PG_RE_THROW();
			}
		}
		PG_END_TRY();
	}
	while (skip);

	tsearch_readline_end(&trst);

	return rootTrie;
}

/*
 * findReplaceTo - find longest possible match in trie
 *
 * On success, returns pointer to ending subnode, plus length of matched
 * source string in *p_matchlen.  On failure, returns NULL.
 */
static TrieChar *
findReplaceTo(TrieChar *node, const unsigned char *src, int srclen,
			  int *p_matchlen)
{
	TrieChar   *result = NULL;
	int			matchlen = 0;

	*p_matchlen = 0;			/* prevent uninitialized-variable warnings */

	while (node && matchlen < srclen)
	{
		node = node + src[matchlen];
		matchlen++;

		if (node->replaceTo)
		{
			result = node;
			*p_matchlen = matchlen;
		}

		node = node->nextChar;
	}

	return result;
}

PG_FUNCTION_INFO_V1(unaccent_init);
Datum
unaccent_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	TrieChar   *rootTrie = NULL;
	bool		fileloaded = false;
	ListCell   *l;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (strcmp(defel->defname, "rules") == 0)
		{
			if (fileloaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple Rules parameters")));
			rootTrie = initTrie(defGetString(defel));
			fileloaded = true;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized Unaccent parameter: \"%s\"",
							defel->defname)));
		}
	}

	if (!fileloaded)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing Rules parameter")));
	}

	PG_RETURN_POINTER(rootTrie);
}

PG_FUNCTION_INFO_V1(unaccent_lexize);
Datum
unaccent_lexize(PG_FUNCTION_ARGS)
{
	TrieChar   *rootTrie = (TrieChar *) PG_GETARG_POINTER(0);
	char	   *srcchar = (char *) PG_GETARG_POINTER(1);
	int32		len = PG_GETARG_INT32(2);
	char	   *srcstart = srcchar;
	TSLexeme   *res;
	StringInfoData buf;

	/* we allocate storage for the buffer only if needed */
	buf.data = NULL;

	while (len > 0)
	{
		TrieChar   *node;
		int			matchlen;

		node = findReplaceTo(rootTrie, (unsigned char *) srcchar, len,
							 &matchlen);
		if (node && node->replaceTo)
		{
			if (buf.data == NULL)
			{
				/* initialize buffer */
				initStringInfo(&buf);
				/* insert any data we already skipped over */
				if (srcchar != srcstart)
					appendBinaryStringInfo(&buf, srcstart, srcchar - srcstart);
			}
			appendBinaryStringInfo(&buf, node->replaceTo, node->replacelen);
		}
		else
		{
			matchlen = pg_mblen(srcchar);
			if (buf.data != NULL)
				appendBinaryStringInfo(&buf, srcchar, matchlen);
		}

		srcchar += matchlen;
		len -= matchlen;
	}

	/* return a result only if we made at least one substitution */
	if (buf.data != NULL)
	{
		res = (TSLexeme *) palloc0(sizeof(TSLexeme) * 2);
		res->lexeme = buf.data;
		res->flags = TSL_FILTER;
	}
	else
		res = NULL;

	PG_RETURN_POINTER(res);
}

/*
 * Function-like wrapper for dictionary
 */
PG_FUNCTION_INFO_V1(unaccent_dict);
Datum
unaccent_dict(PG_FUNCTION_ARGS)
{
	text	   *str;
	int			strArg;
	Oid			dictOid;
	TSDictionaryCacheEntry *dict;
	TSLexeme   *res;

	if (PG_NARGS() == 1)
	{
		/*
		 * Use the "unaccent" dictionary that is in the same schema that this
		 * function is in.
		 */
		Oid			procnspid = get_func_namespace(fcinfo->flinfo->fn_oid);
		const char *dictname = "unaccent";

		dictOid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
								  PointerGetDatum(dictname),
								  ObjectIdGetDatum(procnspid));
		if (!OidIsValid(dictOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("text search dictionary \"%s.%s\" does not exist",
							get_namespace_name(procnspid), dictname)));
		strArg = 0;
	}
	else
	{
		dictOid = PG_GETARG_OID(0);
		strArg = 1;
	}
	str = PG_GETARG_TEXT_PP(strArg);

	dict = lookup_ts_dictionary_cache(dictOid);

	res = (TSLexeme *) DatumGetPointer(FunctionCall4(&(dict->lexize),
													 PointerGetDatum(dict->dictData),
													 PointerGetDatum(VARDATA_ANY(str)),
													 Int32GetDatum(VARSIZE_ANY_EXHDR(str)),
													 PointerGetDatum(NULL)));

	PG_FREE_IF_COPY(str, strArg);

	if (res == NULL)
	{
		PG_RETURN_TEXT_P(PG_GETARG_TEXT_P_COPY(strArg));
	}
	else if (res->lexeme == NULL)
	{
		pfree(res);
		PG_RETURN_TEXT_P(PG_GETARG_TEXT_P_COPY(strArg));
	}
	else
	{
		text	   *txt = cstring_to_text(res->lexeme);

		pfree(res->lexeme);
		pfree(res);

		PG_RETURN_TEXT_P(txt);
	}
}
