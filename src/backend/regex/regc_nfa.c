/*
 * NFA utilities.
 * This file is #included by regcomp.c.
 *
 * Copyright (c) 1998, 1999 Henry Spencer.  All rights reserved.
 *
 * Development of this software was funded, in part, by Cray Research Inc.,
 * UUNET Communications Services Inc., Sun Microsystems Inc., and Scriptics
 * Corporation, none of whom are responsible for the results.  The author
 * thanks all of them.
 *
 * Redistribution and use in source and binary forms -- with or without
 * modification -- are permitted for any purpose, provided that
 * redistributions in source form retain this entire copyright notice and
 * indicate the origin and nature of any modifications.
 *
 * I'd appreciate being given credit for this package in the documentation
 * of software which uses it, but that is not a requirement.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * HENRY SPENCER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * src/backend/regex/regc_nfa.c
 *
 *
 * One or two things that technically ought to be in here
 * are actually in color.c, thanks to some incestuous relationships in
 * the color chains.
 */

#define NISERR()	VISERR(nfa->v)
#define NERR(e)		VERR(nfa->v, (e))


/*
 * newnfa - set up an NFA
 */
static struct nfa *				/* the NFA, or NULL */
newnfa(struct vars * v,
	   struct colormap * cm,
	   struct nfa * parent)		/* NULL if primary NFA */
{
	struct nfa *nfa;

	nfa = (struct nfa *) MALLOC(sizeof(struct nfa));
	if (nfa == NULL)
	{
		ERR(REG_ESPACE);
		return NULL;
	}

	nfa->states = NULL;
	nfa->slast = NULL;
	nfa->free = NULL;
	nfa->nstates = 0;
	nfa->cm = cm;
	nfa->v = v;
	nfa->size = 0;
	nfa->bos[0] = nfa->bos[1] = COLORLESS;
	nfa->eos[0] = nfa->eos[1] = COLORLESS;
	nfa->parent = parent;		/* Precedes newfstate so parent is valid. */
	nfa->post = newfstate(nfa, '@');	/* number 0 */
	nfa->pre = newfstate(nfa, '>');		/* number 1 */

	nfa->init = newstate(nfa);	/* may become invalid later */
	nfa->final = newstate(nfa);
	if (ISERR())
	{
		freenfa(nfa);
		return NULL;
	}
	rainbow(nfa, nfa->cm, PLAIN, COLORLESS, nfa->pre, nfa->init);
	newarc(nfa, '^', 1, nfa->pre, nfa->init);
	newarc(nfa, '^', 0, nfa->pre, nfa->init);
	rainbow(nfa, nfa->cm, PLAIN, COLORLESS, nfa->final, nfa->post);
	newarc(nfa, '$', 1, nfa->final, nfa->post);
	newarc(nfa, '$', 0, nfa->final, nfa->post);

	if (ISERR())
	{
		freenfa(nfa);
		return NULL;
	}
	return nfa;
}

/*
 * TooManyStates - checks if the max states exceeds the compile-time value
 */
static int
TooManyStates(struct nfa * nfa)
{
	struct nfa *parent = nfa->parent;
	size_t		sz = nfa->size;

	while (parent != NULL)
	{
		sz = parent->size;
		parent = parent->parent;
	}
	if (sz > REG_MAX_STATES)
		return 1;
	return 0;
}

/*
 * IncrementSize - increases the tracked size of the NFA and its parents.
 */
static void
IncrementSize(struct nfa * nfa)
{
	struct nfa *parent = nfa->parent;

	nfa->size++;
	while (parent != NULL)
	{
		parent->size++;
		parent = parent->parent;
	}
}

/*
 * DecrementSize - decreases the tracked size of the NFA and its parents.
 */
static void
DecrementSize(struct nfa * nfa)
{
	struct nfa *parent = nfa->parent;

	nfa->size--;
	while (parent != NULL)
	{
		parent->size--;
		parent = parent->parent;
	}
}

/*
 * freenfa - free an entire NFA
 */
static void
freenfa(struct nfa * nfa)
{
	struct state *s;

	while ((s = nfa->states) != NULL)
	{
		s->nins = s->nouts = 0; /* don't worry about arcs */
		freestate(nfa, s);
	}
	while ((s = nfa->free) != NULL)
	{
		nfa->free = s->next;
		destroystate(nfa, s);
	}

	nfa->slast = NULL;
	nfa->nstates = -1;
	nfa->pre = NULL;
	nfa->post = NULL;
	FREE(nfa);
}

/*
 * newstate - allocate an NFA state, with zero flag value
 */
static struct state *			/* NULL on error */
newstate(struct nfa * nfa)
{
	struct state *s;

	/*
	 * This is a handy place to check for operation cancel during regex
	 * compilation, since no code path will go very long without making a new
	 * state or arc.
	 */
	if (CANCEL_REQUESTED(nfa->v->re))
	{
		NERR(REG_CANCEL);
		return NULL;
	}

	if (TooManyStates(nfa))
	{
		NERR(REG_ETOOBIG);
		return NULL;
	}

	if (nfa->free != NULL)
	{
		s = nfa->free;
		nfa->free = s->next;
	}
	else
	{
		s = (struct state *) MALLOC(sizeof(struct state));
		if (s == NULL)
		{
			NERR(REG_ESPACE);
			return NULL;
		}
		s->oas.next = NULL;
		s->free = NULL;
		s->noas = 0;
	}

	assert(nfa->nstates >= 0);
	s->no = nfa->nstates++;
	s->flag = 0;
	if (nfa->states == NULL)
		nfa->states = s;
	s->nins = 0;
	s->ins = NULL;
	s->nouts = 0;
	s->outs = NULL;
	s->tmp = NULL;
	s->next = NULL;
	if (nfa->slast != NULL)
	{
		assert(nfa->slast->next == NULL);
		nfa->slast->next = s;
	}
	s->prev = nfa->slast;
	nfa->slast = s;
	/* track the current size and the parent size */
	IncrementSize(nfa);
	return s;
}

/*
 * newfstate - allocate an NFA state with a specified flag value
 */
static struct state *			/* NULL on error */
newfstate(struct nfa * nfa, int flag)
{
	struct state *s;

	s = newstate(nfa);
	if (s != NULL)
		s->flag = (char) flag;
	return s;
}

/*
 * dropstate - delete a state's inarcs and outarcs and free it
 */
static void
dropstate(struct nfa * nfa,
		  struct state * s)
{
	struct arc *a;

	while ((a = s->ins) != NULL)
		freearc(nfa, a);
	while ((a = s->outs) != NULL)
		freearc(nfa, a);
	freestate(nfa, s);
}

/*
 * freestate - free a state, which has no in-arcs or out-arcs
 */
static void
freestate(struct nfa * nfa,
		  struct state * s)
{
	assert(s != NULL);
	assert(s->nins == 0 && s->nouts == 0);

	s->no = FREESTATE;
	s->flag = 0;
	if (s->next != NULL)
		s->next->prev = s->prev;
	else
	{
		assert(s == nfa->slast);
		nfa->slast = s->prev;
	}
	if (s->prev != NULL)
		s->prev->next = s->next;
	else
	{
		assert(s == nfa->states);
		nfa->states = s->next;
	}
	s->prev = NULL;
	s->next = nfa->free;		/* don't delete it, put it on the free list */
	nfa->free = s;
	DecrementSize(nfa);
}

/*
 * destroystate - really get rid of an already-freed state
 */
static void
destroystate(struct nfa * nfa,
			 struct state * s)
{
	struct arcbatch *ab;
	struct arcbatch *abnext;

	assert(s->no == FREESTATE);
	for (ab = s->oas.next; ab != NULL; ab = abnext)
	{
		abnext = ab->next;
		FREE(ab);
	}
	s->ins = NULL;
	s->outs = NULL;
	s->next = NULL;
	FREE(s);
}

/*
 * newarc - set up a new arc within an NFA
 */
static void
newarc(struct nfa * nfa,
	   int t,
	   pcolor co,
	   struct state * from,
	   struct state * to)
{
	struct arc *a;

	assert(from != NULL && to != NULL);

	/*
	 * This is a handy place to check for operation cancel during regex
	 * compilation, since no code path will go very long without making a new
	 * state or arc.
	 */
	if (CANCEL_REQUESTED(nfa->v->re))
	{
		NERR(REG_CANCEL);
		return;
	}

	/* check for duplicates */
	for (a = from->outs; a != NULL; a = a->outchain)
		if (a->to == to && a->co == co && a->type == t)
			return;

	a = allocarc(nfa, from);
	if (NISERR())
		return;
	assert(a != NULL);

