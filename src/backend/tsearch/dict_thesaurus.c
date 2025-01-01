/*-------------------------------------------------------------------------
 *
 * dict_thesaurus.c
 *		Thesaurus dictionary: phrase to phrase substitution
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/dict_thesaurus.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "utils/fmgrprotos.h"
#include "utils/regproc.h"


/*
 * Temporary we use TSLexeme.flags for inner use...
 */
#define DT_USEASIS		0x1000

typedef struct LexemeInfo
{
	uint32		idsubst;		/* entry's number in DictThesaurus->subst */
	uint16		posinsubst;		/* pos info in entry */
	uint16		tnvariant;		/* total num lexemes in one variant */
	struct LexemeInfo *nextentry;
	struct LexemeInfo *nextvariant;
} LexemeInfo;

typedef struct
{
	char	   *lexeme;
	LexemeInfo *entries;
} TheLexeme;

typedef struct
{
	uint16		lastlexeme;		/* number lexemes to substitute */
	uint16		reslen;
	TSLexeme   *res;			/* prepared substituted result */
} TheSubstitute;

typedef struct
{
	/* subdictionary to normalize lexemes */
	Oid			subdictOid;
	TSDictionaryCacheEntry *subdict;

	/* Array to search lexeme by exact match */
	TheLexeme  *wrds;
	int			nwrds;			/* current number of words */
	int			ntwrds;			/* allocated array length */

	/*
	 * Storage of substituted result, n-th element is for n-th expression
	 */
	TheSubstitute *subst;
	int			nsubst;
} DictThesaurus;


static void
newLexeme(DictThesaurus *d, char *b, char *e, uint32 idsubst, uint16 posinsubst)
{
	TheLexeme  *ptr;

	if (d->nwrds >= d->ntwrds)
	{
		if (d->ntwrds == 0)
		{
			d->ntwrds = 16;
			d->wrds = (TheLexeme *) palloc(sizeof(TheLexeme) * d->ntwrds);
		}
		else
		{
			d->ntwrds *= 2;
			d->wrds = (TheLexeme *) repalloc(d->wrds, sizeof(TheLexeme) * d->ntwrds);
		}
	}

	ptr = d->wrds + d->nwrds;
	d->nwrds++;

	ptr->lexeme = palloc(e - b + 1);

	memcpy(ptr->lexeme, b, e - b);
	ptr->lexeme[e - b] = '\0';

	ptr->entries = (LexemeInfo *) palloc(sizeof(LexemeInfo));

	ptr->entries->nextentry = NULL;
	ptr->entries->idsubst = idsubst;
	ptr->entries->posinsubst = posinsubst;
}

static void
addWrd(DictThesaurus *d, char *b, char *e, uint32 idsubst, uint16 nwrd, uint16 posinsubst, bool useasis)
{
	static int	nres = 0;
	static int	ntres = 0;
	TheSubstitute *ptr;

	if (nwrd == 0)
	{
		nres = ntres = 0;

		if (idsubst >= d->nsubst)
		{
			if (d->nsubst == 0)
			{
				d->nsubst = 16;
				d->subst = (TheSubstitute *) palloc(sizeof(TheSubstitute) * d->nsubst);
			}
			else
			{
				d->nsubst *= 2;
				d->subst = (TheSubstitute *) repalloc(d->subst, sizeof(TheSubstitute) * d->nsubst);
			}
		}
	}

	ptr = d->subst + idsubst;

	ptr->lastlexeme = posinsubst - 1;

	if (nres + 1 >= ntres)
	{
		if (ntres == 0)
		{
			ntres = 2;
			ptr->res = (TSLexeme *) palloc(sizeof(TSLexeme) * ntres);
		}
		else
		{
			ntres *= 2;
			ptr->res = (TSLexeme *) repalloc(ptr->res, sizeof(TSLexeme) * ntres);
		}
	}

	ptr->res[nres].lexeme = palloc(e - b + 1);
	memcpy(ptr->res[nres].lexeme, b, e - b);
	ptr->res[nres].lexeme[e - b] = '\0';

	ptr->res[nres].nvariant = nwrd;
	if (useasis)
		ptr->res[nres].flags = DT_USEASIS;
	else
		ptr->res[nres].flags = 0;

	ptr->res[++nres].lexeme = NULL;
}

