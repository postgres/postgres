#include "btree_gist.h"
#include "btree_utils_num.h"
#include "utils/date.h"

typedef struct
{
   TimeADT    lower;
   TimeADT    upper;
}  timeKEY;

/*
** time ops
*/
PG_FUNCTION_INFO_V1(gbt_time_compress);
PG_FUNCTION_INFO_V1(gbt_timetz_compress);
PG_FUNCTION_INFO_V1(gbt_time_union);
PG_FUNCTION_INFO_V1(gbt_time_picksplit);
PG_FUNCTION_INFO_V1(gbt_time_consistent);
PG_FUNCTION_INFO_V1(gbt_timetz_consistent);
PG_FUNCTION_INFO_V1(gbt_time_penalty);
PG_FUNCTION_INFO_V1(gbt_time_same);

Datum    gbt_time_compress(PG_FUNCTION_ARGS);
Datum    gbt_timetz_compress(PG_FUNCTION_ARGS);
Datum    gbt_time_union(PG_FUNCTION_ARGS);
Datum    gbt_time_picksplit(PG_FUNCTION_ARGS);
Datum    gbt_time_consistent(PG_FUNCTION_ARGS);
Datum    gbt_timetz_consistent(PG_FUNCTION_ARGS);
Datum    gbt_time_penalty(PG_FUNCTION_ARGS);
Datum    gbt_time_same(PG_FUNCTION_ARGS);


static bool     gbt_timegt     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(time_gt,TimeADTGetDatum( *((TimeADT*)a) ), TimeADTGetDatum( *((TimeADT*)b) ) )
  );
}

static bool     gbt_timege     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(time_ge,TimeADTGetDatum( *((TimeADT*)a) ), TimeADTGetDatum( *((TimeADT*)b) ) )
  );
}

static bool     gbt_timeeq     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(time_eq,TimeADTGetDatum( *((TimeADT*)a) ), TimeADTGetDatum( *((TimeADT*)b) ) )
  );
}

static bool     gbt_timele     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(time_le,TimeADTGetDatum( *((TimeADT*)a) ), TimeADTGetDatum( *((TimeADT*)b) ) )
  );
}

static bool     gbt_timelt     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(time_lt,TimeADTGetDatum( *((TimeADT*)a) ), TimeADTGetDatum( *((TimeADT*)b) ) )
  );
}



static int
gbt_timekey_cmp(const void *a, const void *b)
{
  if ( gbt_timegt( (void*)&(((Nsrt *) a)->t[0]) , (void*)&(((Nsrt *) b)->t[0]) ) ){
    return  1;
  } else
  if ( gbt_timelt( (void*)&(((Nsrt *) a)->t[0]) , (void*)&(((Nsrt *) b)->t[0]) ) ){
    return -1;
  }
  return  0;
}


static const gbtree_ninfo tinfo = 
{
  gbt_t_time,
  sizeof(TimeADT),
  gbt_timegt,
  gbt_timege,
  gbt_timeeq,
  gbt_timele,
  gbt_timelt,
  gbt_timekey_cmp
};


/**************************************************
 * time ops
 **************************************************/



Datum
gbt_time_compress(PG_FUNCTION_ARGS)
{
    GISTENTRY  *entry  = (GISTENTRY *) PG_GETARG_POINTER(0);
    GISTENTRY  *retval = NULL;
    PG_RETURN_POINTER( gbt_num_compress( retval , entry , &tinfo ));
}


Datum
gbt_timetz_compress(PG_FUNCTION_ARGS)
{
  GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
  GISTENTRY  *retval;

  if (entry->leafkey)
  {
    timeKEY    *r  = (timeKEY   *) palloc(sizeof(timeKEY));
    TimeTzADT  *tz = DatumGetTimeTzADTP(entry->key);

    retval = palloc(sizeof(GISTENTRY));

    /* We are using the time + zone only to compress */
    r->lower = r->upper = ( tz->time + tz->zone ) ; 
    gistentryinit(*retval, PointerGetDatum(r),
            entry->rel, entry->page,
            entry->offset, sizeof(timeKEY), FALSE);
  }
  else
    retval = entry;
  PG_RETURN_POINTER(retval);
}