	a->type = t;
	a->co = (color) co;
	a->to = to;
	a->from = from;

	/*
	 * Put the new arc on the beginning, not the end, of the chains. Not only
	 * is this easier, it has the very useful side effect that deleting the
	 * most-recently-added arc is the cheapest case rather than the most
	 * expensive one.
	 */
	a->inchain = to->ins;
	to->ins = a;
	a->outchain = from->outs;
	from->outs = a;

	from->nouts++;
	to->nins++;

	if (COLORED(a) && nfa->parent == NULL)
		colorchain(nfa->cm, a);
}

/*
 * allocarc - allocate a new out-arc within a state
 */
static struct arc *				/* NULL for failure */
allocarc(struct nfa * nfa,
		 struct state * s)
{
	struct arc *a;

	/* shortcut */
	if (s->free == NULL && s->noas < ABSIZE)
	{
		a = &s->oas.a[s->noas];
		s->noas++;
		return a;
	}

	/* if none at hand, get more */
	if (s->free == NULL)
	{
		struct arcbatch *newAb;
		int			i;

		newAb = (struct arcbatch *) MALLOC(sizeof(struct arcbatch));
		if (newAb == NULL)
		{
			NERR(REG_ESPACE);
			return NULL;
		}
		newAb->next = s->oas.next;
		s->oas.next = newAb;

		for (i = 0; i < ABSIZE; i++)
		{
			newAb->a[i].type = 0;
			newAb->a[i].freechain = &newAb->a[i + 1];
		}
		newAb->a[ABSIZE - 1].freechain = NULL;
		s->free = &newAb->a[0];
	}
	assert(s->free != NULL);

	a = s->free;
	s->free = a->freechain;
	return a;
}

/*
 * freearc - free an arc
 */
static void
freearc(struct nfa * nfa,
		struct arc * victim)
{
	struct state *from = victim->from;
	struct state *to = victim->to;
	struct arc *a;

	assert(victim->type != 0);

	/* take it off color chain if necessary */
	if (COLORED(victim) && nfa->parent == NULL)
		uncolorchain(nfa->cm, victim);

	/* take it off source's out-chain */
	assert(from != NULL);
	assert(from->outs != NULL);
	a = from->outs;
	if (a == victim)			/* simple case:  first in chain */
		from->outs = victim->outchain;
	else
	{
		for (; a != NULL && a->outchain != victim; a = a->outchain)
			continue;
		assert(a != NULL);
		a->outchain = victim->outchain;
	}
	from->nouts--;

	/* take it off target's in-chain */
	assert(to != NULL);
	assert(to->ins != NULL);
	a = to->ins;
	if (a == victim)			/* simple case:  first in chain */
		to->ins = victim->inchain;
	else
	{
		for (; a != NULL && a->inchain != victim; a = a->inchain)
			continue;
		assert(a != NULL);
		a->inchain = victim->inchain;
	}
	to->nins--;

	/* clean up and place on free list */
	victim->type = 0;
	victim->from = NULL;		/* precautions... */
	victim->to = NULL;
	victim->inchain = NULL;
	victim->outchain = NULL;
	victim->freechain = from->free;
	from->free = victim;
}

/*
 * hasnonemptyout - Does state have a non-EMPTY out arc?
 */
static int
hasnonemptyout(struct state * s)
{
	struct arc *a;

	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->type != EMPTY)
			return 1;
	}
	return 0;
}

/*
 * nonemptyouts - count non-EMPTY out arcs of a state
 */
static int
nonemptyouts(struct state * s)
{
	int			n = 0;
	struct arc *a;

	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->type != EMPTY)
			n++;
	}
	return n;
}

/*
 * nonemptyins - count non-EMPTY in arcs of a state
 */
static int
nonemptyins(struct state * s)
{
	int			n = 0;
	struct arc *a;

	for (a = s->ins; a != NULL; a = a->inchain)
	{
		if (a->type != EMPTY)
			n++;
	}
	return n;
}

/*
 * findarc - find arc, if any, from given source with given type and color
 * If there is more than one such arc, the result is random.
 */
static struct arc *
findarc(struct state * s,
		int type,
		pcolor co)
{
	struct arc *a;

	for (a = s->outs; a != NULL; a = a->outchain)
		if (a->type == type && a->co == co)
			return a;
	return NULL;
}

/*
 * cparc - allocate a new arc within an NFA, copying details from old one
 */
static void
cparc(struct nfa * nfa,
	  struct arc * oa,
	  struct state * from,
	  struct state * to)
{
	newarc(nfa, oa->type, oa->co, from, to);
}

/*
 * moveins - move all in arcs of a state to another state
 *
 * You might think this could be done better by just updating the
 * existing arcs, and you would be right if it weren't for the desire
 * for duplicate suppression, which makes it easier to just make new
 * ones to exploit the suppression built into newarc.
 */
static void
moveins(struct nfa * nfa,
		struct state * oldState,
		struct state * newState)
{
	struct arc *a;

	assert(oldState != newState);

	while ((a = oldState->ins) != NULL)
	{
		cparc(nfa, a, a->from, newState);
		freearc(nfa, a);
	}
	assert(oldState->nins == 0);
	assert(oldState->ins == NULL);
}

/*
 * copyins - copy in arcs of a state to another state
 *
 * Either all arcs, or only non-empty ones as determined by all value.
 */
static void
copyins(struct nfa * nfa,
		struct state * oldState,
		struct state * newState,
		int all)
{
	struct arc *a;

	assert(oldState != newState);

	for (a = oldState->ins; a != NULL; a = a->inchain)
	{
		if (all || a->type != EMPTY)
			cparc(nfa, a, a->from, newState);
	}
}

/*
 * moveouts - move all out arcs of a state to another state
 */
static void
moveouts(struct nfa * nfa,
		 struct state * oldState,
		 struct state * newState)
{
	struct arc *a;

	assert(oldState != newState);

	while ((a = oldState->outs) != NULL)
	{
		cparc(nfa, a, newState, a->to);
		freearc(nfa, a);
	}
}

/*
 * copyouts - copy out arcs of a state to another state
 *
 * Either all arcs, or only non-empty ones as determined by all value.
 */
static void
copyouts(struct nfa * nfa,
		 struct state * oldState,
		 struct state * newState,
		 int all)
{
	struct arc *a;

	assert(oldState != newState);

	for (a = oldState->outs; a != NULL; a = a->outchain)
	{
		if (all || a->type != EMPTY)
			cparc(nfa, a, newState, a->to);
	}
}

/*
 * cloneouts - copy out arcs of a state to another state pair, modifying type
 */
static void
cloneouts(struct nfa * nfa,
		  struct state * old,
		  struct state * from,
		  struct state * to,
		  int type)
{
	struct arc *a;

	assert(old != from);

	for (a = old->outs; a != NULL; a = a->outchain)
		newarc(nfa, type, a->co, from, to);
}

/*
 * delsub - delete a sub-NFA, updating subre pointers if necessary
 *
 * This uses a recursive traversal of the sub-NFA, marking already-seen
 * states using their tmp pointer.
 */
static void
delsub(struct nfa * nfa,
	   struct state * lp,		/* the sub-NFA goes from here... */
	   struct state * rp)		/* ...to here, *not* inclusive */
{
	assert(lp != rp);

	rp->tmp = rp;				/* mark end */

	deltraverse(nfa, lp, lp);
	if (NISERR())
		return;					/* asserts might not hold after failure */
	assert(lp->nouts == 0 && rp->nins == 0);	/* did the job */
	assert(lp->no != FREESTATE && rp->no != FREESTATE); /* no more */

	rp->tmp = NULL;				/* unmark end */
	lp->tmp = NULL;				/* and begin, marked by deltraverse */
}

/*
 * deltraverse - the recursive heart of delsub
 * This routine's basic job is to destroy all out-arcs of the state.
 */
static void
deltraverse(struct nfa * nfa,
			struct state * leftend,
			struct state * s)
{
	struct arc *a;
	struct state *to;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->nouts == 0)
		return;					/* nothing to do */
	if (s->tmp != NULL)
		return;					/* already in progress */

	s->tmp = s;					/* mark as in progress */

	while ((a = s->outs) != NULL)
	{
		to = a->to;
		deltraverse(nfa, leftend, to);
		if (NISERR())
			return;				/* asserts might not hold after failure */
		assert(to->nouts == 0 || to->tmp != NULL);
		freearc(nfa, a);
		if (to->nins == 0 && to->tmp == NULL)
		{
			assert(to->nouts == 0);
			freestate(nfa, to);
		}
	}

	assert(s->no != FREESTATE); /* we're still here */
	assert(s == leftend || s->nins != 0);		/* and still reachable */
	assert(s->nouts == 0);		/* but have no outarcs */

	s->tmp = NULL;				/* we're done here */
}

