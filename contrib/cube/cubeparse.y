%{
/* NdBox = [(lowerleft),(upperright)] */
/* [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))] */

#define YYPARSE_PARAM result  /* need this to pass a pointer (void *) to yyparse */
#define YYSTYPE char *
#define YYDEBUG 1

#include "postgres.h"

#include "cubedata.h"
#include "buffer.h"

#undef yylex                 /* falure to redefine yylex will result in a call to  the */
#define yylex cube_yylex     /* wrong scanner when running inside the postgres backend  */

extern int yylex();           /* defined as cube_yylex in cubescan.c */
extern int errno;

int cube_yyerror( char *msg );
int cube_yyparse(void *result);

static int delim_count(char *s, char delim);
static NDBOX * write_box(unsigned int dim, char *str1, char *str2);
static NDBOX * write_point_as_box(char *s, int dim);

%}

/* BISON Declarations */
%token FLOAT O_PAREN C_PAREN O_BRACKET C_BRACKET COMMA
%start box

/* Grammar follows */
%%

box:
          O_BRACKET paren_list COMMA paren_list C_BRACKET {

	    int dim;
	    int c = parse_buffer_curr_char();
	    int pos = parse_buffer_pos();

	    /* We can't let the parser recognize more than one valid expression:
	       the job is done and memory is allocated. */
	    if ( c != '\0' ) {
	      /* Not at EOF */
	      reset_parse_buffer();     
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("garbage at or before char %d, ('%c', \\%03o)",
                             pos, c, c)));
	      YYERROR;
	    }
	    
	    dim = delim_count($2, ',') + 1;
	    if ( (delim_count($4, ',') + 1) != dim ) {
	      reset_parse_buffer();     
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("different point dimensions in (%s) and (%s)",
                             $2, $4)));
	      YYABORT;
	    }
	    if (dim > CUBE_MAX_DIM) {
              reset_parse_buffer();
              ereport(ERROR,
                      (errcode(ERRCODE_SYNTAX_ERROR),
                       errmsg("bad cube representation"),
                       errdetail("more than %d dimensions",
                                 CUBE_MAX_DIM)));
              YYABORT;
            }
	    
	    *((void **)result) = write_box( dim, $2, $4 );
    
          }
      |
          paren_list COMMA paren_list {
	    int dim;
	    int c = parse_buffer_curr_char();
	    int pos = parse_buffer_pos();

	    if ( c != '\0' ) {  /* Not at EOF */
	      reset_parse_buffer();     
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("garbage at or before char %d, ('%c', \\%03o)",
                             pos, c, c)));
	      YYABORT;
	    }

	    dim = delim_count($1, ',') + 1;
	    
	    if ( (delim_count($3, ',') + 1) != dim ) {
	      reset_parse_buffer();     
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("different point dimensions in (%s) and (%s)",
                             $1, $3)));
	      YYABORT;
	    }
	    if (dim > CUBE_MAX_DIM) {
              reset_parse_buffer();
              ereport(ERROR,
                      (errcode(ERRCODE_SYNTAX_ERROR),
                       errmsg("bad cube representation"),
                       errdetail("more than %d dimensions",
                                 CUBE_MAX_DIM)));
              YYABORT;
            }
	    
	    *((void **)result) = write_box( dim, $1, $3 );
          }
      |

          paren_list {
            int dim;
	    int c = parse_buffer_curr_char();
	    int pos = parse_buffer_pos();

	    if ( c != '\0') {  /* Not at EOF */
	      reset_parse_buffer();     
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("garbage at or before char %d, ('%c', \\%03o)",
                             pos, c, c)));
	      YYABORT;
	    }

	    if ( yychar != YYEOF) {
	      /* There's still a lookahead token to be parsed */
	      reset_parse_buffer();     
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("garbage at or before char %d, ('end of input', \\%03o)",
                             pos, c)));
	      YYABORT;
	    }

            dim = delim_count($1, ',') + 1;
	    if (dim > CUBE_MAX_DIM) {
              reset_parse_buffer();
              ereport(ERROR,
                      (errcode(ERRCODE_SYNTAX_ERROR),
                       errmsg("bad cube representation"),
                       errdetail("more than %d dimensions",
                                 CUBE_MAX_DIM)));
              YYABORT;
            }

	    *((void **)result) = write_point_as_box($1, dim);
          }

      |

          list {
            int dim;
	    int c = parse_buffer_curr_char();
	    int pos = parse_buffer_pos();

	    if ( c != '\0') {  /* Not at EOF */
	      reset_parse_buffer();
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("garbage at or before char %d, ('%c', \\%03o)",
                             pos, c, c)));
	      YYABORT;
	    }

	    if ( yychar != YYEOF) {
	      /* There's still a lookahead token to be parsed */
	      reset_parse_buffer();
          ereport(ERROR,
                  (errcode(ERRCODE_SYNTAX_ERROR),
                   errmsg("bad cube representation"),
                   errdetail("garbage at or before char %d, ('end of input', \\%03o)",
                             pos, c)));
	      YYABORT;
	    }

            dim = delim_count($1, ',') + 1;
	    if (dim > CUBE_MAX_DIM) {
              reset_parse_buffer();
              ereport(ERROR,
                      (errcode(ERRCODE_SYNTAX_ERROR),
                       errmsg("bad cube representation"),
                       errdetail("more than %d dimensions",
                                 CUBE_MAX_DIM)));
              YYABORT;
            }
	    *((void **)result) = write_point_as_box($1, dim);
          }
      ;

