/* This module defines the parse buffer and routines for setting/reading it */

#include "postgres.h"

#include "utils/elog.h"

static char *       PARSE_BUFFER;
static char *       PARSE_BUFFER_PTR; 
static unsigned int PARSE_BUFFER_SIZE; 
static unsigned int SCANNER_POS;

void         set_parse_buffer( char* s );
void         reset_parse_buffer( void );
int          read_parse_buffer( void );
char *       parse_buffer( void );
char *       parse_buffer_ptr( void );
unsigned int parse_buffer_curr_char( void );
unsigned int parse_buffer_size( void );
unsigned int parse_buffer_pos( void );

extern void seg_flush_scanner_buffer(void); /* defined in segscan.l */

void set_parse_buffer( char* s )
{
  PARSE_BUFFER = s;
  PARSE_BUFFER_SIZE = strlen(s);
  if ( PARSE_BUFFER_SIZE == 0 ) {
    elog(ERROR, "seg_in: can't parse an empty string");
  }
  PARSE_BUFFER_PTR = PARSE_BUFFER;
  SCANNER_POS = 0;
}

void reset_parse_buffer( void )
{
  PARSE_BUFFER_PTR = PARSE_BUFFER;
  SCANNER_POS = 0;
  seg_flush_scanner_buffer();
}

int read_parse_buffer( void )
{
  int c;
  /*
  c = *PARSE_BUFFER_PTR++;
  SCANNER_POS++;
  */
  c = PARSE_BUFFER[SCANNER_POS];
  if(SCANNER_POS < PARSE_BUFFER_SIZE)
    SCANNER_POS++;
  return c;
}

char * parse_buffer( void )
{
  return PARSE_BUFFER;
}

unsigned int parse_buffer_curr_char( void )
{
  return PARSE_BUFFER[SCANNER_POS];
}

char * parse_buffer_ptr( void )
{
  return PARSE_BUFFER_PTR;
}

unsigned int parse_buffer_pos( void )
{
  return SCANNER_POS;
}

unsigned int parse_buffer_size( void )
{
  return PARSE_BUFFER_SIZE;
}