/*
 * dupnfa - duplicate sub-NFA
 *
 * Another recursive traversal, this time using tmp to point to duplicates
 * as well as mark already-seen states.  (You knew there was a reason why
 * it's a state pointer, didn't you? :-))
 */
static void
dupnfa(struct nfa * nfa,
	   struct state * start,	/* duplicate of subNFA starting here */
	   struct state * stop,		/* and stopping here */
	   struct state * from,		/* stringing duplicate from here */
	   struct state * to)		/* to here */
{
	if (start == stop)
	{
		newarc(nfa, EMPTY, 0, from, to);
		return;
	}

	stop->tmp = to;
	duptraverse(nfa, start, from);
	/* done, except for clearing out the tmp pointers */

	stop->tmp = NULL;
	cleartraverse(nfa, start);
}

/*
 * duptraverse - recursive heart of dupnfa
 */
static void
duptraverse(struct nfa * nfa,
			struct state * s,
			struct state * stmp)	/* s's duplicate, or NULL */
{
	struct arc *a;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp != NULL)
		return;					/* already done */

	s->tmp = (stmp == NULL) ? newstate(nfa) : stmp;
	if (s->tmp == NULL)
	{
		assert(NISERR());
		return;
	}

	for (a = s->outs; a != NULL && !NISERR(); a = a->outchain)
	{
		duptraverse(nfa, a->to, (struct state *) NULL);
		if (NISERR())
			break;
		assert(a->to->tmp != NULL);
		cparc(nfa, a, s->tmp, a->to->tmp);
	}
}

/*
 * cleartraverse - recursive cleanup for algorithms that leave tmp ptrs set
 */
static void
cleartraverse(struct nfa * nfa,
			  struct state * s)
{
	struct arc *a;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp == NULL)
		return;
	s->tmp = NULL;

	for (a = s->outs; a != NULL; a = a->outchain)
		cleartraverse(nfa, a->to);
}

/*
 * specialcolors - fill in special colors for an NFA
 */
static void
specialcolors(struct nfa * nfa)
{
	/* false colors for BOS, BOL, EOS, EOL */
	if (nfa->parent == NULL)
	{
		nfa->bos[0] = pseudocolor(nfa->cm);
		nfa->bos[1] = pseudocolor(nfa->cm);
		nfa->eos[0] = pseudocolor(nfa->cm);
		nfa->eos[1] = pseudocolor(nfa->cm);
	}
	else
	{
		assert(nfa->parent->bos[0] != COLORLESS);
		nfa->bos[0] = nfa->parent->bos[0];
		assert(nfa->parent->bos[1] != COLORLESS);
		nfa->bos[1] = nfa->parent->bos[1];
		assert(nfa->parent->eos[0] != COLORLESS);
		nfa->eos[0] = nfa->parent->eos[0];
		assert(nfa->parent->eos[1] != COLORLESS);
		nfa->eos[1] = nfa->parent->eos[1];
	}
}

/*
 * optimize - optimize an NFA
 *
 * The main goal of this function is not so much "optimization" (though it
 * does try to get rid of useless NFA states) as reducing the NFA to a form
 * the regex executor can handle.  The executor, and indeed the cNFA format
 * that is its input, can only handle PLAIN and LACON arcs.  The output of
 * the regex parser also includes EMPTY (do-nothing) arcs, as well as
 * ^, $, AHEAD, and BEHIND constraint arcs, which we must get rid of here.
 * We first get rid of EMPTY arcs and then deal with the constraint arcs.
 * The hardest part of either job is to get rid of circular loops of the
 * target arc type.  We would have to do that in any case, though, as such a
 * loop would otherwise allow the executor to cycle through the loop endlessly
 * without making any progress in the input string.
 */
static long						/* re_info bits */
optimize(struct nfa * nfa,
		 FILE *f)				/* for debug output; NULL none */
{
#ifdef REG_DEBUG
	int			verbose = (f != NULL) ? 1 : 0;

	if (verbose)
		fprintf(f, "\ninitial cleanup:\n");
#endif
	cleanup(nfa);				/* may simplify situation */
#ifdef REG_DEBUG
	if (verbose)
		dumpnfa(nfa, f);
	if (verbose)
		fprintf(f, "\nempties:\n");
#endif
	fixempties(nfa, f);			/* get rid of EMPTY arcs */
#ifdef REG_DEBUG
	if (verbose)
		fprintf(f, "\nconstraints:\n");
#endif
	fixconstraintloops(nfa, f); /* get rid of constraint loops */
	pullback(nfa, f);			/* pull back constraints backward */
	pushfwd(nfa, f);			/* push fwd constraints forward */
#ifdef REG_DEBUG
	if (verbose)
		fprintf(f, "\nfinal cleanup:\n");
#endif
	cleanup(nfa);				/* final tidying */
	return analyze(nfa);		/* and analysis */
}

/*
 * pullback - pull back constraints backward to eliminate them
 */
static void
pullback(struct nfa * nfa,
		 FILE *f)				/* for debug output; NULL none */
{
	struct state *s;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	int			progress;

	/* find and pull until there are no more */
	do
	{
		progress = 0;
		for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
		{
			nexts = s->next;
			for (a = s->outs; a != NULL && !NISERR(); a = nexta)
			{
				nexta = a->outchain;
				if (a->type == '^' || a->type == BEHIND)
					if (pull(nfa, a))
						progress = 1;
				assert(nexta == NULL || s->no != FREESTATE);
			}
		}
		if (progress && f != NULL)
			dumpnfa(nfa, f);
	} while (progress && !NISERR());
	if (NISERR())
		return;

	/*
	 * Any ^ constraints we were able to pull to the start state can now be
	 * replaced by PLAIN arcs referencing the BOS or BOL colors.  There should
	 * be no other ^ or BEHIND arcs left in the NFA, though we do not check
	 * that here (compact() will fail if so).
	 */
	for (a = nfa->pre->outs; a != NULL; a = nexta)
	{
		nexta = a->outchain;
		if (a->type == '^')
		{
			assert(a->co == 0 || a->co == 1);
			newarc(nfa, PLAIN, nfa->bos[a->co], a->from, a->to);
			freearc(nfa, a);
		}
	}
}

/*
 * pull - pull a back constraint backward past its source state
 * A significant property of this function is that it deletes at most
 * one state -- the constraint's from state -- and only if the constraint
 * was that state's last outarc.
 */
static int						/* 0 couldn't, 1 could */
pull(struct nfa * nfa,
	 struct arc * con)
{
	struct state *from = con->from;
	struct state *to = con->to;
	struct arc *a;
	struct arc *nexta;
	struct state *s;

	assert(from != to);			/* should have gotten rid of this earlier */
	if (from->flag)				/* can't pull back beyond start */
		return 0;
	if (from->nins == 0)
	{							/* unreachable */
		freearc(nfa, con);
		return 1;
	}

	/* first, clone from state if necessary to avoid other outarcs */
	if (from->nouts > 1)
	{
		s = newstate(nfa);
		if (NISERR())
			return 0;
		copyins(nfa, from, s, 1);		/* duplicate inarcs */
		cparc(nfa, con, s, to); /* move constraint arc */
		freearc(nfa, con);
		from = s;
		con = from->outs;
	}
	assert(from->nouts == 1);

	/* propagate the constraint into the from state's inarcs */
	for (a = from->ins; a != NULL; a = nexta)
	{
		nexta = a->inchain;
		switch (combine(con, a))
		{
			case INCOMPATIBLE:	/* destroy the arc */
				freearc(nfa, a);
				break;
			case SATISFIED:		/* no action needed */
				break;
			case COMPATIBLE:	/* swap the two arcs, more or less */
				s = newstate(nfa);
				if (NISERR())
					return 0;
				cparc(nfa, a, s, to);	/* anticipate move */
				cparc(nfa, con, a->from, s);
				if (NISERR())
					return 0;
				freearc(nfa, a);
				break;
			default:
				assert(NOTREACHED);
				break;
		}
	}

	/* remaining inarcs, if any, incorporate the constraint */
	moveins(nfa, from, to);
	dropstate(nfa, from);		/* will free the constraint */
	return 1;
}

/*
 * pushfwd - push forward constraints forward to eliminate them
 */