paren_list:
          O_PAREN list C_PAREN {
             $$ = $2;
	  }
      ;

list:
          FLOAT {
             $$ = palloc(strlen(parse_buffer()) + 1);
	     strcpy($$, $1);
	  }
      | 
	  list COMMA FLOAT {
             $$ = $1;
	     strcat($$, ",");
	     strcat($$, $3);
	  }
      ;

%%


int cube_yyerror ( char *msg ) {
  char *buf = (char *) palloc(256);
  int position;

  yyclearin;

  if ( !strcmp(msg, "parse error, expecting `$'") ) {
    msg = "expecting end of input";
  }

  position = parse_buffer_pos() > parse_buffer_size() ? parse_buffer_pos() - 1 : parse_buffer_pos();

  snprintf(
	  buf, 
	  256,
	  "%s at or before position %d, character ('%c', \\%03o), input: '%s'", 
	  msg,
	  position,
	  parse_buffer()[position - 1],
	  parse_buffer()[position - 1],
	  parse_buffer()
	  );

  reset_parse_buffer();     
  ereport(ERROR,
          (errcode(ERRCODE_SYNTAX_ERROR),
           errmsg("bad cube representation"),
           errdetail("%s", buf)));

  return 0;
}

static int
delim_count(char *s, char delim)
{
      int        ndelim = 0;

      while ((s = strchr(s, delim)) != NULL)
      {
        ndelim++;
        s++;
      }
      return (ndelim);
}

static NDBOX * 
write_box(unsigned int dim, char *str1, char *str2)
{
  NDBOX * bp;
  char * s;
  int i; 
  int size = offsetof(NDBOX, x[0]) + sizeof(double) * dim * 2;
	    
  bp = palloc(size);
  memset(bp, 0, size);
  bp->size = size;
  bp->dim = dim;
	    
  s = str1;
  bp->x[i=0] = strtod(s, NULL);
  while ((s = strchr(s, ',')) != NULL) {
    s++; i++;
    bp->x[i] = strtod(s, NULL);
  }	
  
  s = str2;
  bp->x[i=dim] = strtod(s, NULL);
  while ((s = strchr(s, ',')) != NULL) {
    s++; i++;
    bp->x[i] = strtod(s, NULL);
  }	

  return(bp);
}


static NDBOX * write_point_as_box(char *str, int dim)
{
  NDBOX * bp;
  int i, size;
  double x;
  char * s = str;
  
  size = offsetof(NDBOX, x[0]) + sizeof(double) * dim * 2;

  bp = palloc(size);
  memset(bp, 0, size);
  bp->size = size;
  bp->dim = dim;
  
  i = 0;
  x = strtod(s, NULL);
  bp->x[0] = x;
  bp->x[dim] = x;
  while ((s = strchr(s, ',')) != NULL) {
    s++; i++;
    x = strtod(s, NULL);
    bp->x[i] = x;
    bp->x[i+dim] = x;
  }	

  return(bp);
}

#include "cubescan.c"
