#include "btree_gist.h"
#include "btree_utils_num.h"

typedef struct
{
   Timestamp    lower;
   Timestamp    upper;
}  tsKEY;

/*
** timestamp ops
*/
PG_FUNCTION_INFO_V1(gbt_ts_compress);
PG_FUNCTION_INFO_V1(gbt_tstz_compress);
PG_FUNCTION_INFO_V1(gbt_ts_union);
PG_FUNCTION_INFO_V1(gbt_ts_picksplit);
PG_FUNCTION_INFO_V1(gbt_ts_consistent);
PG_FUNCTION_INFO_V1(gbt_tstz_consistent);
PG_FUNCTION_INFO_V1(gbt_ts_penalty);
PG_FUNCTION_INFO_V1(gbt_ts_same);

Datum    gbt_ts_compress(PG_FUNCTION_ARGS);
Datum    gbt_tstz_compress(PG_FUNCTION_ARGS);
Datum    gbt_ts_union(PG_FUNCTION_ARGS);
Datum    gbt_ts_picksplit(PG_FUNCTION_ARGS);
Datum    gbt_ts_consistent(PG_FUNCTION_ARGS);
Datum    gbt_tstz_consistent(PG_FUNCTION_ARGS);
Datum    gbt_ts_penalty(PG_FUNCTION_ARGS);
Datum    gbt_ts_same(PG_FUNCTION_ARGS);


static bool     gbt_tsgt     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(timestamp_gt,PointerGetDatum( a ), PointerGetDatum( b ) )
  );
}

static bool     gbt_tsge     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(timestamp_ge,PointerGetDatum( a ), PointerGetDatum( b ) )
  );
}

static bool     gbt_tseq     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(timestamp_eq,PointerGetDatum( a ), PointerGetDatum( b ) )
  );
}

static bool     gbt_tsle     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(timestamp_le,PointerGetDatum( a ), PointerGetDatum( b ) )
  );
}

static bool     gbt_tslt     (const void *a, const void *b)
{
  return DatumGetBool( 
     DirectFunctionCall2(timestamp_lt,PointerGetDatum( a ), PointerGetDatum( b ) )
  );
}


static int
gbt_tskey_cmp(const void *a, const void *b)
{
  if ( gbt_tsgt( (void*)&(((Nsrt *) a)->t[0]) , (void*)&(((Nsrt *) b)->t[0]) ) ){
    return 1;
  } else
  if ( gbt_tslt( (void*)&(((Nsrt *) a)->t[0]) , (void*)&(((Nsrt *) b)->t[0]) ) ){
    return -1;
  }
  return  0;
}


static const gbtree_ninfo tinfo = 
{ 
  gbt_t_ts,
  sizeof(Timestamp),
  gbt_tsgt,
  gbt_tsge,
  gbt_tseq,
  gbt_tsle,
  gbt_tslt,
  gbt_tskey_cmp
};


/**************************************************
 * timestamp ops
 **************************************************/



static Timestamp * tstz_to_ts_gmt ( Timestamp * gmt, TimestampTz * ts )
{
    int         val, tz  ;

    *gmt = *ts;
    DecodeSpecial(0, "gmt", &val);
 
    if ( ! TIMESTAMP_NOT_FINITE(*ts))
    {
      tz = val * 60;

#ifdef HAVE_INT64_TIMESTAMP
      *gmt -= (tz * INT64CONST(1000000));
#else
      *gmt -= tz;
      *gmt  = JROUND(*gmt);
#endif
  
    }
    return gmt;
}




Datum
gbt_ts_compress(PG_FUNCTION_ARGS)
{
    GISTENTRY  *entry  = (GISTENTRY *) PG_GETARG_POINTER(0);
    GISTENTRY  *retval = NULL;
    PG_RETURN_POINTER( gbt_num_compress( retval , entry , &tinfo ));
}


Datum
gbt_tstz_compress(PG_FUNCTION_ARGS)
{
  GISTENTRY  *entry  = (GISTENTRY *) PG_GETARG_POINTER(0);
  GISTENTRY  *retval ;

  if (entry->leafkey)   
  {
    tsKEY      *r      = (tsKEY *) palloc(sizeof(tsKEY));
    
    TimestampTz ts = *(TimestampTz *) DatumGetPointer(entry->key);
    Timestamp   gmt ;

    tstz_to_ts_gmt ( &gmt, &ts );

    retval = palloc(sizeof(GISTENTRY));
    r->lower = r->upper = gmt ;
    gistentryinit(*retval, PointerGetDatum(r),
         entry->rel, entry->page,
         entry->offset, sizeof(tsKEY), FALSE);
  }
  else
        retval = entry;

  PG_RETURN_POINTER( retval );
}


