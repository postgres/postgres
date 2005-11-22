#include "postgres.h"
#include "executor/spi.h"
#include "query_util.h"

QTNode *
QT2QTN(ITEM * in, char *operand)
{
	QTNode	   *node = (QTNode *) palloc0(sizeof(QTNode));

	node->valnode = in;

	if (in->type == OPR)
	{
		node->child = (QTNode **) palloc0(sizeof(QTNode *) * 2);
		node->child[0] = QT2QTN(in + 1, operand);
		node->sign = node->child[0]->sign;
		if (in->val == (int4) '!')
			node->nchild = 1;
		else
		{
			node->nchild = 2;
			node->child[1] = QT2QTN(in + in->left, operand);
			node->sign |= node->child[1]->sign;
		}
	}
	else if (operand)
	{
		node->word = operand + in->distance;
		node->sign = 1 << (in->val % 32);
	}

	return node;
}

void
QTNFree(QTNode * in)
{
	if (!in)
		return;

	if (in->valnode->type == VAL && in->word && (in->flags & QTN_WORDFREE) != 0)
		pfree(in->word);

	if (in->child)
	{
		if (in->valnode)
		{
			if (in->valnode->type == OPR && in->nchild > 0)
			{
				int			i;

				for (i = 0; i < in->nchild; i++)
					QTNFree(in->child[i]);
			}
			if (in->flags & QTN_NEEDFREE)
				pfree(in->valnode);
		}
		pfree(in->child);
	}

	pfree(in);
}

int
QTNodeCompare(QTNode * an, QTNode * bn)
{
	if (an->valnode->type != bn->valnode->type)
		return (an->valnode->type > bn->valnode->type) ? -1 : 1;
	else if (an->valnode->val != bn->valnode->val)
		return (an->valnode->val > bn->valnode->val) ? -1 : 1;
	else if (an->valnode->type == VAL)
	{
		if (an->valnode->length == bn->valnode->length)
			return strncmp(an->word, bn->word, an->valnode->length);
		else
			return (an->valnode->length > bn->valnode->length) ? -1 : 1;
	}
	else if (an->nchild != bn->nchild)
	{
		return (an->nchild > bn->nchild) ? -1 : 1;
	}
	else
	{
		int			i,
					res;

		for (i = 0; i < an->nchild; i++)
			if ((res = QTNodeCompare(an->child[i], bn->child[i])) != 0)
				return res;
	}

	return 0;
}

static int
cmpQTN(const void *a, const void *b)
{
	return QTNodeCompare(*(QTNode **) a, *(QTNode **) b);
}

void
QTNSort(QTNode * in)
{
	int			i;

	if (in->valnode->type != OPR)
		return;

	for (i = 0; i < in->nchild; i++)
		QTNSort(in->child[i]);
	if (in->nchild > 1)
		qsort((void *) in->child, in->nchild, sizeof(QTNode *), cmpQTN);
}

bool
QTNEq(QTNode * a, QTNode * b)
{
	uint32		sign = a->sign & b->sign;

	if (!(sign == a->sign && sign == b->sign))
		return 0;

	return (QTNodeCompare(a, b) == 0) ? true : false;
}

void
QTNTernary(QTNode * in)
{
	int			i;

	if (in->valnode->type != OPR)
		return;

	for (i = 0; i < in->nchild; i++)
		QTNTernary(in->child[i]);

	for (i = 0; i < in->nchild; i++)
	{
		if (in->valnode->type == in->child[i]->valnode->type && in->valnode->val == in->child[i]->valnode->val)
		{
			QTNode	   *cc = in->child[i];
			int			oldnchild = in->nchild;

			in->nchild += cc->nchild - 1;
			in->child = (QTNode **) repalloc(in->child, in->nchild * sizeof(QTNode *));

			if (i + 1 != oldnchild)
				memmove(in->child + i + cc->nchild, in->child + i + 1,
						(oldnchild - i - 1) * sizeof(QTNode *));

			memcpy(in->child + i, cc->child, cc->nchild * sizeof(QTNode *));
			i += cc->nchild - 1;

			pfree(cc);
		}
	}
}

