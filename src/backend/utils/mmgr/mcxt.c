/*-------------------------------------------------------------------------
 *
 * mcxt.c--
 *	  POSTGRES memory context code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mmgr/mcxt.c,v 1.8 1998/06/15 19:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				/* XXX for printf debugging */

#include "postgres.h"

#include "utils/memutils.h"
#include "utils/module.h"
#include "utils/excid.h"

#include "nodes/memnodes.h"
#include "nodes/nodes.h"

#include "utils/mcxt.h"
#include "utils/elog.h"

#include "utils/palloc.h"

#undef MemoryContextAlloc
#undef MemoryContextFree
#undef malloc
#undef free

/*
 * Global State
 */
static int	MemoryContextEnableCount = 0;

#define MemoryContextEnabled	(MemoryContextEnableCount > 0)

static OrderedSetData ActiveGlobalMemorySetData;		/* uninitialized */

#define ActiveGlobalMemorySet	(&ActiveGlobalMemorySetData)

/*
 * description of allocated memory representation goes here
 */

#define PSIZE(PTR)		(*((int32 *)(PTR) - 1))
#define PSIZEALL(PTR)	(*((int32 *)(PTR) - 1) + sizeof (int32))
#define PSIZESKIP(PTR)	((char *)((int32 *)(PTR) + 1))
#define PSIZEFIND(PTR)	((char *)((int32 *)(PTR) - 1))
#define PSIZESPACE(LEN) ((LEN) + sizeof (int32))

/*
 * AllocSizeIsValid --
 *		True iff 0 < size and size <= MaxAllocSize.
 */
#define AllocSizeIsValid(size)	(0 < (size) && (size) <= MaxAllocSize)

/*****************************************************************************
 *	  GLOBAL MEMORY															 *
 *****************************************************************************/

/*
 * CurrentMemoryContext --
 *		Memory context for general global allocations.
 */
MemoryContext CurrentMemoryContext = NULL;

/*****************************************************************************
 *	  PRIVATE DEFINITIONS													 *
 *****************************************************************************/

static Pointer GlobalMemoryAlloc(GlobalMemory this, Size size);
static void GlobalMemoryFree(GlobalMemory this, Pointer pointer);
static Pointer
GlobalMemoryRealloc(GlobalMemory this, Pointer pointer,
					Size size);
static char *GlobalMemoryGetName(GlobalMemory this);
static void GlobalMemoryDump(GlobalMemory this);

#ifdef NOT_USED
static void DumpGlobalMemories(void);

#endif

/*
 * Global Memory Methods
 */

static struct MemoryContextMethodsData GlobalContextMethodsData = {
	GlobalMemoryAlloc,			/* Pointer (*)(this, uint32)  palloc */
	GlobalMemoryFree,			/* void (*)(this, Pointer)	  pfree */
	GlobalMemoryRealloc,		/* Pointer (*)(this, Pointer) repalloc */
	GlobalMemoryGetName,		/* char* (*)(this)			  getName */
	GlobalMemoryDump			/* void (*)(this)			  dump */
};

/*
 * Note:
 *		TopGlobalMemory is handled specially because of bootstrapping.
 */
/* extern bool EqualGlobalMemory(); */

static struct GlobalMemory TopGlobalMemoryData = {
	T_GlobalMemory,				/* NodeTag				tag		  */
	&GlobalContextMethodsData,	/* ContextMethods		method	  */
	{{0}},						/* uninitialized OrderedSetData allocSetD */
	"TopGlobal",				/* char* name	   */
	{0}							/* uninitialized OrderedElemData elemD */
};

/*
 * TopMemoryContext --
 *		Memory context for general global allocations.
 *
 * Note:
 *		Don't use this memory context for random allocations.  If you
 *		allocate something here, you are expected to clean it up when
 *		appropriate.
 */
MemoryContext TopMemoryContext = (MemoryContext) &TopGlobalMemoryData;




/*
 * Module State
 */

/*
 * EnableMemoryContext --
 *		Enables/disables memory management and global contexts.
 *
 * Note:
 *		This must be called before creating contexts or allocating memory.
 *		This must be called before other contexts are created.
 *
 * Exceptions:
 *		BadArg if on is invalid.
 *		BadState if on is false when disabled.
 */
