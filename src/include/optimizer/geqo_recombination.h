/*-------------------------------------------------------------------------
 *
 * geqo_recombination.h
 *	  prototypes for recombination in the genetic query optimizer
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/geqo_recombination.h
 *
 *-------------------------------------------------------------------------
 */

/* contributed by:
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * Institute of Automatic Control	   *
   =							 = University of Mining and Technology =
   *  utesch@aut.tu-freiberg.de  * Freiberg, Germany				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

/* -- parts of this are adapted from D. Whitley's Genitor algorithm -- */

#ifndef GEQO_RECOMBINATION_H
#define GEQO_RECOMBINATION_H

#include "optimizer/geqo.h"


extern void init_tour(PlannerInfo *root, Gene *tour, int num_gene);


/* edge recombination crossover [ERX] */

typedef struct Edge
{
	Gene		edge_list[4];	/* list of edges */
	int			total_edges;
	int			unused_edges;
} Edge;

extern Edge *alloc_edge_table(PlannerInfo *root, int num_gene);
extern void free_edge_table(PlannerInfo *root, Edge *edge_table);

extern float gimme_edge_table(PlannerInfo *root, Gene *tour1, Gene *tour2,
							  int num_gene, Edge *edge_table);

extern int	gimme_tour(PlannerInfo *root, Edge *edge_table, Gene *new_gene,
					   int num_gene);


/* partially matched crossover [PMX] */

#define DAD 1					/* indicator for gene from dad */
#define MOM 0					/* indicator for gene from mom */

extern void pmx(PlannerInfo *root,
				Gene *tour1, Gene *tour2,
				Gene *offspring, int num_gene);


typedef struct City
{
	int			tour2_position;
	int			tour1_position;
	int			used;
	int			select_list;
}			City;

extern City * alloc_city_table(PlannerInfo *root, int num_gene);
extern void free_city_table(PlannerInfo *root, City * city_table);

/* cycle crossover [CX] */
extern int	cx(PlannerInfo *root, Gene *tour1, Gene *tour2,
			   Gene *offspring, int num_gene, City * city_table);

/* position crossover [PX] */
extern void px(PlannerInfo *root, Gene *tour1, Gene *tour2, Gene *offspring,
			   int num_gene, City * city_table);

/* order crossover [OX1] according to Davis */
extern void ox1(PlannerInfo *root, Gene *mom, Gene *dad, Gene *offspring,
				int num_gene, City * city_table);

/* order crossover [OX2] according to Syswerda */
extern void ox2(PlannerInfo *root, Gene *mom, Gene *dad, Gene *offspring,
				int num_gene, City * city_table);

#endif							/* GEQO_RECOMBINATION_H */
