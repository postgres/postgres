/* $PostgreSQL: pgsql/contrib/tsearch2/dict_thesaurus.c,v 1.6.2.1 2006/12/26 14:55:00 teodor Exp $ */

/*
 * thesaurus
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"
#include "executor/spi.h"

#include <ctype.h>

#include "dict.h"
#include "common.h"
#include "ts_locale.h"

/*
 * Temporay we use TSLexeme.flags for inner use...
 */
#define DT_USEASIS		0x1000

typedef struct LexemeInfo
{
	uint16		idsubst;		/* entry's number in DictThesaurus->subst */
	uint16		posinsubst;		/* pos info in entry */
	uint16		tnvariant;		/* total num lexemes in one variant */
	struct LexemeInfo *nextentry;
	struct LexemeInfo *nextvariant;
}	LexemeInfo;

typedef struct
{
	char	   *lexeme;
	LexemeInfo *entries;
}	TheLexeme;

typedef struct
{
	uint16		lastlexeme;		/* number lexemes to substitute */
	uint16		reslen;
	TSLexeme   *res;			/* prepared substituted result */
}	TheSubstitute;

typedef struct
{
	/* subdictionary to normalize lexemes */
	DictInfo	subdict;

	/* Array to search lexeme by exact match */
	TheLexeme  *wrds;
	int			nwrds;
	int			ntwrds;

	/*
	 * Storage of substituted result, n-th element is for n-th expression
	 */
	TheSubstitute *subst;
	int			nsubst;
}	DictThesaurus;