static void
pushfwd(struct nfa * nfa,
		FILE *f)				/* for debug output; NULL none */
{
	struct state *s;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	int			progress;

	/* find and push until there are no more */
	do
	{
		progress = 0;
		for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
		{
			nexts = s->next;
			for (a = s->ins; a != NULL && !NISERR(); a = nexta)
			{
				nexta = a->inchain;
				if (a->type == '$' || a->type == AHEAD)
					if (push(nfa, a))
						progress = 1;
				assert(nexta == NULL || s->no != FREESTATE);
			}
		}
		if (progress && f != NULL)
			dumpnfa(nfa, f);
	} while (progress && !NISERR());
	if (NISERR())
		return;

	/*
	 * Any $ constraints we were able to push to the post state can now be
	 * replaced by PLAIN arcs referencing the EOS or EOL colors.  There should
	 * be no other $ or AHEAD arcs left in the NFA, though we do not check
	 * that here (compact() will fail if so).
	 */
	for (a = nfa->post->ins; a != NULL; a = nexta)
	{
		nexta = a->inchain;
		if (a->type == '$')
		{
			assert(a->co == 0 || a->co == 1);
			newarc(nfa, PLAIN, nfa->eos[a->co], a->from, a->to);
			freearc(nfa, a);
		}
	}
}

/*
 * push - push a forward constraint forward past its destination state
 * A significant property of this function is that it deletes at most
 * one state -- the constraint's to state -- and only if the constraint
 * was that state's last inarc.
 */
static int						/* 0 couldn't, 1 could */
push(struct nfa * nfa,
	 struct arc * con)
{
	struct state *from = con->from;
	struct state *to = con->to;
	struct arc *a;
	struct arc *nexta;
	struct state *s;

	assert(to != from);			/* should have gotten rid of this earlier */
	if (to->flag)				/* can't push forward beyond end */
		return 0;
	if (to->nouts == 0)
	{							/* dead end */
		freearc(nfa, con);
		return 1;
	}

	/* first, clone to state if necessary to avoid other inarcs */
	if (to->nins > 1)
	{
		s = newstate(nfa);
		if (NISERR())
			return 0;
		copyouts(nfa, to, s, 1);	/* duplicate outarcs */
		cparc(nfa, con, from, s);		/* move constraint */
		freearc(nfa, con);
		to = s;
		con = to->ins;
	}
	assert(to->nins == 1);

	/* propagate the constraint into the to state's outarcs */
	for (a = to->outs; a != NULL; a = nexta)
	{
		nexta = a->outchain;
		switch (combine(con, a))
		{
			case INCOMPATIBLE:	/* destroy the arc */
				freearc(nfa, a);
				break;
			case SATISFIED:		/* no action needed */
				break;
			case COMPATIBLE:	/* swap the two arcs, more or less */
				s = newstate(nfa);
				if (NISERR())
					return 0;
				cparc(nfa, con, s, a->to);		/* anticipate move */
				cparc(nfa, a, from, s);
				if (NISERR())
					return 0;
				freearc(nfa, a);
				break;
			default:
				assert(NOTREACHED);
				break;
		}
	}

	/* remaining outarcs, if any, incorporate the constraint */
	moveouts(nfa, to, from);
	dropstate(nfa, to);			/* will free the constraint */
	return 1;
}

/*
 * combine - constraint lands on an arc, what happens?
 *
 * #def INCOMPATIBLE	1	// destroys arc
 * #def SATISFIED		2	// constraint satisfied
 * #def COMPATIBLE		3	// compatible but not satisfied yet
 */
static int
combine(struct arc * con,
		struct arc * a)
{
#define  CA(ct,at)	 (((ct)<<CHAR_BIT) | (at))

	switch (CA(con->type, a->type))
	{
		case CA('^', PLAIN):	/* newlines are handled separately */
		case CA('$', PLAIN):
			return INCOMPATIBLE;
			break;
		case CA(AHEAD, PLAIN):	/* color constraints meet colors */
		case CA(BEHIND, PLAIN):
			if (con->co == a->co)
				return SATISFIED;
			return INCOMPATIBLE;
			break;
		case CA('^', '^'):		/* collision, similar constraints */
		case CA('$', '$'):
		case CA(AHEAD, AHEAD):
		case CA(BEHIND, BEHIND):
			if (con->co == a->co)		/* true duplication */
				return SATISFIED;
			return INCOMPATIBLE;
			break;
		case CA('^', BEHIND):	/* collision, dissimilar constraints */
		case CA(BEHIND, '^'):
		case CA('$', AHEAD):
		case CA(AHEAD, '$'):
			return INCOMPATIBLE;
			break;
		case CA('^', '$'):		/* constraints passing each other */
		case CA('^', AHEAD):
		case CA(BEHIND, '$'):
		case CA(BEHIND, AHEAD):
		case CA('$', '^'):
		case CA('$', BEHIND):
		case CA(AHEAD, '^'):
		case CA(AHEAD, BEHIND):
		case CA('^', LACON):
		case CA(BEHIND, LACON):
		case CA('$', LACON):
		case CA(AHEAD, LACON):
			return COMPATIBLE;
			break;
	}
	assert(NOTREACHED);
	return INCOMPATIBLE;		/* for benefit of blind compilers */
}

/*
 * fixempties - get rid of EMPTY arcs
 */
static void
fixempties(struct nfa * nfa,
		   FILE *f)				/* for debug output; NULL none */
{
	struct state *s;
	struct state *s2;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;

	/*
	 * First, get rid of any states whose sole out-arc is an EMPTY, since
	 * they're basically just aliases for their successor.  The parsing
	 * algorithm creates enough of these that it's worth special-casing this.
	 */
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		if (s->flag || s->nouts != 1)
			continue;
		a = s->outs;
		assert(a != NULL && a->outchain == NULL);
		if (a->type != EMPTY)
			continue;
		if (s != a->to)
			moveins(nfa, s, a->to);
		dropstate(nfa, s);
	}

	/*
	 * Similarly, get rid of any state with a single EMPTY in-arc, by folding
	 * it into its predecessor.
	 */
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		/* while we're at it, ensure tmp fields are clear for next step */
		assert(s->tmp == NULL);
		if (s->flag || s->nins != 1)
			continue;
		a = s->ins;
		assert(a != NULL && a->inchain == NULL);
		if (a->type != EMPTY)
			continue;
		if (s != a->from)
			moveouts(nfa, s, a->from);
		dropstate(nfa, s);
	}

	/*
	 * For each remaining NFA state, find all other states that are reachable
	 * from it by a chain of one or more EMPTY arcs.  Then generate new arcs
	 * that eliminate the need for each such chain.
	 *
	 * If we just do this straightforwardly, the algorithm gets slow in
	 * complex graphs, because the same arcs get copied to all intermediate
	 * states of an EMPTY chain, and then uselessly pushed repeatedly to the
	 * chain's final state; we waste a lot of time in newarc's duplicate
	 * checking.  To improve matters, we decree that any state with only EMPTY
	 * out-arcs is "doomed" and will not be part of the final NFA. That can be
	 * ensured by not adding any new out-arcs to such a state. Having ensured
	 * that, we need not update the state's in-arcs list either; all arcs that
	 * might have gotten pushed forward to it will just get pushed directly to
	 * successor states.  This eliminates most of the useless duplicate arcs.
	 */
	for (s = nfa->states; s != NULL && !NISERR(); s = s->next)
	{
		for (s2 = emptyreachable(nfa, s, s); s2 != s && !NISERR(); s2 = nexts)
		{
			/*
			 * If s2 is doomed, we decide that (1) we will always push arcs
			 * forward to it, not pull them back to s; and (2) we can optimize
			 * away the push-forward, per comment above.  So do nothing.
			 */
			if (s2->flag || hasnonemptyout(s2))
				replaceempty(nfa, s, s2);

			/* Reset the tmp fields as we walk back */
			nexts = s2->tmp;
			s2->tmp = NULL;
		}
		s->tmp = NULL;
	}

	if (NISERR())
		return;

	/*
	 * Now remove all the EMPTY arcs, since we don't need them anymore.
	 */
	for (s = nfa->states; s != NULL; s = s->next)
	{
		for (a = s->outs; a != NULL; a = nexta)
		{
			nexta = a->outchain;
			if (a->type == EMPTY)
				freearc(nfa, a);
		}
	}

	/*
	 * And remove any states that have become useless.  (This cleanup is not
	 * very thorough, and would be even less so if we tried to combine it with
	 * the previous step; but cleanup() will take care of anything we miss.)
	 */
	for (s = nfa->states; s != NULL; s = nexts)
	{
		nexts = s->next;
		if ((s->nins == 0 || s->nouts == 0) && !s->flag)
			dropstate(nfa, s);
	}

	if (f != NULL)
		dumpnfa(nfa, f);
}

