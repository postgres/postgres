/*
 * NFA utilities.
 * This file is #included by regcomp.c.
 *
 * Copyright (c) 1998, 1999 Henry Spencer.	All rights reserved.
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
 * $Header: /cvsroot/pgsql/src/backend/regex/regc_nfa.c,v 1.2 2003/08/04 00:43:21 momjian Exp $
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
		return NULL;

	nfa->states = NULL;
	nfa->slast = NULL;
	nfa->free = NULL;
	nfa->nstates = 0;
	nfa->cm = cm;
	nfa->v = v;
	nfa->bos[0] = nfa->bos[1] = COLORLESS;
	nfa->eos[0] = nfa->eos[1] = COLORLESS;
	nfa->post = newfstate(nfa, '@');	/* number 0 */
	nfa->pre = newfstate(nfa, '>');		/* number 1 */
	nfa->parent = parent;

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
	s->next = nfa->free;		/* don't delete it, put it on the free
								 * list */
	nfa->free = s;
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
	 * Put the new arc on the beginning, not the end, of the chains. Not
	 * only is this easier, it has the very useful side effect that
	 * deleting the most-recently-added arc is the cheapest case rather
	 * than the most expensive one.
	 */
	a->inchain = to->ins;
	to->ins = a;
	a->outchain = from->outs;
	from->outs = a;

	from->nouts++;
	to->nins++;

	if (COLORED(a) && nfa->parent == NULL)
		colorchain(nfa->cm, a);

	return;
}

/*
 * allocarc - allocate a new out-arc within a state
 */
static struct arc *				/* NULL for failure */
allocarc(struct nfa * nfa,
		 struct state * s)
{
	struct arc *a;
	struct arcbatch *new;
	int			i;

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
		new = (struct arcbatch *) MALLOC(sizeof(struct arcbatch));
		if (new == NULL)
		{
			NERR(REG_ESPACE);
			return NULL;
		}
		new->next = s->oas.next;
		s->oas.next = new;

		for (i = 0; i < ABSIZE; i++)
		{
			new->a[i].type = 0;
			new->a[i].freechain = &new->a[i + 1];
		}
		new->a[ABSIZE - 1].freechain = NULL;
		s->free = &new->a[0];
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
		struct state * old,
		struct state * new)
{
	struct arc *a;

	assert(old != new);

	while ((a = old->ins) != NULL)
	{
		cparc(nfa, a, a->from, new);
		freearc(nfa, a);
	}
	assert(old->nins == 0);
	assert(old->ins == NULL);
}

/*
 * copyins - copy all in arcs of a state to another state
 */
static void
copyins(struct nfa * nfa,
		struct state * old,
		struct state * new)
{
	struct arc *a;

	assert(old != new);

	for (a = old->ins; a != NULL; a = a->inchain)
		cparc(nfa, a, a->from, new);
}

/*
 * moveouts - move all out arcs of a state to another state
 */
static void
moveouts(struct nfa * nfa,
		 struct state * old,
		 struct state * new)
{
	struct arc *a;

	assert(old != new);

	while ((a = old->outs) != NULL)
	{
		cparc(nfa, a, new, a->to);
		freearc(nfa, a);
	}
}

/*
 * copyouts - copy all out arcs of a state to another state
 */
static void
copyouts(struct nfa * nfa,
		 struct state * old,
		 struct state * new)
{
	struct arc *a;

	assert(old != new);

	for (a = old->outs; a != NULL; a = a->outchain)
		cparc(nfa, a, new, a->to);
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

	if (s->nouts == 0)
		return;					/* nothing to do */
	if (s->tmp != NULL)
		return;					/* already in progress */

	s->tmp = s;					/* mark as in progress */

	while ((a = s->outs) != NULL)
	{
		to = a->to;
		deltraverse(nfa, leftend, to);
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
 * pullback - pull back constraints backward to (with luck) eliminate them
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

	if (from == to)
	{							/* circular constraint is pointless */
		freearc(nfa, con);
		return 1;
	}
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
		assert(to != from);		/* con is not an inarc */
		copyins(nfa, from, s);	/* duplicate inarcs */
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
 * pushfwd - push forward constraints forward to (with luck) eliminate them
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

	if (to == from)
	{							/* circular constraint is pointless */
		freearc(nfa, con);
		return 1;
	}
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
		copyouts(nfa, to, s);	/* duplicate outarcs */
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
	struct state *nexts;
	struct arc *a;
	struct arc *nexta;
	int			progress;

	/* find and eliminate empties until there are no more */
	do
	{
		progress = 0;
		for (s = nfa->states; s != NULL && !NISERR(); s = nexts)
		{
			nexts = s->next;
			for (a = s->outs; a != NULL && !NISERR(); a = nexta)
			{
				nexta = a->outchain;
				if (a->type == EMPTY && unempty(nfa, a))
					progress = 1;
				assert(nexta == NULL || s->no != FREESTATE);
			}
		}
		if (progress && f != NULL)
			dumpnfa(nfa, f);
	} while (progress && !NISERR());
}

/*
 * unempty - optimize out an EMPTY arc, if possible
 *
 * Actually, as it stands this function always succeeds, but the return
 * value is kept with an eye on possible future changes.
 */
static int						/* 0 couldn't, 1 could */
unempty(struct nfa * nfa,
		struct arc * a)
{
	struct state *from = a->from;
	struct state *to = a->to;
	int			usefrom;		/* work on from, as opposed to to? */

