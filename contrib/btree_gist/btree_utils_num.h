
typedef char GBT_NUMKEY;

/* Better readable key */
typedef struct
{
  const GBT_NUMKEY * lower, * upper;
} GBT_NUMKEY_R;


/* for sorting */
typedef struct
{
  int          i;
  GBT_NUMKEY * t;
} Nsrt;


/* type description */

typedef struct
{

  /* Attribs */

  enum gbtree_type t       ;  /* data type */
  int32            size    ;  /* size of type , 0 means variable */

  /* Methods */

  bool         (*f_gt)         ( const void * , const void * );    /* greater then */
  bool         (*f_ge)         ( const void * , const void * );    /* greater equal */
  bool         (*f_eq)         ( const void * , const void * );    /* equal */
  bool         (*f_le)         ( const void * , const void * );    /* less equal */
  bool         (*f_lt)         ( const void * , const void * );    /* less then */
  int          (*f_cmp)        ( const void * , const void * );    /* key compare function */
} gbtree_ninfo;


/*
 *  Numeric btree functions
*/

extern bool            gbt_num_consistent( const GBT_NUMKEY_R * key , const void * query,
                                const StrategyNumber * strategy , bool is_leaf,
                                const gbtree_ninfo * tinfo );

extern GIST_SPLITVEC  *gbt_num_picksplit ( const GistEntryVector *entryvec, GIST_SPLITVEC *v,
                                const gbtree_ninfo * tinfo );

extern GISTENTRY      *gbt_num_compress( GISTENTRY  *retval , GISTENTRY  *entry ,
                                const gbtree_ninfo * tinfo );


extern void           *gbt_num_union ( GBT_NUMKEY * out, const GistEntryVector * entryvec,
                                const gbtree_ninfo * tinfo );

extern bool            gbt_num_same ( const GBT_NUMKEY * a, const GBT_NUMKEY * b,
                                const gbtree_ninfo * tinfo );

extern void            gbt_num_bin_union(Datum * u , GBT_NUMKEY * e ,
                                const gbtree_ninfo * tinfo );