/*
 * emptyreachable - recursively find all states reachable from s by EMPTY arcs
 *
 * The return value is the last such state found.  Its tmp field links back
 * to the next-to-last such state, and so on back to s, so that all these
 * states can be located without searching the whole NFA.
 *
 * The maximum recursion depth here is equal to the length of the longest
 * loop-free chain of EMPTY arcs, which is surely no more than the size of
 * the NFA ... but that could still be enough to cause trouble.
 */
static struct state *
emptyreachable(struct nfa * nfa,
			   struct state * s,
			   struct state * lastfound)
{
	struct arc *a;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return lastfound;
	}

	s->tmp = lastfound;
	lastfound = s;
	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (a->type == EMPTY && a->to->tmp == NULL)
			lastfound = emptyreachable(nfa, a->to, lastfound);
	}
	return lastfound;
}

/*
 * replaceempty - replace an EMPTY arc chain with some non-empty arcs
 *
 * The EMPTY arc(s) should be deleted later, but we can't do it here because
 * they may still be needed to identify other arc chains during fixempties().
 */
static void
replaceempty(struct nfa * nfa,
			 struct state * from,
			 struct state * to)
{
	int			fromouts;
	int			toins;

	assert(from != to);

	/*
	 * Create replacement arcs that bypass the need for the EMPTY chain.  We
	 * can do this either by pushing arcs forward (linking directly from
	 * "from"'s predecessors to "to") or by pulling them back (linking
	 * directly from "from" to "to"'s successors).  In general, we choose
	 * whichever way creates greater fan-out or fan-in, so as to improve the
	 * odds of reducing the other state to zero in-arcs or out-arcs and
	 * thereby being able to delete it.  However, if "from" is doomed (has no
	 * non-EMPTY out-arcs), we must keep it so, so always push forward in that
	 * case.
	 *
	 * The fan-out/fan-in comparison should count only non-EMPTY arcs.  If
	 * "from" is doomed, we can skip counting "to"'s arcs, since we want to
	 * force taking the copyins path in that case.
	 */
	fromouts = nonemptyouts(from);
	toins = (fromouts == 0) ? 1 : nonemptyins(to);

	if (fromouts > toins)
	{
		copyouts(nfa, to, from, 0);
		return;
	}
	if (fromouts < toins)
	{
		copyins(nfa, from, to, 0);
		return;
	}

	/*
	 * fromouts == toins.  Decide on secondary issue: copy fewest arcs.
	 *
	 * Doesn't seem to be worth the trouble to exclude empties from these
	 * comparisons; that takes extra time and doesn't seem to improve the
	 * resulting graph much.
	 */
	if (from->nins > to->nouts)
	{
		copyouts(nfa, to, from, 0);
		return;
	}
	else
	{
		copyins(nfa, from, to, 0);
		return;
	}
}

/*
 * isconstraintarc - detect whether an arc is of a constraint type
 */
static inline int
isconstraintarc(struct arc * a)
{
	switch (a->type)
	{
		case '^':
		case '$':
		case BEHIND:
		case AHEAD:
		case LACON:
			return 1;
	}
	return 0;
}

/*
 * hasconstraintout - does state have a constraint out arc?
 */
static int
hasconstraintout(struct state * s)
{
	struct arc *a;

	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (isconstraintarc(a))
			return 1;
	}
	return 0;
}

/*
 * fixconstraintloops - get rid of loops containing only constraint arcs
 *
 * A loop of states that contains only constraint arcs is useless, since
 * passing around the loop represents no forward progress.  Moreover, it
 * would cause infinite looping in pullback/pushfwd, so we need to get rid
 * of such loops before doing that.
 */
static void
fixconstraintloops(struct nfa * nfa,
				   FILE *f)		/* for debug output; NULL none */
{
	struct state *s;
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	int			hasconstraints;

	/*
	 * In the trivial case of a state that loops to itself, we can just drop
	 * the constraint arc altogether.  This is worth special-casing because
	 * such loops are far more common than loops containing multiple states.
	 * While we're at it, note whether any constraint arcs survive.
	 */
	hasconstraints = 0;
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		/* while we're at it, ensure tmp fields are clear for next step */
		assert(s->tmp == NULL);
		for (a = s->outs; a != NULL && !NISERR(); a = nexta)
		{
			nexta = a->outchain;
			if (isconstraintarc(a))
			{
				if (a->to == s)
					freearc(nfa, a);
				else
					hasconstraints = 1;
			}
		}
		/* If we removed all the outarcs, the state is useless. */
		if (s->nouts == 0 && !s->flag)
			dropstate(nfa, s);
	}

	/* Nothing to do if no remaining constraint arcs */
	if (NISERR() || !hasconstraints)
		return;

	/*
	 * Starting from each remaining NFA state, search outwards for a
	 * constraint loop.  If we find a loop, break the loop, then start the
	 * search over.  (We could possibly retain some state from the first scan,
	 * but it would complicate things greatly, and multi-state constraint
	 * loops are rare enough that it's not worth optimizing the case.)
	 */
restart:
	for (s = nfa->states; s != NULL && !NISERR(); s = s->next)
	{
		if (findconstraintloop(nfa, s))
			goto restart;
	}

	if (NISERR())
		return;

	/*
	 * Now remove any states that have become useless.  (This cleanup is not
	 * very thorough, and would be even less so if we tried to combine it with
	 * the previous step; but cleanup() will take care of anything we miss.)
	 *
	 * Because findconstraintloop intentionally doesn't reset all tmp fields,
	 * we have to clear them after it's done.  This is a convenient place to
	 * do that, too.
	 */
	for (s = nfa->states; s != NULL; s = nexts)
	{
		nexts = s->next;
		s->tmp = NULL;
		if ((s->nins == 0 || s->nouts == 0) && !s->flag)
			dropstate(nfa, s);
	}

	if (f != NULL)
		dumpnfa(nfa, f);
}

/*
 * findconstraintloop - recursively find a loop of constraint arcs
 *
 * If we find a loop, break it by calling breakconstraintloop(), then
 * return 1; otherwise return 0.
 *
 * State tmp fields are guaranteed all NULL on a success return, because
 * breakconstraintloop does that.  After a failure return, any state that
 * is known not to be part of a loop is marked with s->tmp == s; this allows
 * us not to have to re-prove that fact on later calls.  (This convention is
 * workable because we already eliminated single-state loops.)
 *
 * Note that the found loop doesn't necessarily include the first state we
 * are called on.  Any loop reachable from that state will do.
 *
 * The maximum recursion depth here is one more than the length of the longest
 * loop-free chain of constraint arcs, which is surely no more than the size
 * of the NFA ... but that could still be enough to cause trouble.
 */
static int
findconstraintloop(struct nfa * nfa, struct state * s)
{
	struct arc *a;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return 1;				/* to exit as quickly as possible */
	}

	if (s->tmp != NULL)
	{
		/* Already proven uninteresting? */
		if (s->tmp == s)
			return 0;
		/* Found a loop involving s */
		breakconstraintloop(nfa, s);
		/* The tmp fields have been cleaned up by breakconstraintloop */
		return 1;
	}
	for (a = s->outs; a != NULL; a = a->outchain)
	{
		if (isconstraintarc(a))
		{
			struct state *sto = a->to;

			assert(sto != s);
			s->tmp = sto;
			if (findconstraintloop(nfa, sto))
				return 1;
		}
	}

	/*
	 * If we get here, no constraint loop exists leading out from s.  Mark it
	 * with s->tmp == s so we need not rediscover that fact again later.
	 */
	s->tmp = s;
	return 0;
}

