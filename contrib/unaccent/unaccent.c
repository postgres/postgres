/*-------------------------------------------------------------------------
 *
 * unaccent.c
 *	  Text search unaccent dictionary
 *
 * Copyright (c) 2009-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/unaccent/unaccent.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/*
 * Unaccent dictionary uses a trie to find a character to replace. Each node of
 * the trie is an array of 256 TrieChar structs (n-th element of array
 * corresponds to byte)
 */
typedef struct TrieChar
{
	struct TrieChar *nextChar;
	char	   *replaceTo;
	int			replacelen;
} TrieChar;

/*
 * placeChar - put str into trie's structure, byte by byte.
 */
static TrieChar *
placeChar(TrieChar *node, unsigned char *str, int lenstr, char *replaceTo, int replacelen)
{
	TrieChar   *curnode;

	if (!node)
	{
		node = palloc(sizeof(TrieChar) * 256);
		memset(node, 0, sizeof(TrieChar) * 256);
	}

	curnode = node + *str;

	if (lenstr == 1)
	{
		if (curnode->replaceTo)
			elog(WARNING, "duplicate TO argument, use first one");
		else
		{
			curnode->replacelen = replacelen;
			curnode->replaceTo = palloc(replacelen);
			memcpy(curnode->replaceTo, replaceTo, replacelen);
		}
	}
	else
	{
		curnode->nextChar = placeChar(curnode->nextChar, str + 1, lenstr - 1, replaceTo, replacelen);
	}

	return node;
}

/*
 * initTrie  - create trie from file.
 *
 * Function converts UTF8-encoded file into current encoding.
 */
static TrieChar *
initTrie(char *filename)
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
				/*
				 * The format of each line must be "src trg" where src and trg
				 * are sequences of one or more non-whitespace characters,
				 * separated by whitespace.  Whitespace at start or end of
				 * line is ignored.
				 */
				int			state;
				char	   *ptr;
				char	   *src = NULL;
				char	   *trg = NULL;
				int			ptrlen;
				int			srclen = 0;
				int			trglen = 0;

				state = 0;
				for (ptr = line; *ptr; ptr += ptrlen)
				{
					ptrlen = pg_mblen(ptr);
					/* ignore whitespace, but end src or trg */
					if (t_isspace(ptr))
					{
						if (state == 1)
							state = 2;
						else if (state == 3)
							state = 4;
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
							trg = ptr;
							trglen = ptrlen;
							state = 3;
							break;
						case 3:
							/* continue trg */
							trglen += ptrlen;
							break;
						default:
							/* bogus line format */
							state = -1;
							break;
					}
				}

				if (state >= 3)
					rootTrie = placeChar(rootTrie,
										 (unsigned char *) src, srclen,
										 trg, trglen);

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
 * findReplaceTo - find multibyte character in trie
 */
static TrieChar *
findReplaceTo(TrieChar *node, unsigned char *src, int srclen)
{
	while (node)
	{
		node = node + *src;
		if (srclen == 1)
			return node;

		src++;
		srclen--;
		node = node->nextChar;
	}

	return NULL;
}

PG_FUNCTION_INFO_V1(unaccent_init);
Datum		unaccent_init(PG_FUNCTION_ARGS);
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

		if (pg_strcasecmp("Rules", defel->defname) == 0)
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
Datum		unaccent_lexize(PG_FUNCTION_ARGS);
Datum
unaccent_lexize(PG_FUNCTION_ARGS)
{
	TrieChar   *rootTrie = (TrieChar *) PG_GETARG_POINTER(0);
	char	   *srcchar = (char *) PG_GETARG_POINTER(1);
	int32		len = PG_GETARG_INT32(2);
	char	   *srcstart,
			   *trgchar = NULL;
	int			charlen;
	TSLexeme   *res = NULL;
	TrieChar   *node;

	srcstart = srcchar;
	while (srcchar - srcstart < len)
	{
		charlen = pg_mblen(srcchar);

		node = findReplaceTo(rootTrie, (unsigned char *) srcchar, charlen);
		if (node && node->replaceTo)
		{
			if (!res)
			{
				/* allocate res only if it's needed */
				res = palloc0(sizeof(TSLexeme) * 2);
				res->lexeme = trgchar = palloc(len * pg_database_encoding_max_length() + 1 /* \0 */ );
				res->flags = TSL_FILTER;
				if (srcchar != srcstart)
				{
					memcpy(trgchar, srcstart, srcchar - srcstart);
					trgchar += (srcchar - srcstart);
				}
			}
			memcpy(trgchar, node->replaceTo, node->replacelen);
			trgchar += node->replacelen;
		}
		else if (res)
		{
			memcpy(trgchar, srcchar, charlen);
			trgchar += charlen;
		}

		srcchar += charlen;
	}

	if (res)
		*trgchar = '\0';

	PG_RETURN_POINTER(res);
}

/*
 * Function-like wrapper for dictionary
 */
PG_FUNCTION_INFO_V1(unaccent_dict);
Datum		unaccent_dict(PG_FUNCTION_ARGS);
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
		dictOid = get_ts_dict_oid(stringToQualifiedNameList("unaccent"), false);
		strArg = 0;
	}
	else
	{
		dictOid = PG_GETARG_OID(0);
		strArg = 1;
	}
	str = PG_GETARG_TEXT_P(strArg);

	dict = lookup_ts_dictionary_cache(dictOid);

	res = (TSLexeme *) DatumGetPointer(FunctionCall4(&(dict->lexize),
											 PointerGetDatum(dict->dictData),
											   PointerGetDatum(VARDATA(str)),
									  Int32GetDatum(VARSIZE(str) - VARHDRSZ),
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