#define TR_WAITLEX	1
#define TR_INLEX	2
#define TR_WAITSUBS 3
#define TR_INSUBS	4

static void
thesaurusRead(const char *filename, DictThesaurus *d)
{
	tsearch_readline_state trst;
	uint32		idsubst = 0;
	bool		useasis = false;
	char	   *line;

	filename = get_tsearch_config_filename(filename, "ths");
	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open thesaurus file \"%s\": %m",
						filename)));

	while ((line = tsearch_readline(&trst)) != NULL)
	{
		char	   *ptr;
		int			state = TR_WAITLEX;
		char	   *beginwrd = NULL;
		uint32		posinsubst = 0;
		uint32		nwrd = 0;

		ptr = line;

		/* is it a comment? */
		while (*ptr && isspace((unsigned char) *ptr))
			ptr += pg_mblen(ptr);

		if (t_iseq(ptr, '#') || *ptr == '\0' ||
			t_iseq(ptr, '\n') || t_iseq(ptr, '\r'))
		{
			pfree(line);
			continue;
		}

		while (*ptr)
		{
			if (state == TR_WAITLEX)
			{
				if (t_iseq(ptr, ':'))
				{
					if (posinsubst == 0)
						ereport(ERROR,
								(errcode(ERRCODE_CONFIG_FILE_ERROR),
								 errmsg("unexpected delimiter")));
					state = TR_WAITSUBS;
				}
				else if (!isspace((unsigned char) *ptr))
				{
					beginwrd = ptr;
					state = TR_INLEX;
				}
			}
			else if (state == TR_INLEX)
			{
				if (t_iseq(ptr, ':'))
				{
					newLexeme(d, beginwrd, ptr, idsubst, posinsubst++);
					state = TR_WAITSUBS;
				}
				else if (isspace((unsigned char) *ptr))
				{
					newLexeme(d, beginwrd, ptr, idsubst, posinsubst++);
					state = TR_WAITLEX;
				}
			}
			else if (state == TR_WAITSUBS)
			{
				if (t_iseq(ptr, '*'))
				{
					useasis = true;
					state = TR_INSUBS;
					beginwrd = ptr + pg_mblen(ptr);
				}
				else if (t_iseq(ptr, '\\'))
				{
					useasis = false;
					state = TR_INSUBS;
					beginwrd = ptr + pg_mblen(ptr);
				}
				else if (!isspace((unsigned char) *ptr))
				{
					useasis = false;
					beginwrd = ptr;
					state = TR_INSUBS;
				}
			}
			else if (state == TR_INSUBS)
			{
				if (isspace((unsigned char) *ptr))
				{
					if (ptr == beginwrd)
						ereport(ERROR,
								(errcode(ERRCODE_CONFIG_FILE_ERROR),
								 errmsg("unexpected end of line or lexeme")));
					addWrd(d, beginwrd, ptr, idsubst, nwrd++, posinsubst, useasis);
					state = TR_WAITSUBS;
				}
			}
			else
				elog(ERROR, "unrecognized thesaurus state: %d", state);

			ptr += pg_mblen(ptr);
		}

		if (state == TR_INSUBS)
		{
			if (ptr == beginwrd)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("unexpected end of line or lexeme")));
			addWrd(d, beginwrd, ptr, idsubst, nwrd++, posinsubst, useasis);
		}

		idsubst++;

		if (!(nwrd && posinsubst))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("unexpected end of line")));

		if (nwrd != (uint16) nwrd || posinsubst != (uint16) posinsubst)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("too many lexemes in thesaurus entry")));

		pfree(line);
	}

	d->nsubst = idsubst;

	tsearch_readline_end(&trst);
}