/*
 * breakconstraintloop - break a loop of constraint arcs
 *
 * sinitial is any one member state of the loop.  Each loop member's tmp
 * field links to its successor within the loop.  (Note that this function
 * will reset all the tmp fields to NULL.)
 *
 * We can break the loop by, for any one state S1 in the loop, cloning its
 * loop successor state S2 (and possibly following states), and then moving
 * all S1->S2 constraint arcs to point to the cloned S2.  The cloned S2 should
 * copy any non-constraint outarcs of S2.  Constraint outarcs should be
 * dropped if they point back to S1, else they need to be copied as arcs to
 * similarly cloned states S3, S4, etc.  In general, each cloned state copies
 * non-constraint outarcs, drops constraint outarcs that would lead to itself
 * or any earlier cloned state, and sends other constraint outarcs to newly
 * cloned states.  No cloned state will have any inarcs that aren't constraint
 * arcs or do not lead from S1 or earlier-cloned states.  It's okay to drop
 * constraint back-arcs since they would not take us to any state we've not
 * already been in; therefore, no new constraint loop is created.  In this way
 * we generate a modified NFA that can still represent every useful state
 * sequence, but not sequences that represent state loops with no consumption
 * of input data.  Note that the set of cloned states will certainly include
 * all of the loop member states other than S1, and it may also include
 * non-loop states that are reachable from S2 via constraint arcs.  This is
 * important because there is no guarantee that findconstraintloop found a
 * maximal loop (and searching for one would be NP-hard, so don't try).
 * Frequently the "non-loop states" are actually part of a larger loop that
 * we didn't notice, and indeed there may be several overlapping loops.
 * This technique ensures convergence in such cases, while considering only
 * the originally-found loop does not.
 *
 * If there is only one S1->S2 constraint arc, then that constraint is
 * certainly satisfied when we enter any of the clone states.  This means that
 * in the common case where many of the constraint arcs are identically
 * labeled, we can merge together clone states linked by a similarly-labeled
 * constraint: if we can get to the first one we can certainly get to the
 * second, so there's no need to distinguish.  This greatly reduces the number
 * of new states needed, so we preferentially break the given loop at a state
 * pair where this is true.
 *
 * Furthermore, it's fairly common to find that a cloned successor state has
 * no outarcs, especially if we're a bit aggressive about removing unnecessary
 * outarcs.  If that happens, then there is simply not any interesting state
 * that can be reached through the predecessor's loop arcs, which means we can
 * break the loop just by removing those loop arcs, with no new states added.
 */
static void
breakconstraintloop(struct nfa * nfa, struct state * sinitial)
{
	struct state *s;
	struct state *shead;
	struct state *stail;
	struct state *sclone;
	struct state *nexts;
	struct arc *refarc;
	struct arc *a;
	struct arc *nexta;

	/*
	 * Start by identifying which loop step we want to break at.
	 * Preferentially this is one with only one constraint arc.  (XXX are
	 * there any other secondary heuristics we want to use here?)  Set refarc
	 * to point to the selected lone constraint arc, if there is one.
	 */
	refarc = NULL;
	s = sinitial;
	do
	{
		nexts = s->tmp;
		assert(nexts != s);		/* should not see any one-element loops */
		if (refarc == NULL)
		{
			int			narcs = 0;

			for (a = s->outs; a != NULL; a = a->outchain)
			{
				if (a->to == nexts && isconstraintarc(a))
				{
					refarc = a;
					narcs++;
				}
			}
			assert(narcs > 0);
			if (narcs > 1)
				refarc = NULL;	/* multiple constraint arcs here, no good */
		}
		s = nexts;
	} while (s != sinitial);

	if (refarc)
	{
		/* break at the refarc */
		shead = refarc->from;
		stail = refarc->to;
		assert(stail == shead->tmp);
	}
	else
	{
		/* for lack of a better idea, break after sinitial */
		shead = sinitial;
		stail = sinitial->tmp;
	}

	/*
	 * Reset the tmp fields so that we can use them for local storage in
	 * clonesuccessorstates.  (findconstraintloop won't mind, since it's just
	 * going to abandon its search anyway.)
	 */
	for (s = nfa->states; s != NULL; s = s->next)
		s->tmp = NULL;

	/*
	 * Recursively build clone state(s) as needed.
	 */
	sclone = newstate(nfa);
	if (sclone == NULL)
	{
		assert(NISERR());
		return;
	}

	clonesuccessorstates(nfa, stail, sclone, shead, refarc,
						 NULL, NULL, nfa->nstates);

	if (NISERR())
		return;

	/*
	 * It's possible that sclone has no outarcs at all, in which case it's
	 * useless.  (We don't try extremely hard to get rid of useless states
	 * here, but this is an easy and fairly common case.)
	 */
	if (sclone->nouts == 0)
	{
		freestate(nfa, sclone);
		sclone = NULL;
	}

	/*
	 * Move shead's constraint-loop arcs to point to sclone, or just drop them
	 * if we discovered we don't need sclone.
	 */
	for (a = shead->outs; a != NULL; a = nexta)
	{
		nexta = a->outchain;
		if (a->to == stail && isconstraintarc(a))
		{
			if (sclone)
				cparc(nfa, a, shead, sclone);
			freearc(nfa, a);
			if (NISERR())
				break;
		}
	}
}

/*
 * clonesuccessorstates - create a tree of constraint-arc successor states
 *
 * ssource is the state to be cloned, and sclone is the state to copy its
 * outarcs into.  sclone's inarcs, if any, should already be set up.
 *
 * spredecessor is the original predecessor state that we are trying to build
 * successors for (it may not be the immediate predecessor of ssource).
 * refarc, if not NULL, is the original constraint arc that is known to have
 * been traversed out of spredecessor to reach the successor(s).
 *
 * For each cloned successor state, we transiently create a "donemap" that is
 * a boolean array showing which source states we've already visited for this
 * clone state.  This prevents infinite recursion as well as useless repeat
 * visits to the same state subtree (which can add up fast, since typical NFAs
 * have multiple redundant arc pathways).  Each donemap is a char array
 * indexed by state number.  The donemaps are all of the same size "nstates",
 * which is nfa->nstates as of the start of the recursion.  This is enough to
 * have entries for all pre-existing states, but *not* entries for clone
 * states created during the recursion.  That's okay since we have no need to
 * mark those.
 *
 * curdonemap is NULL when recursing to a new sclone state, or sclone's
 * donemap when we are recursing without having created a new state (which we
 * do when we decide we can merge a successor state into the current clone
 * state).  outerdonemap is NULL at the top level and otherwise the parent
 * clone state's donemap.
 *
 * The successor states we create and fill here form a strict tree structure,
 * with each state having exactly one predecessor, except that the toplevel
 * state has no inarcs as yet (breakconstraintloop will add its inarcs from
 * spredecessor after we're done).  Thus, we can examine sclone's inarcs back
 * to the root, plus refarc if any, to identify the set of constraints already
 * known valid at the current point.  This allows us to avoid generating extra
 * successor states.
 */