	assert(a->type == EMPTY);
	assert(from != nfa->pre && to != nfa->post);

	if (from == to)
	{							/* vacuous loop */
		freearc(nfa, a);
		return 1;
	}

	/* decide which end to work on */
	usefrom = 1;				/* default:  attack from */
	if (from->nouts > to->nins)
		usefrom = 0;
	else if (from->nouts == to->nins)
	{
		/* decide on secondary issue:  move/copy fewest arcs */
		if (from->nins > to->nouts)
			usefrom = 0;
	}

	freearc(nfa, a);
	if (usefrom)
	{
		if (from->nouts == 0)
		{
			/* was the state's only outarc */
			moveins(nfa, from, to);
			freestate(nfa, from);
		}
		else
			copyins(nfa, from, to);
	}
	else
	{
		if (to->nins == 0)
		{
			/* was the state's only inarc */
			moveouts(nfa, to, from);
			freestate(nfa, to);
		}
		else
			copyouts(nfa, to, from);
	}

	return 1;
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

	/* clear out unreachable or dead-end states */
	/* use pre to mark reachable, then post to mark can-reach-post */
	markreachable(nfa, nfa->pre, (struct state *) NULL, nfa->pre);
	markcanreach(nfa, nfa->post, nfa->pre, nfa->post);
	for (s = nfa->states; s != NULL; s = nexts)
	{
		nexts = s->next;
		if (s->tmp != nfa->post && !s->flag)
			dropstate(nfa, s);
	}
	assert(nfa->post->nins == 0 || nfa->post->tmp == nfa->post);
	cleartraverse(nfa, nfa->pre);
	assert(nfa->post->nins == 0 || nfa->post->tmp == NULL);
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
			  struct state * okay,		/* consider only states with this
										 * mark */
			  struct state * mark)		/* the value to mark with */
{
	struct arc *a;

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
			 struct state * okay,		/* consider only states with this
										 * mark */
			 struct state * mark)		/* the value to mark with */
{
	struct arc *a;

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

	if (nfa->pre->outs == NULL)
		return REG_UIMPOSSIBLE;
	for (a = nfa->pre->outs; a != NULL; a = a->outchain)
		for (aa = a->to->outs; aa != NULL; aa = aa->outchain)
			if (aa->to == nfa->post)
				return REG_UEMPTYMATCH;
	return 0;
}

/*
 * compact - compact an NFA
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
		narcs += 1 + s->nouts + 1;
		/* 1 as a fake for flags, nouts for arcs, 1 as endmarker */
	}

	cnfa->states = (struct carc **) MALLOC(nstates * sizeof(struct carc *));
	cnfa->arcs = (struct carc *) MALLOC(narcs * sizeof(struct carc));
	if (cnfa->states == NULL || cnfa->arcs == NULL)
	{
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
		cnfa->states[s->no] = ca;
		ca->co = 0;				/* clear and skip flags "arc" */
		ca++;
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
					assert(NOTREACHED);
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
		cnfa->states[a->to->no]->co = 1;
	cnfa->states[nfa->pre->no]->co = 1;
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
		dumpcstate(st, cnfa->states[st], cnfa, f);
	fflush(f);
}
#endif

#ifdef REG_DEBUG				/* subordinates of dumpcnfa */

/*
 * dumpcstate - dump a compacted-NFA state in human-readable form
 */
static void
dumpcstate(int st,
		   struct carc * ca,
		   struct cnfa * cnfa,
		   FILE *f)
{
	int			i;
	int			pos;

	fprintf(f, "%d%s", st, (ca[0].co) ? ":" : ".");
	pos = 1;
	for (i = 1; ca[i].co != COLORLESS; i++)
	{
		if (ca[i].co < cnfa->ncolors)
			fprintf(f, "\t[%ld]->%d", (long) ca[i].co, ca[i].to);
		else
			fprintf(f, "\t:%ld:->%d", (long) ca[i].co - cnfa->ncolors,
					ca[i].to);
		if (pos == 5)
		{
			fprintf(f, "\n");
			pos = 1;
		}
		else
			pos++;
	}
	if (i == 1 || pos != 1)
		fprintf(f, "\n");
	fflush(f);
}

#endif   /* REG_DEBUG */
