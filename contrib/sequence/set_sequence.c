/*
 * set_sequence.c --
 *
 * Set a new sequence value.
 *
 * Copyright (c) 1996, Massimo Dal Zotto <dz@cs.unitn.it>
 */

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "commands/sequence.h"

#include "set_sequence.h"

extern int	setval(struct varlena * seqin, int4 val);

int
set_currval(struct varlena * sequence, int4 nextval)
{
	return setval(sequence, nextval);
}

int
next_id(struct varlena * sequence)
{
	return nextval(sequence);
}

int
last_id(struct varlena * sequence)
{
	return currval(sequence);
}

int
set_last_id(struct varlena * sequence, int4 nextval)
{
	return setval(sequence, nextval);
}

/* end of file */
