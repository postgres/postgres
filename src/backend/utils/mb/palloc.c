#include "postgres.h"
#include "utils/memutils.h"

void
elog(int lev, const char *fmt,...)
{
  printf(fmt);
}

MemoryContext CurrentMemoryContext;

void *
MemoryContextAlloc(MemoryContext context, Size size)
{
}

void
pfree(void *pointer)
{
}

void *
repalloc(void *pointer, Size size)
{
}