static TheLexeme *
addCompiledLexeme(TheLexeme *newwrds, int *nnw, int *tnm, TSLexeme *lexeme, LexemeInfo *src, uint16 tnvariant)
{
	if (*nnw >= *tnm)
	{
		*tnm *= 2;
		newwrds = (TheLexeme *) repalloc(newwrds, sizeof(TheLexeme) * *tnm);
	}

	newwrds[*nnw].entries = (LexemeInfo *) palloc(sizeof(LexemeInfo));

	if (lexeme && lexeme->lexeme)
	{
		newwrds[*nnw].lexeme = pstrdup(lexeme->lexeme);
		newwrds[*nnw].entries->tnvariant = tnvariant;
	}
	else
	{
		newwrds[*nnw].lexeme = NULL;
		newwrds[*nnw].entries->tnvariant = 1;
	}

	newwrds[*nnw].entries->idsubst = src->idsubst;
	newwrds[*nnw].entries->posinsubst = src->posinsubst;

	newwrds[*nnw].entries->nextentry = NULL;

	(*nnw)++;
	return newwrds;
}

static int
cmpLexemeInfo(LexemeInfo *a, LexemeInfo *b)
{
	if (a == NULL || b == NULL)
		return 0;

	if (a->idsubst == b->idsubst)
	{
		if (a->posinsubst == b->posinsubst)
		{
			if (a->tnvariant == b->tnvariant)
				return 0;

			return (a->tnvariant > b->tnvariant) ? 1 : -1;
		}

		return (a->posinsubst > b->posinsubst) ? 1 : -1;
	}

	return (a->idsubst > b->idsubst) ? 1 : -1;
}

static int
cmpLexeme(const TheLexeme *a, const TheLexeme *b)
{
	if (a->lexeme == NULL)
	{
		if (b->lexeme == NULL)
			return 0;
		else
			return 1;
	}
	else if (b->lexeme == NULL)
		return -1;

	return strcmp(a->lexeme, b->lexeme);
}

static int
cmpLexemeQ(const void *a, const void *b)
{
	return cmpLexeme((const TheLexeme *) a, (const TheLexeme *) b);
}

static int
cmpTheLexeme(const void *a, const void *b)
{
	const TheLexeme *la = (const TheLexeme *) a;
	const TheLexeme *lb = (const TheLexeme *) b;
	int			res;

	if ((res = cmpLexeme(la, lb)) != 0)
		return res;

	return -cmpLexemeInfo(la->entries, lb->entries);
}