void
EnableMemoryContext(bool on)
{
	static bool processing = false;

	AssertState(!processing);
	AssertArg(BoolIsValid(on));

	if (BypassEnable(&MemoryContextEnableCount, on))
		return;

	processing = true;

	if (on)
	{							/* initialize */
		/* initialize TopGlobalMemoryData.setData */
		AllocSetInit(&TopGlobalMemoryData.setData, DynamicAllocMode,
					 (Size) 0);

		/* make TopGlobalMemoryData member of ActiveGlobalMemorySet */
		OrderedSetInit(ActiveGlobalMemorySet,
					   offsetof(struct GlobalMemory, elemData));
		OrderedElemPushInto(&TopGlobalMemoryData.elemData,
							ActiveGlobalMemorySet);

		/* initialize CurrentMemoryContext */
		CurrentMemoryContext = TopMemoryContext;

	}
	else
	{							/* cleanup */
		GlobalMemory context;

		/* walk the list of allocations */
		while (PointerIsValid(context = (GlobalMemory)
							  OrderedSetGetHead(ActiveGlobalMemorySet)))
		{

			if (context == &TopGlobalMemoryData)
			{
				/* don't free it and clean it last */
				OrderedElemPop(&TopGlobalMemoryData.elemData);
			}
			else
				GlobalMemoryDestroy(context);
			/* what is needed for the top? */
		}

		/*
		 * Freeing memory here should be safe as this is called only after
		 * all modules which allocate in TopMemoryContext have been
		 * disabled.
		 */

		/* step through remaining allocations and log */
		/* AllocSetStep(...); */

		/* deallocate whatever is left */
		AllocSetReset(&TopGlobalMemoryData.setData);
	}

	processing = false;
}

/*
 * MemoryContextAlloc --
 *		Returns pointer to aligned allocated memory in the given context.
 *
 * Note:
 *		none
 *
 * Exceptions:
 *		BadState if called before InitMemoryManager.
 *		BadArg if context is invalid or if size is 0.
 *		BadAllocSize if size is larger than MaxAllocSize.
 */
Pointer
MemoryContextAlloc(MemoryContext context, Size size)
{
	AssertState(MemoryContextEnabled);
	AssertArg(MemoryContextIsValid(context));

	LogTrap(!AllocSizeIsValid(size), BadAllocSize,
			("size=%d [0x%x]", size, size));

	return (context->method->alloc(context, size));
}

/*
 * MemoryContextFree --
 *		Frees allocated memory referenced by pointer in the given context.
 *
 * Note:
 *		none
 *
 * Exceptions:
 *		???
 *		BadArgumentsErr if firstTime is true for subsequent calls.
 */
void
MemoryContextFree(MemoryContext context, Pointer pointer)
{
	AssertState(MemoryContextEnabled);
	AssertArg(MemoryContextIsValid(context));
	AssertArg(PointerIsValid(pointer));

	context->method->free_p(context, pointer);
}

/*
 * MemoryContextRelloc --
 *		Returns pointer to aligned allocated memory in the given context.
 *
 * Note:
 *		none
 *
 * Exceptions:
 *		???
 *		BadArgumentsErr if firstTime is true for subsequent calls.
 */
Pointer
MemoryContextRealloc(MemoryContext context,
					 Pointer pointer,
					 Size size)
{
	AssertState(MemoryContextEnabled);
	AssertArg(MemoryContextIsValid(context));
	AssertArg(PointerIsValid(pointer));

	LogTrap(!AllocSizeIsValid(size), BadAllocSize,
			("size=%d [0x%x]", size, size));

	return (context->method->realloc(context, pointer, size));
}

/*
 * MemoryContextGetName --
 *		Returns pointer to aligned allocated memory in the given context.
 *
 * Note:
 *		none
 *
 * Exceptions:
 *		???
 *		BadArgumentsErr if firstTime is true for subsequent calls.
 */
#ifdef NOT_USED
char *
MemoryContextGetName(MemoryContext context)
{
	AssertState(MemoryContextEnabled);
	AssertArg(MemoryContextIsValid(context));

	return (context->method->getName(context));
}

#endif

/*
 * PointerGetAllocSize --
 *		Returns size of aligned allocated memory given pointer to it.
 *
 * Note:
 *		none
 *
 * Exceptions:
 *		???
 *		BadArgumentsErr if firstTime is true for subsequent calls.
 */
#ifdef NOT_USED
Size
PointerGetAllocSize(Pointer pointer)
{
	AssertState(MemoryContextEnabled);
	AssertArg(PointerIsValid(pointer));

	return (PSIZE(pointer));
}

#endif

/*
 * MemoryContextSwitchTo --
 *		Returns the current context; installs the given context.
 *
 * Note:
 *		none
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadArg if context is invalid.
 */
MemoryContext
MemoryContextSwitchTo(MemoryContext context)
{
	MemoryContext old;

	AssertState(MemoryContextEnabled);
	AssertArg(MemoryContextIsValid(context));

	old = CurrentMemoryContext;
	CurrentMemoryContext = context;
	return (old);
}

