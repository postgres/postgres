/*-------------------------------------------------------------------------
 *
 * isolationtester.h
 *	  include file for isolation tests
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/isolation/isolationtester.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ISOLATIONTESTER_H
#define ISOLATIONTESTER_H

/*
 * The structs declared here are used in the output of specparse.y.
 * Except where noted, all fields are set in the grammar and not
 * changed thereafter.
 */
typedef struct Step Step;

typedef struct
{
	char	   *name;
	char	   *setupsql;
	char	   *teardownsql;
	Step	  **steps;
	int			nsteps;
} Session;

struct Step
{
	char	   *name;
	char	   *sql;
	/* These fields are filled by check_testspec(): */
	int			session;		/* identifies owning session */
	bool		used;			/* has step been used in a permutation? */
};

typedef enum
{
	PSB_ONCE,					/* force step to wait once */
	PSB_OTHER_STEP,				/* wait for another step to complete first */
	PSB_NUM_NOTICES,			/* wait for N notices from another session */
} PermutationStepBlockerType;

typedef struct
{
	char	   *stepname;
	PermutationStepBlockerType blocktype;
	int			num_notices;	/* only used for PSB_NUM_NOTICES */
	/* These fields are filled by check_testspec(): */
	Step	   *step;			/* link to referenced step (if any) */
	/* These fields are runtime workspace: */
	int			target_notices; /* total notices needed from other session */
} PermutationStepBlocker;

typedef struct
{
	char	   *name;			/* name of referenced Step */
	PermutationStepBlocker **blockers;
	int			nblockers;
	/* These fields are filled by check_testspec(): */
	Step	   *step;			/* link to referenced Step */
} PermutationStep;

typedef struct
{
	int			nsteps;
	PermutationStep **steps;
} Permutation;

typedef struct
{
	char	  **setupsqls;
	int			nsetupsqls;
	char	   *teardownsql;
	Session   **sessions;
	int			nsessions;
	Permutation **permutations;
	int			npermutations;
} TestSpec;

extern TestSpec parseresult;

extern int	spec_yyparse(void);

extern int	spec_yylex(void);
extern void spec_yyerror(const char *message);

#endif							/* ISOLATIONTESTER_H */
