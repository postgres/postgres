typedef struct NDBOX {
  unsigned int size;			/* required to be a Postgres varlena type */
  unsigned int dim;
  float x[1];
} NDBOX;