PG_FUNCTION_INFO_V1(thesaurus_init);
Datum		thesaurus_init(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(thesaurus_lexize);
Datum		thesaurus_lexize(PG_FUNCTION_ARGS);

static void
freeDictThesaurus(DictThesaurus * d)
{
	free(d);
}

static void
newLexeme(DictThesaurus * d, char *b, char *e, uint16 idsubst, uint16 posinsubst)
{
	TheLexeme  *ptr;

	if (d->nwrds >= d->ntwrds)
	{
		if (d->ntwrds == 0)
		{
			d->ntwrds = 16;
			d->wrds = (TheLexeme *) malloc(sizeof(TheLexeme) * d->ntwrds);
		}
		else
		{
			d->ntwrds *= 2;
			d->wrds = (TheLexeme *) realloc(d->wrds, sizeof(TheLexeme) * d->ntwrds);
		}
		if (!d->wrds)
			elog(ERROR, "Out of memory");
	}

	ptr = d->wrds + d->nwrds;
	d->nwrds++;

	if ((ptr->lexeme = malloc(e - b + 1)) == NULL)
		elog(ERROR, "Out of memory");

	memcpy(ptr->lexeme, b, e - b);
	ptr->lexeme[e - b] = '\0';

	if ((ptr->entries = (LexemeInfo *) malloc(sizeof(LexemeInfo))) == NULL)
		elog(ERROR, "Out of memory");

	ptr->entries->nextentry = NULL;
	ptr->entries->idsubst = idsubst;
	ptr->entries->posinsubst = posinsubst;
}

static void
addWrd(DictThesaurus * d, char *b, char *e, uint16 idsubst, uint16 nwrd, uint16 posinsubst, bool useasis)
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
				d->subst = (TheSubstitute *) malloc(sizeof(TheSubstitute) * d->nsubst);
			}
			else
			{
				d->nsubst *= 2;
				d->subst = (TheSubstitute *) realloc(d->subst, sizeof(TheSubstitute) * d->nsubst);
			}
			if (!d->subst)
				elog(ERROR, "Out of memory");
		}
	}

	ptr = d->subst + idsubst;

	ptr->lastlexeme = posinsubst - 1;

	if (nres + 1 >= ntres)
	{
		if (ntres == 0)
		{
			ntres = 2;
			ptr->res = (TSLexeme *) malloc(sizeof(TSLexeme) * ntres);
		}
		else
		{
			ntres *= 2;
			ptr->res = (TSLexeme *) realloc(ptr->res, sizeof(TSLexeme) * ntres);
		}

		if (!ptr->res)
			elog(ERROR, "Out of memory");
	}

	if ((ptr->res[nres].lexeme = malloc(e - b + 1)) == 0)
		elog(ERROR, "Out of memory");
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
thesaurusRead(char *filename, DictThesaurus * d)
{
	FILE	   *fh;
	char		str[BUFSIZ];
	int			lineno = 0;
	uint16		idsubst = 0;
	bool		useasis = false;

	fh = fopen(to_absfilename(filename), "r");
	if (!fh)
		elog(ERROR, "Thesaurus: can't open '%s' file", filename);

	while (fgets(str, sizeof(str), fh))
	{
		char	   *ptr = str;
		int			state = TR_WAITLEX;
		char	   *beginwrd = NULL;
		uint16		posinsubst = 0;
		uint16		nwrd = 0;

		lineno++;

		/* is it comment ? */
		while (t_isspace(ptr))
			ptr += pg_mblen(ptr);
		if (t_iseq(str, '#') || *str == '\0' || t_iseq(str, '\n') || t_iseq(str, '\r'))
			continue;

		pg_verifymbstr(ptr, strlen(ptr), false);
		while (*ptr)
		{
			if (state == TR_WAITLEX)
			{
				if (t_iseq(ptr, ':'))
				{
					if (posinsubst == 0)
					{
						fclose(fh);
						elog(ERROR, "Thesaurus: Unexpected delimiter at %d line", lineno);
					}
					state = TR_WAITSUBS;
				}
				else if (!t_isspace(ptr))
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
				else if (t_isspace(ptr))
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
				else if (!t_isspace(ptr))
				{
					useasis = false;
					beginwrd = ptr;
					state = TR_INSUBS;
				}
			}
			else if (state == TR_INSUBS)
			{
				if (t_isspace(ptr))
				{
					if (ptr == beginwrd)
						elog(ERROR, "Thesaurus: Unexpected end of line or lexeme at %d line", lineno);
					addWrd(d, beginwrd, ptr, idsubst, nwrd++, posinsubst, useasis);
					state = TR_WAITSUBS;
				}
			}
			else
				elog(ERROR, "Thesaurus: Unknown state: %d", state);

			ptr += pg_mblen(ptr);
		}

		if (state == TR_INSUBS)
		{
			if (ptr == beginwrd)
				elog(ERROR, "Thesaurus: Unexpected end of line or lexeme at %d line", lineno);
			addWrd(d, beginwrd, ptr, idsubst, nwrd++, posinsubst, useasis);
		}

		idsubst++;

		if (!(nwrd && posinsubst))
		{
			fclose(fh);
			elog(ERROR, "Thesaurus: Unexpected end of line at %d line", lineno);
		}

	}

	d->nsubst = idsubst;

	fclose(fh);
}