void
QTNBinary(QTNode * in)
{
	int			i;

	if (in->valnode->type != OPR)
		return;

	for (i = 0; i < in->nchild; i++)
		QTNBinary(in->child[i]);

	if (in->nchild <= 2)
		return;

	while (in->nchild > 2)
	{
		QTNode	   *nn = (QTNode *) palloc0(sizeof(QTNode));

		nn->valnode = (ITEM *) palloc0(sizeof(ITEM));
		nn->child = (QTNode **) palloc0(sizeof(QTNode *) * 2);

		nn->nchild = 2;
		nn->flags = QTN_NEEDFREE;

		nn->child[0] = in->child[0];
		nn->child[1] = in->child[1];
		nn->sign = nn->child[0]->sign | nn->child[1]->sign;

		nn->valnode->type = in->valnode->type;
		nn->valnode->val = in->valnode->val;

		in->child[0] = nn;
		in->child[1] = in->child[in->nchild - 1];
		in->nchild--;
	}
}

static void
cntsize(QTNode * in, int4 *sumlen, int4 *nnode)
{
	*nnode += 1;
	if (in->valnode->type == OPR)
	{
		int			i;

		for (i = 0; i < in->nchild; i++)
			cntsize(in->child[i], sumlen, nnode);
	}
	else
	{
		*sumlen += in->valnode->length + 1;
	}
}

typedef struct
{
	ITEM	   *curitem;
	char	   *operand;
	char	   *curoperand;
}	QTN2QTState;

static void
fillQT(QTN2QTState * state, QTNode * in)
{
	*(state->curitem) = *(in->valnode);

	if (in->valnode->type == VAL)
	{
		memcpy(state->curoperand, in->word, in->valnode->length);
		state->curitem->distance = state->curoperand - state->operand;
		state->curoperand[in->valnode->length] = '\0';
		state->curoperand += in->valnode->length + 1;
		state->curitem++;
	}
	else
	{
		ITEM	   *curitem = state->curitem;

		Assert(in->nchild <= 2);
		state->curitem++;

		fillQT(state, in->child[0]);

		if (in->nchild == 2)
		{
			curitem->left = state->curitem - curitem;
			fillQT(state, in->child[1]);
		}
	}
}

QUERYTYPE *
QTN2QT(QTNode * in, MemoryType memtype)
{
	QUERYTYPE  *out;
	int			len;
	int			sumlen = 0,
				nnode = 0;
	QTN2QTState state;

	cntsize(in, &sumlen, &nnode);
	len = COMPUTESIZE(nnode, sumlen);

	out = (QUERYTYPE *) MEMALLOC(memtype, len);
	out->len = len;
	out->size = nnode;

	state.curitem = GETQUERY(out);
	state.operand = state.curoperand = GETOPERAND(out);

	fillQT(&state, in);
	return out;
}

QTNode *
QTNCopy(QTNode * in, MemoryType memtype)
{
	QTNode	   *out = (QTNode *) MEMALLOC(memtype, sizeof(QTNode));

	*out = *in;
	out->valnode = (ITEM *) MEMALLOC(memtype, sizeof(ITEM));
	*(out->valnode) = *(in->valnode);
	out->flags |= QTN_NEEDFREE;

	if (in->valnode->type == VAL)
	{
		out->word = MEMALLOC(memtype, in->valnode->length + 1);
		memcpy(out->word, in->word, in->valnode->length);
		out->word[in->valnode->length] = '\0';
		out->flags |= QTN_WORDFREE;
	}
	else
	{
		int			i;

		out->child = (QTNode **) MEMALLOC(memtype, sizeof(QTNode *) * in->nchild);

		for (i = 0; i < in->nchild; i++)
			out->child[i] = QTNCopy(in->child[i], memtype);
	}

	return out;
}
