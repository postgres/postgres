/*-------------------------------------------------------------------------
 *
 * exc.h--
 *    POSTGRES exception handling definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: exc.h,v 1.1.1.1 1996/07/09 06:22:01 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	EXC_H
#define EXC_H

#include "c.h"		/* for Exception, etc. */
#include <setjmp.h>

extern char *ExcFileName;
extern Index ExcLineNumber;

/*
 * ExcMessage and Exception are now defined in c.h
 */

#if defined(PORTNAME_linux) \
|| defined(PORTNAME_hpux) \
|| defined(PORTNAME_next)\
|| defined(WIN32)
typedef jmp_buf		ExcContext;
#else
typedef sigjmp_buf	ExcContext;
#endif

typedef Exception*	ExcId;
typedef long		ExcDetail;
typedef char*		ExcData;

typedef struct ExcFrame {
    struct ExcFrame	*link;
    ExcContext		context;
    ExcId		id;
    ExcDetail		detail;
    ExcData		data;
    ExcMessage		message;
} ExcFrame;

extern	ExcFrame*	ExcCurFrameP;

#define	ExcBegin()							\
	{								\
		ExcFrame	exception;				\
									\
		exception.link = ExcCurFrameP; 				\
		if (sigsetjmp(exception.context, 1) == 0) {		\
			ExcCurFrameP = &exception;			\
			{
#define	ExcExcept()							\
			}						\
			ExcCurFrameP = exception.link;			\
		} else {						\
			{
#define	ExcEnd()							\
			}						\
		}							\
	}

#define raise4(x, t, d, message) \
	ExcRaise(&(x), (ExcDetail)(t), (ExcData)(d), (ExcMessage)(message))

#define	reraise() \
	raise4(*exception.id,exception.detail,exception.data,exception.message)

typedef	void ExcProc(Exception*, ExcDetail, ExcData, ExcMessage);


/*
 * prototypes for functions in exc.c
 */
extern void EnableExceptionHandling(bool on);
extern void ExcPrint(Exception *excP, ExcDetail detail, ExcData data,
		     ExcMessage message);
extern ExcProc *ExcGetUnCaught();
extern ExcProc *ExcSetUnCaught(ExcProc *newP);
extern void ExcUnCaught(Exception *excP, ExcDetail detail, ExcData data,
			ExcMessage message);
extern void ExcUnCaught(Exception *excP, ExcDetail detail, ExcData data,
			ExcMessage message);
extern void ExcRaise(Exception *excP,
		     ExcDetail detail,
		     ExcData    data,
		     ExcMessage message);


/*
 * prototypes for functions in excabort.c
 */
extern void ExcAbort(const Exception *excP, ExcDetail detail, ExcData data,
		     ExcMessage message);

#endif	/* EXC_H */
