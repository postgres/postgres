/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteHandler.h,v 1.1 1996/08/28 07:24:07 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	REWRITEHANDLER_H
#define	REWRITEHANDLER_H


struct _rewrite_meta_knowledge {
    List *rt;
    int rt_index;
    bool instead_flag;
    int event;
    CmdType action;
    int current_varno;
    int new_varno;
    Query *rule_action;
    Node *rule_qual;
    bool nothing;
};

typedef struct _rewrite_meta_knowledge RewriteInfo;


extern List *QueryRewrite(Query *parsetree);

#endif	/*REWRITEHANDLER_H */