Datum
gbt_ts_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY        *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
    Timestamp        *query = (Timestamp *) PG_GETARG_POINTER(1);
    tsKEY              *kkk = (tsKEY *) DatumGetPointer(entry->key);
    GBT_NUMKEY_R        key ;
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
    key.lower = (GBT_NUMKEY*) &kkk->lower ;
    key.upper = (GBT_NUMKEY*) &kkk->upper ;

    PG_RETURN_BOOL(
      gbt_num_consistent( &key, (void*)query,&strategy,GIST_LEAF(entry),&tinfo)
    );
}

Datum
gbt_tstz_consistent(PG_FUNCTION_ARGS)
{
    GISTENTRY        *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
    TimestampTz      *query = (Timestamp *) PG_GETARG_POINTER(1);
    tsKEY              *kkk = (tsKEY *) DatumGetPointer(entry->key);
    GBT_NUMKEY_R        key ;
    StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
    Timestamp    qqq  ;
    key.lower = (GBT_NUMKEY*) &kkk->lower ;
    key.upper = (GBT_NUMKEY*) &kkk->upper ;
    tstz_to_ts_gmt ( &qqq, query );

    PG_RETURN_BOOL( 
      gbt_num_consistent( &key, (void*)&qqq,&strategy,GIST_LEAF(entry),&tinfo)
    );
}


Datum
gbt_ts_union(PG_FUNCTION_ARGS)
{
    GistEntryVector     *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
    void                     *out = palloc(sizeof(tsKEY));
    *(int *) PG_GETARG_POINTER(1) = sizeof(tsKEY);
    PG_RETURN_POINTER( gbt_num_union ( (void*)out, entryvec, &tinfo ) );
}


Datum
gbt_ts_penalty(PG_FUNCTION_ARGS)
{

        tsKEY      *origentry = (tsKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
        tsKEY      *newentry  = (tsKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
        float      *result = (float *) PG_GETARG_POINTER(2);
        Interval   *intr;
#ifdef HAVE_INT64_TIMESTAMP
        int64      res;
#else
        double     res;
#endif

        intr = DatumGetIntervalP(DirectFunctionCall2( 
                  timestamp_mi,
                  TimestampGetDatum(newentry->upper),
                  TimestampGetDatum(origentry->upper)
        ));

        /* see interval_larger */

        res  = Max(intr->time + intr->month * (30 * 86400), 0);
        pfree(intr);

        intr = DatumGetIntervalP(DirectFunctionCall2(
                  timestamp_mi,
                  TimestampGetDatum(origentry->lower),
                  TimestampGetDatum(newentry->lower)
        ));

        /* see interval_larger */
        res += Max(intr->time + intr->month * (30 * 86400), 0);
        pfree(intr);

        *result = 0.0;

        if ( res > 0 ){
          intr = DatumGetIntervalP(DirectFunctionCall2(
                  timestamp_mi,
                  TimestampGetDatum(origentry->upper),
                  TimestampGetDatum(origentry->lower)
          ));
          *result += FLT_MIN ;
          *result += (float) ( res / ( (double) ( res + intr->time + intr->month * (30 * 86400) ) ) );
          *result *= ( FLT_MAX / ( ( (GISTENTRY *) PG_GETARG_POINTER(0))->rel->rd_att->natts + 1 ) );
          pfree(intr);
        }

        PG_RETURN_POINTER(result);

}


Datum
gbt_ts_picksplit(PG_FUNCTION_ARGS)
{
  PG_RETURN_POINTER(gbt_num_picksplit(
      (GistEntryVector *) PG_GETARG_POINTER(0),
      (GIST_SPLITVEC *) PG_GETARG_POINTER(1),
      &tinfo
  ));
}

Datum
gbt_ts_same(PG_FUNCTION_ARGS)
{
  tsKEY    *b1 = (tsKEY *) PG_GETARG_POINTER(0);
  tsKEY    *b2 = (tsKEY *) PG_GETARG_POINTER(1);
  bool     *result = (bool *) PG_GETARG_POINTER(2);

  *result  = gbt_num_same ( (void*)b1, (void*)b2, &tinfo );
  PG_RETURN_POINTER(result);
}