static TheLexeme *
addCompiledLexeme(TheLexeme * newwrds, int *nnw, int *tnm, TSLexeme * lexeme, LexemeInfo * src, uint16 tnvariant)
{

	if (*nnw >= *tnm)
	{
		*tnm *= 2;
		newwrds = (TheLexeme *) realloc(newwrds, sizeof(TheLexeme) * *tnm);
		if (!newwrds)
			elog(ERROR, "Out of memory");
	}

	newwrds[*nnw].entries = (LexemeInfo *) malloc(sizeof(LexemeInfo));
	if (!newwrds[*nnw].entries)
		elog(ERROR, "Out of memory");

	if (lexeme && lexeme->lexeme)
	{
		newwrds[*nnw].lexeme = strdup(lexeme->lexeme);
		if (!newwrds[*nnw].lexeme)
			elog(ERROR, "Out of memory");

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
cmpLexemeInfo(LexemeInfo * a, LexemeInfo * b)
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
cmpLexeme(TheLexeme * a, TheLexeme * b)
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
	return cmpLexeme((TheLexeme *) a, (TheLexeme *) b);
}

static int
cmpTheLexeme(const void *a, const void *b)
{
	TheLexeme  *la = (TheLexeme *) a;
	TheLexeme  *lb = (TheLexeme *) b;
	int			res;

	if ((res = cmpLexeme(la, lb)) != 0)
		return res;

	return -cmpLexemeInfo(la->entries, lb->entries);
}

static void
compileTheLexeme(DictThesaurus * d)
{
	int			i,
				nnw = 0,
				tnm = 16;
	TheLexeme  *newwrds = (TheLexeme *) malloc(sizeof(TheLexeme) * tnm),
			   *ptrwrds;

	if (!newwrds)
		elog(ERROR, "Out of memory");

	for (i = 0; i < d->nwrds; i++)
	{
		TSLexeme   *ptr;

		ptr = (TSLexeme *) DatumGetPointer(
										   FunctionCall4(
												   &(d->subdict.lexize_info),
									  PointerGetDatum(d->subdict.dictionary),
										  PointerGetDatum(d->wrds[i].lexeme),
									Int32GetDatum(strlen(d->wrds[i].lexeme)),
														 PointerGetDatum(NULL)
														 )
			);

		if (!(ptr && ptr->lexeme))
		{
			if (!ptr)
				elog(ERROR, "Thesaurus: word-sample '%s' isn't recognized by subdictionary (rule %d)",
					 d->wrds[i].lexeme, d->wrds[i].entries->idsubst + 1);
			else
				elog(NOTICE, "Thesaurus: word-sample '%s' is recognized as stop-word, assign any stop-word (rule %d)",
					 d->wrds[i].lexeme, d->wrds[i].entries->idsubst + 1);

			newwrds = addCompiledLexeme(newwrds, &nnw, &tnm, NULL, d->wrds[i].entries, 0);
		}
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

		free(d->wrds[i].lexeme);
		free(d->wrds[i].entries);
	}

	free(d->wrds);
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
					free(ptrwrds->entries);

				if (ptrwrds->lexeme)
					free(ptrwrds->lexeme);
			}
			else
			{
				newwrds++;
				*newwrds = *ptrwrds;
			}

			ptrwrds++;
		}

		d->nwrds = newwrds - d->wrds + 1;
		d->wrds = (TheLexeme *) realloc(d->wrds, sizeof(TheLexeme) * d->nwrds);
	}
}

static void
compileTheSubstitute(DictThesaurus * d)
{
	int			i;

	for (i = 0; i < d->nsubst; i++)
	{
		TSLexeme   *rem = d->subst[i].res,
				   *outptr,
				   *inptr;
		int			n = 2;

		outptr = d->subst[i].res = (TSLexeme *) malloc(sizeof(TSLexeme) * n);
		if (d->subst[i].res == NULL)
			elog(ERROR, "Out of Memory");
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
				lexized = (TSLexeme *) DatumGetPointer(
													   FunctionCall4(
												   &(d->subdict.lexize_info),
									  PointerGetDatum(d->subdict.dictionary),
											  PointerGetDatum(inptr->lexeme),
										Int32GetDatum(strlen(inptr->lexeme)),
														PointerGetDatum(NULL)
																	 )
					);
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
						d->subst[i].res = (TSLexeme *) realloc(d->subst[i].res, sizeof(TSLexeme) * n);
						if (d->subst[i].res == NULL)
							elog(ERROR, "Out of Memory");
						outptr = d->subst[i].res + diff;
					}

					*outptr = *lexized;
					if ((outptr->lexeme = strdup(lexized->lexeme)) == NULL)
						elog(ERROR, "Out of Memory");

					outptr++;
					lexized++;
				}

				if (toset > 0)
					d->subst[i].res[toset].flags |= TSL_ADDPOS;
			}
			else if (lexized)
			{
				elog(NOTICE, "Thesaurus: word '%s' in substition is a stop-word, ignored (rule %d)", inptr->lexeme, i + 1);
			}
			else
			{
				elog(ERROR, "Thesaurus: word '%s' in substition isn't recognized (rule %d)", inptr->lexeme, i + 1);
			}

			if (inptr->lexeme)
				free(inptr->lexeme);
			inptr++;
		}

		if (outptr == d->subst[i].res)
			elog(ERROR, "Thesaurus: all words in subsitution are stop word (rule %d)", i + 1);

		d->subst[i].reslen = outptr - d->subst[i].res;

		free(rem);
	}
}

