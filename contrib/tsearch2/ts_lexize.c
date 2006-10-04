/*
 * lexize stream of lexemes
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include <ctype.h>
#include <locale.h>

#include "ts_cfg.h"
#include "dict.h"

void
LexizeInit(LexizeData * ld, TSCfgInfo * cfg)
{
	ld->cfg = cfg;
	ld->curDictId = InvalidOid;
	ld->posDict = 0;
	ld->towork.head = ld->towork.tail = ld->curSub = NULL;
	ld->waste.head = ld->waste.tail = NULL;
	ld->lastRes = NULL;
	ld->tmpRes = NULL;
}

static void
LPLAddTail(ListParsedLex * list, ParsedLex * newpl)
{
	if (list->tail)
	{
		list->tail->next = newpl;
		list->tail = newpl;
	}
	else
		list->head = list->tail = newpl;
	newpl->next = NULL;
}

static ParsedLex *
LPLRemoveHead(ListParsedLex * list)
{
	ParsedLex  *res = list->head;

	if (list->head)
		list->head = list->head->next;

	if (list->head == NULL)
		list->tail = NULL;

	return res;
}


void
LexizeAddLemm(LexizeData * ld, int type, char *lemm, int lenlemm)
{
	ParsedLex  *newpl = (ParsedLex *) palloc(sizeof(ParsedLex));

	newpl = (ParsedLex *) palloc(sizeof(ParsedLex));
	newpl->type = type;
	newpl->lemm = lemm;
	newpl->lenlemm = lenlemm;
	LPLAddTail(&ld->towork, newpl);
	ld->curSub = ld->towork.tail;
}

static void
RemoveHead(LexizeData * ld)
{
	LPLAddTail(&ld->waste, LPLRemoveHead(&ld->towork));

	ld->posDict = 0;
}

static void
setCorrLex(LexizeData * ld, ParsedLex ** correspondLexem)
{
	if (correspondLexem)
	{
		*correspondLexem = ld->waste.head;
	}
	else
	{
		ParsedLex  *tmp,
				   *ptr = ld->waste.head;

		while (ptr)
		{
			tmp = ptr->next;
			pfree(ptr);
			ptr = tmp;
		}
	}
	ld->waste.head = ld->waste.tail = NULL;
}

static void
moveToWaste(LexizeData * ld, ParsedLex * stop)
{
	bool		go = true;

	while (ld->towork.head && go)
	{
		if (ld->towork.head == stop)
		{
			ld->curSub = stop->next;
			go = false;
		}
		RemoveHead(ld);
	}
}

static void
setNewTmpRes(LexizeData * ld, ParsedLex * lex, TSLexeme * res)
{
	if (ld->tmpRes)
	{
		TSLexeme   *ptr;

		for (ptr = ld->tmpRes; ptr->lexeme; ptr++)
			pfree(ptr->lexeme);
		pfree(ld->tmpRes);
	}
	ld->tmpRes = res;
	ld->lastRes = lex;
}

TSLexeme *
LexizeExec(LexizeData * ld, ParsedLex ** correspondLexem)
{
	int			i;
	ListDictionary *map;
	DictInfo   *dict;
	TSLexeme   *res;

	if (ld->curDictId == InvalidOid)
	{
		/*
		 * usial mode: dictionary wants only one word, but we should keep in
		 * mind that we should go through all stack
		 */

		while (ld->towork.head)
		{
			ParsedLex  *curVal = ld->towork.head;

			map = ld->cfg->map + curVal->type;

			if (curVal->type == 0 || curVal->type >= ld->cfg->len || map->len == 0)
			{
				/* skip this type of lexeme */
				RemoveHead(ld);
				continue;
			}

			for (i = ld->posDict; i < map->len; i++)
			{
				dict = finddict(DatumGetObjectId(map->dict_id[i]));

				ld->dictState.isend = ld->dictState.getnext = false;
				ld->dictState.private = NULL;
				res = (TSLexeme *) DatumGetPointer(FunctionCall4(
														&(dict->lexize_info),
										   PointerGetDatum(dict->dictionary),
											   PointerGetDatum(curVal->lemm),
											  Int32GetDatum(curVal->lenlemm),
											  PointerGetDatum(&ld->dictState)
																 ));

				if (ld->dictState.getnext)
				{
					/*
					 * dictinary wants next word, so setup and store current
					 * position and go to multiword  mode
					 */

					ld->curDictId = DatumGetObjectId(map->dict_id[i]);
					ld->posDict = i + 1;
					ld->curSub = curVal->next;
					if (res)
						setNewTmpRes(ld, curVal, res);
					return LexizeExec(ld, correspondLexem);
				}

				if (!res)		/* dictionary doesn't know this lexeme */
					continue;

				RemoveHead(ld);
				setCorrLex(ld, correspondLexem);
				return res;
			}

			RemoveHead(ld);
		}
	}
	else
	{							/* curDictId is valid */
		dict = finddict(ld->curDictId);

		/*
		 * Dictionary ld->curDictId asks  us about following words
		 */

		while (ld->curSub)
		{
			ParsedLex  *curVal = ld->curSub;

			map = ld->cfg->map + curVal->type;

			if (curVal->type != 0)
			{
				bool		dictExists = false;

				if (curVal->type >= ld->cfg->len || map->len == 0)
				{
					/* skip this type of lexeme */
					ld->curSub = curVal->next;
					continue;
				}

				/*
				 * We should be sure that current type of lexeme is recognized
				 * by our dictinonary: we just check is it exist in list of
				 * dictionaries ?
				 */
				for (i = 0; i < map->len && !dictExists; i++)
					if (ld->curDictId == DatumGetObjectId(map->dict_id[i]))
						dictExists = true;

				if (!dictExists)
				{
					/*
					 * Dictionary can't work with current tpe of lexeme,
					 * return to basic mode and redo all stored lexemes
					 */
					ld->curDictId = InvalidOid;
					return LexizeExec(ld, correspondLexem);
				}
			}

			ld->dictState.isend = (curVal->type == 0) ? true : false;
			ld->dictState.getnext = false;

			res = (TSLexeme *) DatumGetPointer(FunctionCall4(
														&(dict->lexize_info),
										   PointerGetDatum(dict->dictionary),
											   PointerGetDatum(curVal->lemm),
											  Int32GetDatum(curVal->lenlemm),
											  PointerGetDatum(&ld->dictState)
															 ));

			if (ld->dictState.getnext)
			{
				/* Dictionary wants one more */
				ld->curSub = curVal->next;
				if (res)
					setNewTmpRes(ld, curVal, res);
				continue;
			}

			if (res || ld->tmpRes)
			{
				/*
				 * Dictionary normalizes lexemes, so we remove from stack all
				 * used lexemes , return to basic mode and redo end of stack
				 * (if it exists)
				 */
				if (res)
				{
					moveToWaste(ld, ld->curSub);
				}
				else
				{
					res = ld->tmpRes;
					moveToWaste(ld, ld->lastRes);
				}

				/* reset to initial state */
				ld->curDictId = InvalidOid;
				ld->posDict = 0;
				ld->lastRes = NULL;
				ld->tmpRes = NULL;
				setCorrLex(ld, correspondLexem);
				return res;
			}

			/*
			 * Dict don't want next lexem and didn't recognize anything, redo
			 * from ld->towork.head
			 */
			ld->curDictId = InvalidOid;
			return LexizeExec(ld, correspondLexem);
		}
	}

	setCorrLex(ld, correspondLexem);
	return NULL;
}
