/*-------------------------------------------------------------------------
 *
 * multikey.c--
 *    POSTGRES multikey indices create code.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include <postgres.h>

#include <storage/lmgr.h>
#include <nodes/parsenodes.h>
#include <commands/defrem.h>
#include <utils/builtins.h>

int4 create_mki_2 (struct varlena * index, struct varlena * rel, 
			struct varlena * a1, struct varlena * a2); 
int4 create_mki_3 (struct varlena * index, struct varlena * rel, 
			struct varlena * a1, struct varlena * a2,
			struct varlena * a3); 
int4 create_mki_4 (struct varlena * index, struct varlena * rel, 
			struct varlena * a1, struct varlena * a2, 
			struct varlena * a3, struct varlena * a4); 

extern bool FastBuild;
static bool BuildMode;


int4 create_mki_2 (struct varlena * index, struct varlena * rel, 
			struct varlena * a1, struct varlena * a2)
{
    char * indexname = textout (index);
    char * relname = textout (rel);
    char * a1name = textout (a1);
    char * a2name = textout (a2);
    IndexElem *ie1 = makeNode(IndexElem);
    IndexElem *ie2 = makeNode(IndexElem);
    List *alist;

    ie1->name = a1name;
    ie1->args = NIL; ie1->class = NULL; ie1->tname = (TypeName*)NULL;
    ie2->name = a2name;
    ie2->args = NIL; ie2->class = NULL; ie2->tname = (TypeName*)NULL;

    alist = lcons (ie2,NIL);
    alist = lcons (ie1,alist);
    
    BuildMode = FastBuild;
    FastBuild = false;		/* for the moment */
    
    DefineIndex (relname, indexname, "btree", alist, 
    			(List*) NULL, false, (Expr*)NULL, (List*)NULL);

    FastBuild = BuildMode;

    return (0);

}


int4 create_mki_3 (struct varlena * index, struct varlena * rel, 
			struct varlena * a1, struct varlena * a2,
			struct varlena * a3)
{
    char * indexname = textout (index);
    char * relname = textout (rel);
    char * a1name = textout (a1);
    char * a2name = textout (a2);
    char * a3name = textout (a3);
    IndexElem *ie1 = makeNode(IndexElem);
    IndexElem *ie2 = makeNode(IndexElem);
    IndexElem *ie3 = makeNode(IndexElem);
    List *alist;

    ie1->name = a1name;
    ie1->args = NIL; ie1->class = NULL; ie1->tname = (TypeName*)NULL;
    ie2->name = a2name;
    ie2->args = NIL; ie2->class = NULL; ie2->tname = (TypeName*)NULL;
    ie3->name = a3name;
    ie3->args = NIL; ie3->class = NULL; ie3->tname = (TypeName*)NULL;

    alist = lcons (ie3,NIL);
    alist = lcons (ie2,alist);
    alist = lcons (ie1,alist);
    
    BuildMode = FastBuild;
    FastBuild = false;		/* for the moment */
    
    DefineIndex (relname, indexname, "btree", alist, 
    			(List*) NULL, false, (Expr*)NULL, (List*)NULL);

    FastBuild = BuildMode;

    return (0);

}


int4 create_mki_4 (struct varlena * index, struct varlena * rel, 
			struct varlena * a1, struct varlena * a2,
			struct varlena * a3, struct varlena * a4)
{
    char * indexname = textout (index);
    char * relname = textout (rel);
    char * a1name = textout (a1);
    char * a2name = textout (a2);
    char * a3name = textout (a3);
    char * a4name = textout (a4);
    IndexElem *ie1 = makeNode(IndexElem);
    IndexElem *ie2 = makeNode(IndexElem);
    IndexElem *ie3 = makeNode(IndexElem);
    IndexElem *ie4 = makeNode(IndexElem);
    List *alist;

    ie1->name = a1name;
    ie1->args = NIL; ie1->class = NULL; ie1->tname = (TypeName*)NULL;
    ie2->name = a2name;
    ie2->args = NIL; ie2->class = NULL; ie2->tname = (TypeName*)NULL;
    ie3->name = a3name;
    ie3->args = NIL; ie3->class = NULL; ie3->tname = (TypeName*)NULL;
    ie4->name = a4name;
    ie4->args = NIL; ie4->class = NULL; ie4->tname = (TypeName*)NULL;

    alist = lcons (ie4,NIL);
    alist = lcons (ie3,alist);
    alist = lcons (ie2,alist);
    alist = lcons (ie1,alist);
    
    BuildMode = FastBuild;
    FastBuild = false;		/* for the moment */
    
    DefineIndex (relname, indexname, "btree", alist, 
    			(List*) NULL, false, (Expr*)NULL, (List*)NULL);

    FastBuild = BuildMode;

    return (0);

}