static void
clonesuccessorstates(struct nfa * nfa,
					 struct state * ssource,
					 struct state * sclone,
					 struct state * spredecessor,
					 struct arc * refarc,
					 char *curdonemap,
					 char *outerdonemap,
					 int nstates)
{
	char	   *donemap;
	struct arc *a;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	/* If this state hasn't already got a donemap, create one */
	donemap = curdonemap;
	if (donemap == NULL)
	{
		donemap = (char *) MALLOC(nstates * sizeof(char));
		if (donemap == NULL)
		{
			NERR(REG_ESPACE);
			return;
		}

		if (outerdonemap != NULL)
		{
			/*
			 * Not at outermost recursion level, so copy the outer level's
			 * donemap; this ensures that we see states in process of being
			 * visited at outer levels, or already merged into predecessor
			 * states, as ones we shouldn't traverse back to.
			 */
			memcpy(donemap, outerdonemap, nstates * sizeof(char));
		}
		else
		{
			/* At outermost level, only spredecessor is off-limits */
			memset(donemap, 0, nstates * sizeof(char));
			assert(spredecessor->no < nstates);
			donemap[spredecessor->no] = 1;
		}
	}

	/* Mark ssource as visited in the donemap */
	assert(ssource->no < nstates);
	assert(donemap[ssource->no] == 0);
	donemap[ssource->no] = 1;

	/*
	 * We proceed by first cloning all of ssource's outarcs, creating new
	 * clone states as needed but not doing more with them than that.  Then in
	 * a second pass, recurse to process the child clone states.  This allows
	 * us to have only one child clone state per reachable source state, even
	 * when there are multiple outarcs leading to the same state.  Also, when
	 * we do visit a child state, its set of inarcs is known exactly, which
	 * makes it safe to apply the constraint-is-already-checked optimization.
	 * Also, this ensures that we've merged all the states we can into the
	 * current clone before we recurse to any children, thus possibly saving
	 * them from making extra images of those states.
	 *
	 * While this function runs, child clone states of the current state are
	 * marked by setting their tmp fields to point to the original state they
	 * were cloned from.  This makes it possible to detect multiple outarcs
	 * leading to the same state, and also makes it easy to distinguish clone
	 * states from original states (which will have tmp == NULL).
	 */
	for (a = ssource->outs; a != NULL && !NISERR(); a = a->outchain)
	{
		struct state *sto = a->to;

		/*
		 * We do not consider cloning successor states that have no constraint
		 * outarcs; just link to them as-is.  They cannot be part of a
		 * constraint loop so there is no need to make copies.  In particular,
		 * this rule keeps us from trying to clone the post state, which would
		 * be a bad idea.
		 */
		if (isconstraintarc(a) && hasconstraintout(sto))
		{
			struct state *prevclone;
			int			canmerge;
			struct arc *a2;

			/*
			 * Back-link constraint arcs must not be followed.  Nor is there a
			 * need to revisit states previously merged into this clone.
			 */
			assert(sto->no < nstates);
			if (donemap[sto->no] != 0)
				continue;

			/*
			 * Check whether we already have a child clone state for this
			 * source state.
			 */
			prevclone = NULL;
			for (a2 = sclone->outs; a2 != NULL; a2 = a2->outchain)
			{
				if (a2->to->tmp == sto)
				{
					prevclone = a2->to;
					break;
				}
			}

			/*
			 * If this arc is labeled the same as refarc, or the same as any
			 * arc we must have traversed to get to sclone, then no additional
			 * constraints need to be met to get to sto, so we should just
			 * merge its outarcs into sclone.
			 */
			if (refarc && a->type == refarc->type && a->co == refarc->co)
				canmerge = 1;
			else
			{
				struct state *s;

				canmerge = 0;
				for (s = sclone; s->ins; s = s->ins->from)
				{
					if (s->nins == 1 &&
						a->type == s->ins->type && a->co == s->ins->co)
					{
						canmerge = 1;
						break;
					}
				}
			}

			if (canmerge)
			{
				/*
				 * We can merge into sclone.  If we previously made a child
				 * clone state, drop it; there's no need to visit it.  (This
				 * can happen if ssource has multiple pathways to sto, and we
				 * only just now found one that is provably a no-op.)
				 */
				if (prevclone)
					dropstate(nfa, prevclone);	/* kills our outarc, too */

				/* Recurse to merge sto's outarcs into sclone */
				clonesuccessorstates(nfa,
									 sto,
									 sclone,
									 spredecessor,
									 refarc,
									 donemap,
									 outerdonemap,
									 nstates);
				/* sto should now be marked as previously visited */
				assert(NISERR() || donemap[sto->no] == 1);
			}
			else if (prevclone)
			{
				/*
				 * We already have a clone state for this successor, so just
				 * make another arc to it.
				 */
				cparc(nfa, a, sclone, prevclone);
			}
			else
			{
				/*
				 * We need to create a new successor clone state.
				 */
				struct state *stoclone;

				stoclone = newstate(nfa);
				if (stoclone == NULL)
				{
					assert(NISERR());
					break;
				}
				/* Mark it as to what it's a clone of */
				stoclone->tmp = sto;
				/* ... and add the outarc leading to it */
				cparc(nfa, a, sclone, stoclone);
			}
		}
		else
		{
			/*
			 * Non-constraint outarcs just get copied to sclone, as do outarcs
			 * leading to states with no constraint outarc.
			 */
			cparc(nfa, a, sclone, sto);
		}
	}

	/*
	 * If we are at outer level for this clone state, recurse to all its child
	 * clone states, clearing their tmp fields as we go.  (If we're not
	 * outermost for sclone, leave this to be done by the outer call level.)
	 * Note that if we have multiple outarcs leading to the same clone state,
	 * it will only be recursed-to once.
	 */
	if (curdonemap == NULL)
	{
		for (a = sclone->outs; a != NULL && !NISERR(); a = a->outchain)
		{
			struct state *stoclone = a->to;
			struct state *sto = stoclone->tmp;

			if (sto != NULL)
			{
				stoclone->tmp = NULL;
				clonesuccessorstates(nfa,
									 sto,
									 stoclone,
									 spredecessor,
									 refarc,
									 NULL,
									 donemap,
									 nstates);
			}
		}

		/* Don't forget to free sclone's donemap when done with it */
		FREE(donemap);
	}
}

/*
 * cleanup - clean up NFA after optimizations
 */
static void
cleanup(struct nfa * nfa)
{
	struct state *s;
	struct state *nexts;
	int			n;

	if (NISERR())
		return;

	/* clear out unreachable or dead-end states */
	/* use pre to mark reachable, then post to mark can-reach-post */
	markreachable(nfa, nfa->pre, (struct state *) NULL, nfa->pre);
	markcanreach(nfa, nfa->post, nfa->pre, nfa->post);
	for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
	{
		nexts = s->next;
		if (s->tmp != nfa->post && !s->flag)
			dropstate(nfa, s);
	}
	assert(NISERR() || nfa->post->nins == 0 || nfa->post->tmp == nfa->post);
	cleartraverse(nfa, nfa->pre);
	assert(NISERR() || nfa->post->nins == 0 || nfa->post->tmp == NULL);
	/* the nins==0 (final unreachable) case will be caught later */

	/* renumber surviving states */
	n = 0;
	for (s = nfa->states; s != NULL; s = s->next)
		s->no = n++;
	nfa->nstates = n;
}

/*
 * markreachable - recursive marking of reachable states
 */
static void
markreachable(struct nfa * nfa,
			  struct state * s,
			  struct state * okay,		/* consider only states with this mark */
			  struct state * mark)		/* the value to mark with */
{
	struct arc *a;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp != okay)
		return;
	s->tmp = mark;

	for (a = s->outs; a != NULL; a = a->outchain)
		markreachable(nfa, a->to, okay, mark);
}

/*
 * markcanreach - recursive marking of states which can reach here
 */
static void
markcanreach(struct nfa * nfa,
			 struct state * s,
			 struct state * okay,		/* consider only states with this mark */
			 struct state * mark)		/* the value to mark with */
{
	struct arc *a;

	/* Since this is recursive, it could be driven to stack overflow */
	if (STACK_TOO_DEEP(nfa->v->re))
	{
		NERR(REG_ETOOBIG);
		return;
	}

	if (s->tmp != okay)
		return;
	s->tmp = mark;

	for (a = s->ins; a != NULL; a = a->inchain)
		markcanreach(nfa, a->from, okay, mark);
}

/*
 * analyze - ascertain potentially-useful facts about an optimized NFA
 */
static long						/* re_info bits to be ORed in */
analyze(struct nfa * nfa)
{
	struct arc *a;
	struct arc *aa;

	if (NISERR())
		return 0;

	if (nfa->pre->outs == NULL)
		return REG_UIMPOSSIBLE;
	for (a = nfa->pre->outs; a != NULL; a = a->outchain)
		for (aa = a->to->outs; aa != NULL; aa = aa->outchain)
			if (aa->to == nfa->post)
				return REG_UEMPTYMATCH;
	return 0;
}

/*
 * compact - construct the compact representation of an NFA
 */
static void
compact(struct nfa * nfa,
		struct cnfa * cnfa)
{
	struct state *s;
	struct arc *a;
	size_t		nstates;
	size_t		narcs;
	struct carc *ca;
	struct carc *first;

	assert(!NISERR());

	nstates = 0;
	narcs = 0;
	for (s = nfa->states; s != NULL; s = s->next)
	{
		nstates++;
		narcs += s->nouts + 1;	/* need one extra for endmarker */
	}

	cnfa->stflags = (char *) MALLOC(nstates * sizeof(char));
	cnfa->states = (struct carc **) MALLOC(nstates * sizeof(struct carc *));
	cnfa->arcs = (struct carc *) MALLOC(narcs * sizeof(struct carc));
	if (cnfa->stflags == NULL || cnfa->states == NULL || cnfa->arcs == NULL)
	{
		if (cnfa->stflags != NULL)
			FREE(cnfa->stflags);
		if (cnfa->states != NULL)
			FREE(cnfa->states);
		if (cnfa->arcs != NULL)
			FREE(cnfa->arcs);
		NERR(REG_ESPACE);
		return;
	}
	cnfa->nstates = nstates;
	cnfa->pre = nfa->pre->no;
	cnfa->post = nfa->post->no;
	cnfa->bos[0] = nfa->bos[0];
	cnfa->bos[1] = nfa->bos[1];
	cnfa->eos[0] = nfa->eos[0];
	cnfa->eos[1] = nfa->eos[1];
	cnfa->ncolors = maxcolor(nfa->cm) + 1;
	cnfa->flags = 0;

