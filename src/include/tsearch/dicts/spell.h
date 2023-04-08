/*-------------------------------------------------------------------------
 *
 * spell.h
 *
 * Declarations for ISpell dictionary
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/tsearch/dicts/spell.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef __SPELL_H__
#define __SPELL_H__

#include "regex/regex.h"
#include "tsearch/dicts/regis.h"
#include "tsearch/ts_public.h"

/*
 * SPNode and SPNodeData are used to represent prefix tree (Trie) to store
 * a words list.
 */
struct SPNode;

typedef struct
{
	uint32		val:8,
				isword:1,
	/* Stores compound flags listed below */
				compoundflag:4,
	/* Reference to an entry of the AffixData field */
				affix:19;
	struct SPNode *node;
} SPNodeData;

/*
 * Names of FF_ are correlated with Hunspell options in affix file
 * https://hunspell.github.io/
 */
#define FF_COMPOUNDONLY		0x01
#define FF_COMPOUNDBEGIN	0x02
#define FF_COMPOUNDMIDDLE	0x04
#define FF_COMPOUNDLAST		0x08
#define FF_COMPOUNDFLAG		( FF_COMPOUNDBEGIN | FF_COMPOUNDMIDDLE | \
							FF_COMPOUNDLAST )
#define FF_COMPOUNDFLAGMASK		0x0f

typedef struct SPNode
{
	uint32		length;
	SPNodeData	data[FLEXIBLE_ARRAY_MEMBER];
} SPNode;

#define SPNHDRSZ	(offsetof(SPNode,data))

/*
 * Represents an entry in a words list.
 */
typedef struct spell_struct
{
	union
	{
		/*
		 * flag is filled in by NIImportDictionary(). After
		 * NISortDictionary(), d is used instead of flag.
		 */
		char	   *flag;
		/* d is used in mkSPNode() */
		struct
		{
			/* Reference to an entry of the AffixData field */
			int			affix;
			/* Length of the word */
			int			len;
		}			d;
	}			p;
	char		word[FLEXIBLE_ARRAY_MEMBER];
} SPELL;

#define SPELLHDRSZ	(offsetof(SPELL, word))

/*
 * Represents an entry in an affix list.
 */
typedef struct aff_struct
{
	char	   *flag;
	/* FF_SUFFIX or FF_PREFIX */
	uint32		type:1,
				flagflags:7,
				issimple:1,
				isregis:1,
				replen:14;
	char	   *find;
	char	   *repl;
	union
	{
		/*
		 * Arrays of AFFIX are moved and sorted.  We'll use a pointer to
		 * regex_t to keep this struct small, and avoid assuming that regex_t
		 * is movable.
		 */
		regex_t    *pregex;
		Regis		regis;
	}			reg;
} AFFIX;

/*
 * affixes use dictionary flags too
 */
#define FF_COMPOUNDPERMITFLAG	0x10
#define FF_COMPOUNDFORBIDFLAG	0x20
#define FF_CROSSPRODUCT			0x40

/*
 * Don't change the order of these. Initialization sorts by these,
 * and expects prefixes to come first after sorting.
 */
#define FF_SUFFIX				1
#define FF_PREFIX				0

/*
 * AffixNode and AffixNodeData are used to represent prefix tree (Trie) to store
 * an affix list.
 */
struct AffixNode;

typedef struct
{
	uint32		val:8,
				naff:24;
	AFFIX	  **aff;
	struct AffixNode *node;
} AffixNodeData;

typedef struct AffixNode
{
	uint32		isvoid:1,
				length:31;
	AffixNodeData data[FLEXIBLE_ARRAY_MEMBER];
} AffixNode;

#define ANHRDSZ		   (offsetof(AffixNode, data))

typedef struct
{
	char	   *affix;
	int			len;
	bool		issuffix;
} CMPDAffix;

/*
 * Type of encoding affix flags in Hunspell dictionaries
 */
typedef enum
{
	FM_CHAR,					/* one character (like ispell) */
	FM_LONG,					/* two characters */
	FM_NUM						/* number, >= 0 and < 65536 */
} FlagMode;

/*
 * Structure to store Hunspell options. Flag representation depends on flag
 * type. These flags are about support of compound words.
 */
typedef struct CompoundAffixFlag
{
	union
	{
		/* Flag name if flagMode is FM_CHAR or FM_LONG */
		char	   *s;
		/* Flag name if flagMode is FM_NUM */
		uint32		i;
	}			flag;
	/* we don't have a bsearch_arg version, so, copy FlagMode */
	FlagMode	flagMode;
	uint32		value;
} CompoundAffixFlag;

#define FLAGNUM_MAXSIZE		(1 << 16)

typedef struct
{
	int			maffixes;
	int			naffixes;
	AFFIX	   *Affix;

	AffixNode  *Suffix;
	AffixNode  *Prefix;

	SPNode	   *Dictionary;
	/* Array of sets of affixes */
	char	  **AffixData;
	int			lenAffixData;
	int			nAffixData;
	bool		useFlagAliases;

	CMPDAffix  *CompoundAffix;

	bool		usecompound;
	FlagMode	flagMode;

	/*
	 * All follow fields are actually needed only for initialization
	 */

	/* Array of Hunspell options in affix file */
	CompoundAffixFlag *CompoundAffixFlags;
	/* number of entries in CompoundAffixFlags array */
	int			nCompoundAffixFlag;
	/* allocated length of CompoundAffixFlags array */
	int			mCompoundAffixFlag;

	/*
	 * Remaining fields are only used during dictionary construction; they are
	 * set up by NIStartBuild and cleared by NIFinishBuild.
	 */
	MemoryContext buildCxt;		/* temp context for construction */

	/* Temporary array of all words in the dict file */
	SPELL	  **Spell;
	int			nspell;			/* number of valid entries in Spell array */
	int			mspell;			/* allocated length of Spell array */

	/* These are used to allocate "compact" data without palloc overhead */
	char	   *firstfree;		/* first free address (always maxaligned) */
	size_t		avail;			/* free space remaining at firstfree */
} IspellDict;

extern TSLexeme *NINormalizeWord(IspellDict *Conf, char *word);

extern void NIStartBuild(IspellDict *Conf);
extern void NIImportAffixes(IspellDict *Conf, const char *filename);
extern void NIImportDictionary(IspellDict *Conf, const char *filename);
extern void NISortDictionary(IspellDict *Conf);
extern void NISortAffixes(IspellDict *Conf);
extern void NIFinishBuild(IspellDict *Conf);

#endif