static void
compileTheLexeme(DictThesaurus *d)
{
	int			i,
				nnw = 0,
				tnm = 16;
	TheLexeme  *newwrds = (TheLexeme *) palloc(sizeof(TheLexeme) * tnm),
			   *ptrwrds;

	for (i = 0; i < d->nwrds; i++)
	{
		TSLexeme   *ptr;

		if (strcmp(d->wrds[i].lexeme, "?") == 0)	/* Is stop word marker? */
			newwrds = addCompiledLexeme(newwrds, &nnw, &tnm, NULL, d->wrds[i].entries, 0);
		else
		{
			ptr = (TSLexeme *) DatumGetPointer(FunctionCall4(&(d->subdict->lexize),
															 PointerGetDatum(d->subdict->dictData),
															 PointerGetDatum(d->wrds[i].lexeme),
															 Int32GetDatum(strlen(d->wrds[i].lexeme)),
															 PointerGetDatum(NULL)));

			if (!ptr)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("thesaurus sample word \"%s\" isn't recognized by subdictionary (rule %d)",
								d->wrds[i].lexeme,
								d->wrds[i].entries->idsubst + 1)));
			else if (!(ptr->lexeme))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("thesaurus sample word \"%s\" is a stop word (rule %d)",
								d->wrds[i].lexeme,
								d->wrds[i].entries->idsubst + 1),
						 errhint("Use \"?\" to represent a stop word within a sample phrase.")));
			else
			{
				while (ptr->lexeme)
				{
					TSLexeme   *remptr = ptr + 1;
					int			tnvar = 1;
					int			curvar = ptr->nvariant;

					/* compute n words in one variant */
					while (remptr->lexeme)
					{
						if (remptr->nvariant != (remptr - 1)->nvariant)
							break;
						tnvar++;
						remptr++;
					}

					remptr = ptr;
					while (remptr->lexeme && remptr->nvariant == curvar)
					{
						newwrds = addCompiledLexeme(newwrds, &nnw, &tnm, remptr, d->wrds[i].entries, tnvar);
						remptr++;
					}

					ptr = remptr;
				}
			}
		}

		pfree(d->wrds[i].lexeme);
		pfree(d->wrds[i].entries);
	}

	if (d->wrds)
		pfree(d->wrds);
	d->wrds = newwrds;
	d->nwrds = nnw;
	d->ntwrds = tnm;

	if (d->nwrds > 1)
	{
		qsort(d->wrds, d->nwrds, sizeof(TheLexeme), cmpTheLexeme);

		/* uniq */
		newwrds = d->wrds;
		ptrwrds = d->wrds + 1;
		while (ptrwrds - d->wrds < d->nwrds)
		{
			if (cmpLexeme(ptrwrds, newwrds) == 0)
			{
				if (cmpLexemeInfo(ptrwrds->entries, newwrds->entries))
				{
					ptrwrds->entries->nextentry = newwrds->entries;
					newwrds->entries = ptrwrds->entries;
				}
				else
					pfree(ptrwrds->entries);

				if (ptrwrds->lexeme)
					pfree(ptrwrds->lexeme);
			}
			else
			{
				newwrds++;
				*newwrds = *ptrwrds;
			}

			ptrwrds++;
		}

		d->nwrds = newwrds - d->wrds + 1;
		d->wrds = (TheLexeme *) repalloc(d->wrds, sizeof(TheLexeme) * d->nwrds);
	}
}

static void
compileTheSubstitute(DictThesaurus *d)
{
	int			i;

	for (i = 0; i < d->nsubst; i++)
	{
		TSLexeme   *rem = d->subst[i].res,
				   *outptr,
				   *inptr;
		int			n = 2;

		outptr = d->subst[i].res = (TSLexeme *) palloc(sizeof(TSLexeme) * n);
		outptr->lexeme = NULL;
		inptr = rem;

		while (inptr && inptr->lexeme)
		{
			TSLexeme   *lexized,
						tmplex[2];

			if (inptr->flags & DT_USEASIS)
			{					/* do not lexize */
				tmplex[0] = *inptr;
				tmplex[0].flags = 0;
				tmplex[1].lexeme = NULL;
				lexized = tmplex;
			}
			else
			{
				lexized = (TSLexeme *) DatumGetPointer(FunctionCall4(&(d->subdict->lexize),
																	 PointerGetDatum(d->subdict->dictData),
																	 PointerGetDatum(inptr->lexeme),
																	 Int32GetDatum(strlen(inptr->lexeme)),
																	 PointerGetDatum(NULL)));
			}

			if (lexized && lexized->lexeme)
			{
				int			toset = (lexized->lexeme && outptr != d->subst[i].res) ? (outptr - d->subst[i].res) : -1;

				while (lexized->lexeme)
				{
					if (outptr - d->subst[i].res + 1 >= n)
					{
						int			diff = outptr - d->subst[i].res;

						n *= 2;
						d->subst[i].res = (TSLexeme *) repalloc(d->subst[i].res, sizeof(TSLexeme) * n);
						outptr = d->subst[i].res + diff;
					}

					*outptr = *lexized;
					outptr->lexeme = pstrdup(lexized->lexeme);

					outptr++;
					lexized++;
				}

				if (toset > 0)
					d->subst[i].res[toset].flags |= TSL_ADDPOS;
			}
			else if (lexized)
			{
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("thesaurus substitute word \"%s\" is a stop word (rule %d)",
								inptr->lexeme, i + 1)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("thesaurus substitute word \"%s\" isn't recognized by subdictionary (rule %d)",
								inptr->lexeme, i + 1)));
			}

			if (inptr->lexeme)
				pfree(inptr->lexeme);
			inptr++;
		}

		if (outptr == d->subst[i].res)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("thesaurus substitute phrase is empty (rule %d)",
							i + 1)));

		d->subst[i].reslen = outptr - d->subst[i].res;

		pfree(rem);
	}
}