Datum
thesaurus_init(PG_FUNCTION_ARGS)
{
	DictThesaurus *d;
	Map		   *cfg,
			   *pcfg;
	text	   *in,
			   *subdictname = NULL;
	bool		fileloaded = false;

	if (PG_ARGISNULL(0) || PG_GETARG_POINTER(0) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("Thesaurus confguration error")));

	d = (DictThesaurus *) malloc(sizeof(DictThesaurus));
	if (!d)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	memset(d, 0, sizeof(DictThesaurus));

	in = PG_GETARG_TEXT_P(0);
	parse_cfgdict(in, &cfg);
	PG_FREE_IF_COPY(in, 0);
	pcfg = cfg;
	while (pcfg->key)
	{
		if (pg_strcasecmp("DictFile", pcfg->key) == 0)
		{
			if (fileloaded)
			{
				freeDictThesaurus(d);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("Thesaurus file is already loaded")));
			}
			fileloaded = true;
			thesaurusRead(pcfg->value, d);
		}
		else if (pg_strcasecmp("Dictionary", pcfg->key) == 0)
		{
			if (subdictname)
			{
				freeDictThesaurus(d);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("Thesaurus: SubDictionary is already defined")));
			}
			subdictname = char2text(pcfg->value);
		}
		else
		{
			freeDictThesaurus(d);
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized option: %s => %s",
							pcfg->key, pcfg->value)));
		}
		pfree(pcfg->key);
		pfree(pcfg->value);
		pcfg++;
	}
	pfree(cfg);

	if (!fileloaded)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("Thesaurus file  isn't defined")));

	if (subdictname)
	{
		DictInfo   *subdictptr;

		/*
		 * we already in SPI, but name2id_dict()/finddict() invoke
		 * SPI_connect()
		 */
		SPI_push();

		subdictptr = finddict(name2id_dict(subdictname));

		SPI_pop();

		d->subdict = *subdictptr;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("Thesaurus: SubDictionary isn't defined")));

	compileTheLexeme(d);
	compileTheSubstitute(d);

	PG_RETURN_POINTER(d);
}

static LexemeInfo *
findTheLexeme(DictThesaurus * d, char *lexeme)
{
	TheLexeme	key = {lexeme, NULL}, *res;

	if (d->nwrds == 0)
		return NULL;

	res = bsearch(&key, d->wrds, d->nwrds, sizeof(TheLexeme), cmpLexemeQ);

	if (res == NULL)
		return NULL;
	return res->entries;
}

static bool
matchIdSubst(LexemeInfo * stored, uint16 idsubst)
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
findVariant(LexemeInfo * in, LexemeInfo * stored, uint16 curpos, LexemeInfo ** newin, int newn)
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

	return NULL;
}

static TSLexeme *
copyTSLexeme(TheSubstitute * ts)
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
checkMatch(DictThesaurus * d, LexemeInfo * info, uint16 curpos, bool *moreres)
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

	if (dstate == NULL || PG_NARGS() < 4)
		elog(ERROR, "Forbidden call of thesaurus or nested call");

	if (dstate->isend)
		PG_RETURN_POINTER(NULL);
	stored = (LexemeInfo *) dstate->private;

	if (stored)
		curpos = stored->posinsubst + 1;

	res = (TSLexeme *) DatumGetPointer(
									   FunctionCall4(
												   &(d->subdict.lexize_info),
									  PointerGetDatum(d->subdict.dictionary),
													 PG_GETARG_DATUM(1),
													 PG_GETARG_INT32(2),
													 PointerGetDatum(NULL)
													 )
		);

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

	dstate->private = (void *) info;

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