/*
 * External Functions
 */
/*
 * CreateGlobalMemory --
 *		Returns new global memory context.
 *
 * Note:
 *		Assumes name is static.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadState if called outside TopMemoryContext (TopGlobalMemory).
 *		BadArg if name is invalid.
 */
GlobalMemory
CreateGlobalMemory(char *name)	/* XXX MemoryContextName */
{
	GlobalMemory context;
	MemoryContext savecxt;

	AssertState(MemoryContextEnabled);

	savecxt = MemoryContextSwitchTo(TopMemoryContext);

	context = (GlobalMemory) newNode(sizeof(struct GlobalMemory), T_GlobalMemory);
	context->method = &GlobalContextMethodsData;
	context->name = name;		/* assumes name is static */
	AllocSetInit(&context->setData, DynamicAllocMode, (Size) 0);

	/* link the context */
	OrderedElemPushInto(&context->elemData, ActiveGlobalMemorySet);

	MemoryContextSwitchTo(savecxt);
	return (context);
}

/*
 * GlobalMemoryDestroy --
 *		Destroys given global memory context.
 *
 * Exceptions:
 *		BadState if called when disabled.
 *		BadState if called outside TopMemoryContext (TopGlobalMemory).
 *		BadArg if context is invalid GlobalMemory.
 *		BadArg if context is TopMemoryContext (TopGlobalMemory).
 */
void
GlobalMemoryDestroy(GlobalMemory context)
{
	AssertState(MemoryContextEnabled);
	AssertArg(IsA(context, GlobalMemory));
	AssertArg(context != &TopGlobalMemoryData);

	AllocSetReset(&context->setData);

	/* unlink and delete the context */
	OrderedElemPop(&context->elemData);
	MemoryContextFree(TopMemoryContext, (Pointer) context);
}

/*****************************************************************************
 *	  PRIVATE																 *
 *****************************************************************************/

/*
 * GlobalMemoryAlloc --
 *		Returns pointer to aligned space in the global context.
 *
 * Exceptions:
 *		ExhaustedMemory if allocation fails.
 */
static Pointer
GlobalMemoryAlloc(GlobalMemory this, Size size)
{
	return (AllocSetAlloc(&this->setData, size));
}

/*
 * GlobalMemoryFree --
 *		Frees allocated memory in the global context.
 *
 * Exceptions:
 *		BadContextErr if current context is not the global context.
 *		BadArgumentsErr if pointer is invalid.
 */
static void
GlobalMemoryFree(GlobalMemory this,
				 Pointer pointer)
{
	AllocSetFree(&this->setData, pointer);
}

/*
 * GlobalMemoryRealloc --
 *		Returns pointer to aligned space in the global context.
 *
 * Note:
 *		Memory associated with the pointer is freed before return.
 *
 * Exceptions:
 *		BadContextErr if current context is not the global context.
 *		BadArgumentsErr if pointer is invalid.
 *		NoMoreMemoryErr if allocation fails.
 */
static Pointer
GlobalMemoryRealloc(GlobalMemory this,
					Pointer pointer,
					Size size)
{
	return (AllocSetRealloc(&this->setData, pointer, size));
}

/*
 * GlobalMemoryGetName --
 *		Returns name string for context.
 *
 * Exceptions:
 *		???
 */
static char *
GlobalMemoryGetName(GlobalMemory this)
{
	return (this->name);
}

/*
 * GlobalMemoryDump --
 *		Dumps global memory context for debugging.
 *
 * Exceptions:
 *		???
 */
static void
GlobalMemoryDump(GlobalMemory this)
{
	GlobalMemory context;

	printf("--\n%s:\n", GlobalMemoryGetName(this));

	context = (GlobalMemory) OrderedElemGetPredecessor(&this->elemData);
	if (PointerIsValid(context))
		printf("\tpredecessor=%s\n", GlobalMemoryGetName(context));

	context = (GlobalMemory) OrderedElemGetSuccessor(&this->elemData);
	if (PointerIsValid(context))
		printf("\tsucessor=%s\n", GlobalMemoryGetName(context));

	AllocSetDump(&this->setData);		/* XXX is this right interface */
}

/*
 * DumpGlobalMemories --
 *		Dumps all global memory contexts for debugging.
 *
 * Exceptions:
 *		???
 */
#ifdef NOT_USED
static void
DumpGlobalMemories()
{
	GlobalMemory context;

	context = (GlobalMemory) OrderedSetGetHead(&ActiveGlobalMemorySetData);

	while (PointerIsValid(context))
	{
		GlobalMemoryDump(context);

		context = (GlobalMemory) OrderedElemGetSuccessor(
													 &context->elemData);
	}
}

#endif