Datum
thesaurus_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	DictThesaurus *d;
	char	   *subdictname = NULL;
	bool		fileloaded = false;
	List	   *namelist;
	ListCell   *l;

	d = (DictThesaurus *) palloc0(sizeof(DictThesaurus));

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (strcmp(defel->defname, "dictfile") == 0)
		{
			if (fileloaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple DictFile parameters")));
			thesaurusRead(defGetString(defel), d);
			fileloaded = true;
		}
		else if (strcmp(defel->defname, "dictionary") == 0)
		{
			if (subdictname)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple Dictionary parameters")));
			subdictname = pstrdup(defGetString(defel));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized Thesaurus parameter: \"%s\"",
							defel->defname)));
		}
	}

	if (!fileloaded)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing DictFile parameter")));
	if (!subdictname)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing Dictionary parameter")));

	namelist = stringToQualifiedNameList(subdictname, NULL);
	d->subdictOid = get_ts_dict_oid(namelist, false);
	d->subdict = lookup_ts_dictionary_cache(d->subdictOid);

	compileTheLexeme(d);
	compileTheSubstitute(d);

	PG_RETURN_POINTER(d);
}

static LexemeInfo *
findTheLexeme(DictThesaurus *d, char *lexeme)
{
	TheLexeme	key,
			   *res;

	if (d->nwrds == 0)
		return NULL;

	key.lexeme = lexeme;
	key.entries = NULL;

	res = bsearch(&key, d->wrds, d->nwrds, sizeof(TheLexeme), cmpLexemeQ);

	if (res == NULL)
		return NULL;
	return res->entries;
}

static bool
matchIdSubst(LexemeInfo *stored, uint32 idsubst)
{
	bool		res = true;

	if (stored)
	{
		res = false;

		for (; stored; stored = stored->nextvariant)
			if (stored->idsubst == idsubst)
			{
				res = true;
				break;
			}
	}

	return res;
}

static LexemeInfo *
findVariant(LexemeInfo *in, LexemeInfo *stored, uint16 curpos, LexemeInfo **newin, int newn)
{
	for (;;)
	{
		int			i;
		LexemeInfo *ptr = newin[0];

		for (i = 0; i < newn; i++)
		{
			while (newin[i] && newin[i]->idsubst < ptr->idsubst)
				newin[i] = newin[i]->nextentry;

			if (newin[i] == NULL)
				return in;

			if (newin[i]->idsubst > ptr->idsubst)
			{
				ptr = newin[i];
				i = -1;
				continue;
			}

			while (newin[i]->idsubst == ptr->idsubst)
			{
				if (newin[i]->posinsubst == curpos && newin[i]->tnvariant == newn)
				{
					ptr = newin[i];
					break;
				}

				newin[i] = newin[i]->nextentry;
				if (newin[i] == NULL)
					return in;
			}

			if (newin[i]->idsubst != ptr->idsubst)
			{
				ptr = newin[i];
				i = -1;
				continue;
			}
		}

		if (i == newn && matchIdSubst(stored, ptr->idsubst) && (in == NULL || !matchIdSubst(in, ptr->idsubst)))
		{						/* found */

			ptr->nextvariant = in;
			in = ptr;
		}

		/* step forward */
		for (i = 0; i < newn; i++)
			newin[i] = newin[i]->nextentry;
	}
}