Datum
gbt_time_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY        *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
    TimeADT          query  = PG_GETARG_TIMEADT( 1 );
    timeKEY            *kkk = (timeKEY*) DatumGetPointer(entry->key);
    GBT_NUMKEY_R        key ;
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
 
    key.lower = (GBT_NUMKEY*) &kkk->lower ;
    key.upper = (GBT_NUMKEY*) &kkk->upper ;
        

    PG_RETURN_BOOL(
      gbt_num_consistent( &key, (void*)&query,&strategy,GIST_LEAF(entry),&tinfo)
    );
}

Datum
gbt_timetz_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY        *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
    TimeTzADT        *query = PG_GETARG_TIMETZADT_P( 1 );
    TimeADT             qqq = query->time + query->zone ;
    timeKEY            *kkk = (timeKEY*) DatumGetPointer(entry->key);
    GBT_NUMKEY_R        key ;
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
 
    key.lower = (GBT_NUMKEY*) &kkk->lower ;
    key.upper = (GBT_NUMKEY*) &kkk->upper ;

    PG_RETURN_BOOL(
      gbt_num_consistent( &key, (void*)&qqq, &strategy,GIST_LEAF(entry),&tinfo)
    );
}


Datum
gbt_time_union(PG_FUNCTION_ARGS)
{
    GistEntryVector     *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    void                     *out = palloc(sizeof(timeKEY));
    *(int *) PG_GETARG_POINTER(1) = sizeof(timeKEY);
    PG_RETURN_POINTER( gbt_num_union ( (void*)out, entryvec, &tinfo ) );
}


Datum
gbt_time_penalty(PG_FUNCTION_ARGS)
{
        timeKEY      *origentry = (timeKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
        timeKEY      *newentry  = (timeKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
        float           *result = (float *)   PG_GETARG_POINTER(2);
        Interval   *intr;
#ifdef HAVE_INT64_TIMESTAMP
        int64      res;
#else
        double     res;
#endif

        intr = DatumGetIntervalP(DirectFunctionCall2(
                  time_mi_time,
                  TimeADTGetDatum(newentry->upper),
                  TimeADTGetDatum(origentry->upper)));

        /* see interval_larger */
        res   = Max(intr->time + intr->month * (30 * 86400), 0);
        pfree(intr);

        intr = DatumGetIntervalP(DirectFunctionCall2(
                  time_mi_time,
                  TimeADTGetDatum(origentry->lower),
                  TimeADTGetDatum(newentry->lower)));
   
        /* see interval_larger */
        res  += Max(intr->time + intr->month * (30 * 86400), 0);
        pfree(intr);

        *result = 0.0;

        if ( res > 0 ){
          intr = DatumGetIntervalP(DirectFunctionCall2(
                  time_mi_time,
                  TimeADTGetDatum(origentry->upper),
                  TimeADTGetDatum(origentry->lower)));
          *result += FLT_MIN ;
          *result += (float) ( res / ( (double) ( res + intr->time + intr->month * (30 * 86400) ) ) );
          *result *= ( FLT_MAX / ( ( (GISTENTRY *) PG_GETARG_POINTER(0))->rel->rd_att->natts + 1 ) );
          pfree ( intr );
        }

        PG_RETURN_POINTER(result);
}


Datum
gbt_time_picksplit(PG_FUNCTION_ARGS)
{
  PG_RETURN_POINTER(gbt_num_picksplit(
      (GistEntryVector *) PG_GETARG_POINTER(0),
      (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
      &tinfo
  ));
}

Datum
gbt_time_same(PG_FUNCTION_ARGS)
{
  timeKEY      *b1 = (timeKEY *) PG_GETARG_POINTER(0);
  timeKEY      *b2 = (timeKEY *) PG_GETARG_POINTER(1);
  bool     *result = (bool *) PG_GETARG_POINTER(2);

  *result  = gbt_num_same ( (void*)b1, (void*)b2, &tinfo );
  PG_RETURN_POINTER(result);
}

