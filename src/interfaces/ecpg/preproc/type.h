#include <ecpgtype.h>

struct ECPGtype;
struct ECPGrecord_member {
    char *		name;
    struct ECPGtype *	typ;
    struct ECPGrecord_member * next;
};

struct ECPGtype {
    enum ECPGttype	typ;
    unsigned short	size;	/* For array it is the number of elements.
				 * For varchar it is the maxsize of the area.
				 */
    union {
	struct ECPGtype * element;	/* For an array this is the type of the 
				 * element */

	struct ECPGrecord_member * members;
				/* A pointer to a list of members. */
    } u;
};

/* Everything is malloced. */
struct ECPGrecord_member * ECPGmake_record_member(char *, struct ECPGtype *, struct ECPGrecord_member **);
struct ECPGtype * ECPGmake_simple_type(enum ECPGttype);
struct ECPGtype * ECPGmake_varchar_type(enum ECPGttype, unsigned short);
struct ECPGtype * ECPGmake_array_type(struct ECPGtype *, unsigned short);
struct ECPGtype * ECPGmake_record_type(struct ECPGrecord_member *);

/* Frees a type. */
void ECPGfree_record_member(struct ECPGrecord_member *);
void ECPGfree_type(struct ECPGtype *);

/* Dump a type.
   The type is dumped as:
   type-tag <comma> reference-to-variable <comma> arrsize <comma> size <comma>
   Where:
   type-tag is one of the simple types or varchar.
   reference-to-variable can be a reference to a struct element.
   arrsize is the size of the array in case of array fetches. Otherwise 0.
   size is the maxsize in case it is a varchar. Otherwise it is the size of 
       the variable (required to do array fetches of records).
 */
void ECPGdump_a_type(FILE *, const char * name, struct ECPGtype *, const char *);

/* A simple struct to keep a variable and its type. */
struct ECPGtemp_type {
    struct ECPGtype * typ;
    const char * name;
};

extern const char * ECPGtype_name(enum ECPGttype typ);