static TSLexeme *
copyTSLexeme(TheSubstitute *ts)
{
	TSLexeme   *res;
	uint16		i;

	res = (TSLexeme *) palloc(sizeof(TSLexeme) * (ts->reslen + 1));
	for (i = 0; i < ts->reslen; i++)
	{
		res[i] = ts->res[i];
		res[i].lexeme = pstrdup(ts->res[i].lexeme);
	}

	res[ts->reslen].lexeme = NULL;

	return res;
}

static TSLexeme *
checkMatch(DictThesaurus *d, LexemeInfo *info, uint16 curpos, bool *moreres)
{
	*moreres = false;
	while (info)
	{
		Assert(info->idsubst < d->nsubst);
		if (info->nextvariant)
			*moreres = true;
		if (d->subst[info->idsubst].lastlexeme == curpos)
			return copyTSLexeme(d->subst + info->idsubst);
		info = info->nextvariant;
	}

	return NULL;
}

Datum
thesaurus_lexize(PG_FUNCTION_ARGS)
{
	DictThesaurus *d = (DictThesaurus *) PG_GETARG_POINTER(0);
	DictSubState *dstate = (DictSubState *) PG_GETARG_POINTER(3);
	TSLexeme   *res = NULL;
	LexemeInfo *stored,
			   *info = NULL;
	uint16		curpos = 0;
	bool		moreres = false;

	if (PG_NARGS() != 4 || dstate == NULL)
		elog(ERROR, "forbidden call of thesaurus or nested call");

	if (dstate->isend)
		PG_RETURN_POINTER(NULL);
	stored = (LexemeInfo *) dstate->private_state;

	if (stored)
		curpos = stored->posinsubst + 1;

	if (!d->subdict->isvalid)
		d->subdict = lookup_ts_dictionary_cache(d->subdictOid);

	res = (TSLexeme *) DatumGetPointer(FunctionCall4(&(d->subdict->lexize),
													 PointerGetDatum(d->subdict->dictData),
													 PG_GETARG_DATUM(1),
													 PG_GETARG_DATUM(2),
													 PointerGetDatum(NULL)));

	if (res && res->lexeme)
	{
		TSLexeme   *ptr = res,
				   *basevar;

		while (ptr->lexeme)
		{
			uint16		nv = ptr->nvariant;
			uint16		i,
						nlex = 0;
			LexemeInfo **infos;

			basevar = ptr;
			while (ptr->lexeme && nv == ptr->nvariant)
			{
				nlex++;
				ptr++;
			}

			infos = (LexemeInfo **) palloc(sizeof(LexemeInfo *) * nlex);
			for (i = 0; i < nlex; i++)
				if ((infos[i] = findTheLexeme(d, basevar[i].lexeme)) == NULL)
					break;

			if (i < nlex)
			{
				/* no chance to find */
				pfree(infos);
				continue;
			}

			info = findVariant(info, stored, curpos, infos, nlex);
		}
	}
	else if (res)
	{							/* stop-word */
		LexemeInfo *infos = findTheLexeme(d, NULL);

		info = findVariant(NULL, stored, curpos, &infos, 1);
	}
	else
	{
		info = NULL;			/* word isn't recognized */
	}

	dstate->private_state = info;

	if (!info)
	{
		dstate->getnext = false;
		PG_RETURN_POINTER(NULL);
	}

	if ((res = checkMatch(d, info, curpos, &moreres)) != NULL)
	{
		dstate->getnext = moreres;
		PG_RETURN_POINTER(res);
	}

	dstate->getnext = true;

	PG_RETURN_POINTER(NULL);
}