	ca = cnfa->arcs;
	for (s = nfa->states; s != NULL; s = s->next)
	{
		assert((size_t) s->no < nstates);
		cnfa->stflags[s->no] = 0;
		cnfa->states[s->no] = ca;
		first = ca;
		for (a = s->outs; a != NULL; a = a->outchain)
			switch (a->type)
			{
				case PLAIN:
					ca->co = a->co;
					ca->to = a->to->no;
					ca++;
					break;
				case LACON:
					assert(s->no != cnfa->pre);
					ca->co = (color) (cnfa->ncolors + a->co);
					ca->to = a->to->no;
					ca++;
					cnfa->flags |= HASLACONS;
					break;
				default:
					NERR(REG_ASSERT);
					break;
			}
		carcsort(first, ca - 1);
		ca->co = COLORLESS;
		ca->to = 0;
		ca++;
	}
	assert(ca == &cnfa->arcs[narcs]);
	assert(cnfa->nstates != 0);

	/* mark no-progress states */
	for (a = nfa->pre->outs; a != NULL; a = a->outchain)
		cnfa->stflags[a->to->no] = CNFA_NOPROGRESS;
	cnfa->stflags[nfa->pre->no] = CNFA_NOPROGRESS;
}

/*
 * carcsort - sort compacted-NFA arcs by color
 *
 * Really dumb algorithm, but if the list is long enough for that to matter,
 * you're in real trouble anyway.
 */
static void
carcsort(struct carc * first,
		 struct carc * last)
{
	struct carc *p;
	struct carc *q;
	struct carc tmp;

	if (last - first <= 1)
		return;

	for (p = first; p <= last; p++)
		for (q = p; q <= last; q++)
			if (p->co > q->co ||
				(p->co == q->co && p->to > q->to))
			{
				assert(p != q);
				tmp = *p;
				*p = *q;
				*q = tmp;
			}
}

/*
 * freecnfa - free a compacted NFA
 */
static void
freecnfa(struct cnfa * cnfa)
{
	assert(cnfa->nstates != 0); /* not empty already */
	cnfa->nstates = 0;
	FREE(cnfa->stflags);
	FREE(cnfa->states);
	FREE(cnfa->arcs);
}

/*
 * dumpnfa - dump an NFA in human-readable form
 */
static void
dumpnfa(struct nfa * nfa,
		FILE *f)
{
#ifdef REG_DEBUG
	struct state *s;

	fprintf(f, "pre %d, post %d", nfa->pre->no, nfa->post->no);
	if (nfa->bos[0] != COLORLESS)
		fprintf(f, ", bos [%ld]", (long) nfa->bos[0]);
	if (nfa->bos[1] != COLORLESS)
		fprintf(f, ", bol [%ld]", (long) nfa->bos[1]);
	if (nfa->eos[0] != COLORLESS)
		fprintf(f, ", eos [%ld]", (long) nfa->eos[0]);
	if (nfa->eos[1] != COLORLESS)
		fprintf(f, ", eol [%ld]", (long) nfa->eos[1]);
	fprintf(f, "\n");
	for (s = nfa->states; s != NULL; s = s->next)
		dumpstate(s, f);
	if (nfa->parent == NULL)
		dumpcolors(nfa->cm, f);
	fflush(f);
#endif
}

#ifdef REG_DEBUG				/* subordinates of dumpnfa */

/*
 * dumpstate - dump an NFA state in human-readable form
 */
static void
dumpstate(struct state * s,
		  FILE *f)
{
	struct arc *a;

	fprintf(f, "%d%s%c", s->no, (s->tmp != NULL) ? "T" : "",
			(s->flag) ? s->flag : '.');
	if (s->prev != NULL && s->prev->next != s)
		fprintf(f, "\tstate chain bad\n");
	if (s->nouts == 0)
		fprintf(f, "\tno out arcs\n");
	else
		dumparcs(s, f);
	fflush(f);
	for (a = s->ins; a != NULL; a = a->inchain)
	{
		if (a->to != s)
			fprintf(f, "\tlink from %d to %d on %d's in-chain\n",
					a->from->no, a->to->no, s->no);
	}
}

/*
 * dumparcs - dump out-arcs in human-readable form
 */
static void
dumparcs(struct state * s,
		 FILE *f)
{
	int			pos;

	assert(s->nouts > 0);
	/* printing arcs in reverse order is usually clearer */
	pos = dumprarcs(s->outs, s, f, 1);
	if (pos != 1)
		fprintf(f, "\n");
}

/*
 * dumprarcs - dump remaining outarcs, recursively, in reverse order
 */
static int						/* resulting print position */
dumprarcs(struct arc * a,
		  struct state * s,
		  FILE *f,
		  int pos)				/* initial print position */
{
	if (a->outchain != NULL)
		pos = dumprarcs(a->outchain, s, f, pos);
	dumparc(a, s, f);
	if (pos == 5)
	{
		fprintf(f, "\n");
		pos = 1;
	}
	else
		pos++;
	return pos;
}

/*
 * dumparc - dump one outarc in readable form, including prefixing tab
 */
static void
dumparc(struct arc * a,
		struct state * s,
		FILE *f)
{
	struct arc *aa;
	struct arcbatch *ab;

	fprintf(f, "\t");
	switch (a->type)
	{
		case PLAIN:
			fprintf(f, "[%ld]", (long) a->co);
			break;
		case AHEAD:
			fprintf(f, ">%ld>", (long) a->co);
			break;
		case BEHIND:
			fprintf(f, "<%ld<", (long) a->co);
			break;
		case LACON:
			fprintf(f, ":%ld:", (long) a->co);
			break;
		case '^':
		case '$':
			fprintf(f, "%c%d", a->type, (int) a->co);
			break;
		case EMPTY:
			break;
		default:
			fprintf(f, "0x%x/0%lo", a->type, (long) a->co);
			break;
	}
	if (a->from != s)
		fprintf(f, "?%d?", a->from->no);
	for (ab = &a->from->oas; ab != NULL; ab = ab->next)
	{
		for (aa = &ab->a[0]; aa < &ab->a[ABSIZE]; aa++)
			if (aa == a)
				break;			/* NOTE BREAK OUT */
		if (aa < &ab->a[ABSIZE])	/* propagate break */
			break;				/* NOTE BREAK OUT */
	}
	if (ab == NULL)
		fprintf(f, "?!?");		/* not in allocated space */
	fprintf(f, "->");
	if (a->to == NULL)
	{
		fprintf(f, "NULL");
		return;
	}
	fprintf(f, "%d", a->to->no);
	for (aa = a->to->ins; aa != NULL; aa = aa->inchain)
		if (aa == a)
			break;				/* NOTE BREAK OUT */
	if (aa == NULL)
		fprintf(f, "?!?");		/* missing from in-chain */
}
#endif   /* REG_DEBUG */

/*
 * dumpcnfa - dump a compacted NFA in human-readable form
 */
#ifdef REG_DEBUG
static void
dumpcnfa(struct cnfa * cnfa,
		 FILE *f)
{
	int			st;

	fprintf(f, "pre %d, post %d", cnfa->pre, cnfa->post);
	if (cnfa->bos[0] != COLORLESS)
		fprintf(f, ", bos [%ld]", (long) cnfa->bos[0]);
	if (cnfa->bos[1] != COLORLESS)
		fprintf(f, ", bol [%ld]", (long) cnfa->bos[1]);
	if (cnfa->eos[0] != COLORLESS)
		fprintf(f, ", eos [%ld]", (long) cnfa->eos[0]);
	if (cnfa->eos[1] != COLORLESS)
		fprintf(f, ", eol [%ld]", (long) cnfa->eos[1]);
	if (cnfa->flags & HASLACONS)
		fprintf(f, ", haslacons");
	fprintf(f, "\n");
	for (st = 0; st < cnfa->nstates; st++)
		dumpcstate(st, cnfa, f);
	fflush(f);
}
#endif

#ifdef REG_DEBUG				/* subordinates of dumpcnfa */

/*
 * dumpcstate - dump a compacted-NFA state in human-readable form
 */
static void
dumpcstate(int st,
		   struct cnfa * cnfa,
		   FILE *f)
{
	struct carc *ca;
	int			pos;

	fprintf(f, "%d%s", st, (cnfa->stflags[st] & CNFA_NOPROGRESS) ? ":" : ".");
	pos = 1;
	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co < cnfa->ncolors)
			fprintf(f, "\t[%ld]->%d", (long) ca->co, ca->to);
		else
			fprintf(f, "\t:%ld:->%d", (long) (ca->co - cnfa->ncolors), ca->to);
		if (pos == 5)
		{
			fprintf(f, "\n");
			pos = 1;
		}
		else
			pos++;
	}
	if (ca == cnfa->states[st] || pos != 1)
		fprintf(f, "\n");
	fflush(f);
}

#endif   /* REG_DEBUG */
