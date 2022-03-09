#!/usr/bin/python
# -*- coding: utf-8 -*-
#
# This script builds unaccent.rules on standard output when given the
# contents of UnicodeData.txt [1] and Latin-ASCII.xml [2] given as
# arguments. Optionally includes ligature expansion and Unicode CLDR
# Latin-ASCII transliterator, enabled by default, this can be disabled
# with "--no-ligatures-expansion" command line option.
#
# The approach is to use the Unicode decomposition data to identify
# precomposed codepoints that are equivalent to a ligature of several
# letters, or a base letter with any number of diacritical marks.
#
# This approach handles most letters with diacritical marks and some
# ligatures.  However, several characters (notably a majority of
# ligatures) don't have decomposition. To handle all these cases, one can
# use a standard Unicode transliterator available in Common Locale Data
# Repository (CLDR): Latin-ASCII.  This transliterator associates Unicode
# characters to ASCII-range equivalent.  Unless "--no-ligatures-expansion"
# option is enabled, the XML file of this transliterator [2] -- given as a
# command line argument -- will be parsed and used.
#
# Ideally you should use the latest release for each data set.  This
# script is compatible with at least CLDR release 29.
#
# [1] https://www.unicode.org/Public/${UNICODE_VERSION}/ucd/UnicodeData.txt
# [2] https://raw.githubusercontent.com/unicode-org/cldr/${TAG}/common/transforms/Latin-ASCII.xml

import argparse
import codecs
import re
import sys
import xml.etree.ElementTree as ET

sys.stdout = codecs.getwriter('utf8')(sys.stdout.buffer)

# The ranges of Unicode characters that we consider to be "plain letters".
# For now we are being conservative by including only Latin and Greek.  This
# could be extended in future based on feedback from people with relevant
# language knowledge.
PLAIN_LETTER_RANGES = ((ord('a'), ord('z')),  # Latin lower case
                       (ord('A'), ord('Z')),  # Latin upper case
                       (0x03b1, 0x03c9),      # GREEK SMALL LETTER ALPHA, GREEK SMALL LETTER OMEGA
                       (0x0391, 0x03a9))      # GREEK CAPITAL LETTER ALPHA, GREEK CAPITAL LETTER OMEGA

# Combining marks follow a "base" character, and result in a composite
# character. Example: "U&'A\0300'"produces "À".There are three types of
# combining marks: enclosing (Me), non-spacing combining (Mn), spacing
# combining (Mc). We identify the ranges of marks we feel safe removing.
# References:
#   https://en.wikipedia.org/wiki/Combining_character
#   https://www.unicode.org/charts/PDF/U0300.pdf
#   https://www.unicode.org/charts/PDF/U20D0.pdf
COMBINING_MARK_RANGES = ((0x0300, 0x0362),   # Mn: Accents, IPA
                         (0x20dd, 0x20E0),   # Me: Symbols
                         (0x20e2, 0x20e4),)  # Me: Screen, keycap, triangle


def print_record(codepoint, letter):
    if letter:
        output = chr(codepoint) + "\t" + letter
    else:
        output = chr(codepoint)

    print(output)


class Codepoint:
    def __init__(self, id, general_category, combining_ids):
        self.id = id
        self.general_category = general_category
        self.combining_ids = combining_ids


def is_mark_to_remove(codepoint):
    """Return true if this is a combining mark to remove."""
    if not is_mark(codepoint):
        return False

    for begin, end in COMBINING_MARK_RANGES:
        if codepoint.id >= begin and codepoint.id <= end:
            return True
    return False


def is_plain_letter(codepoint):
    """Return true if codepoint represents a "plain letter"."""
    for begin, end in PLAIN_LETTER_RANGES:
        if codepoint.id >= begin and codepoint.id <= end:
            return True
    return False


def is_mark(codepoint):
    """Returns true for diacritical marks (combining codepoints)."""
    return codepoint.general_category in ("Mn", "Me", "Mc")


def is_letter_with_marks(codepoint, table):
    """Returns true for letters combined with one or more marks."""
    # See https://www.unicode.org/reports/tr44/tr44-14.html#General_Category_Values

    # Letter may have no combining characters, in which case it has
    # no marks.
    if len(codepoint.combining_ids) == 1:
        return False

    # A letter without diacritical marks has none of them.
    if any(is_mark(table[i]) for i in codepoint.combining_ids[1:]) is False:
        return False

    # Check if the base letter of this letter has marks.
    codepoint_base = codepoint.combining_ids[0]
    if is_plain_letter(table[codepoint_base]) is False and \
       is_letter_with_marks(table[codepoint_base], table) is False:
        return False

    return True


def is_letter(codepoint, table):
    """Return true for letter with or without diacritical marks."""
    return is_plain_letter(codepoint) or is_letter_with_marks(codepoint, table)


def get_plain_letter(codepoint, table):
    """Return the base codepoint without marks. If this codepoint has more
    than one combining character, do a recursive lookup on the table to
    find out its plain base letter."""
    if is_letter_with_marks(codepoint, table):
        if len(table[codepoint.combining_ids[0]].combining_ids) > 1:
            return get_plain_letter(table[codepoint.combining_ids[0]], table)
        elif is_plain_letter(table[codepoint.combining_ids[0]]):
            return table[codepoint.combining_ids[0]]

        # Should not come here
        assert(False)
    elif is_plain_letter(codepoint):
        return codepoint

    # Should not come here
    assert(False)


def is_ligature(codepoint, table):
    """Return true for letters combined with letters."""
    return all(is_letter(table[i], table) for i in codepoint.combining_ids)


def get_plain_letters(codepoint, table):
    """Return a list of plain letters from a ligature."""
    assert(is_ligature(codepoint, table))
    return [get_plain_letter(table[id], table) for id in codepoint.combining_ids]


def parse_cldr_latin_ascii_transliterator(latinAsciiFilePath):
    """Parse the XML file and return a set of tuples (src, trg), where "src"
    is the original character and "trg" the substitute."""
    charactersSet = set()

    # RegEx to parse rules
    rulePattern = re.compile(r'^(?:(.)|(\\u[0-9a-fA-F]{4})) \u2192 (?:\'(.+)\'|(.+)) ;')

    # construct tree from XML
    transliterationTree = ET.parse(latinAsciiFilePath)
    transliterationTreeRoot = transliterationTree.getroot()

    # Fetch all the transliteration rules.  Since release 29 of Latin-ASCII.xml
    # all the transliteration rules are located in a single tRule block with
    # all rules separated into separate lines.
    blockRules = transliterationTreeRoot.findall("./transforms/transform/tRule")
    assert(len(blockRules) == 1)

    # Split the block of rules into one element per line.
    rules = blockRules[0].text.splitlines()

    # And finish the processing of each individual rule.
    for rule in rules:
        matches = rulePattern.search(rule)

        # The regular expression capture four groups corresponding
        # to the characters.
        #
        # Group 1: plain "src" char. Empty if group 2 is not.
        # Group 2: unicode-escaped "src" char (e.g. "\u0110"). Empty if group 1 is not.
        #
        # Group 3: plain "trg" char. Empty if group 4 is not.
        # Group 4: plain "trg" char between quotes. Empty if group 3 is not.
        if matches is not None:
            src = matches.group(1) if matches.group(1) is not None else bytes(matches.group(2), 'UTF-8').decode('unicode-escape')
            trg = matches.group(3) if matches.group(3) is not None else matches.group(4)

            # "'" and """ are escaped
            trg = trg.replace("\\'", "'").replace('\\"', '"')

            # the parser of unaccent only accepts non-whitespace characters
            # for "src" and "trg" (see unaccent.c)
            if not src.isspace() and not trg.isspace():
                charactersSet.add((ord(src), trg))

    return charactersSet


def special_cases():
    """Returns the special cases which are not handled by other methods"""
    charactersSet = set()

    # Cyrillic
    charactersSet.add((0x0401, "\u0415"))  # CYRILLIC CAPITAL LETTER IO
    charactersSet.add((0x0451, "\u0435"))  # CYRILLIC SMALL LETTER IO

    # Symbols of "Letterlike Symbols" Unicode Block (U+2100 to U+214F)
    charactersSet.add((0x2103, "\xb0C"))   # DEGREE CELSIUS
    charactersSet.add((0x2109, "\xb0F"))   # DEGREE FAHRENHEIT
    charactersSet.add((0x2117, "(P)"))     # SOUND RECORDING COPYRIGHT

    return charactersSet


def main(args):
    # https://www.unicode.org/reports/tr44/tr44-14.html#Character_Decomposition_Mappings
    decomposition_type_pattern = re.compile(" *<[^>]*> *")

    table = {}
    all = []

    # unordered set for ensure uniqueness
    charactersSet = set()

    # read file UnicodeData.txt
    with codecs.open(
      args.unicodeDataFilePath, mode='r', encoding='UTF-8',
      ) as unicodeDataFile:
        # read everything we need into memory
        for line in unicodeDataFile:
            fields = line.split(";")
            if len(fields) > 5:
                # https://www.unicode.org/reports/tr44/tr44-14.html#UnicodeData.txt
                general_category = fields[2]
                decomposition = fields[5]
                decomposition = re.sub(decomposition_type_pattern, ' ', decomposition)
                id = int(fields[0], 16)
                combining_ids = [int(s, 16) for s in decomposition.split(" ") if s != ""]
                codepoint = Codepoint(id, general_category, combining_ids)
                table[id] = codepoint
                all.append(codepoint)

    # walk through all the codepoints looking for interesting mappings
    for codepoint in all:
        if codepoint.general_category.startswith('L') and \
           len(codepoint.combining_ids) > 1:
            if is_letter_with_marks(codepoint, table):
                charactersSet.add((codepoint.id,
                                   chr(get_plain_letter(codepoint, table).id)))
            elif args.noLigaturesExpansion is False and is_ligature(codepoint, table):
                charactersSet.add((codepoint.id,
                                   "".join(chr(combining_codepoint.id)
                                           for combining_codepoint
                                           in get_plain_letters(codepoint, table))))
        elif is_mark_to_remove(codepoint):
            charactersSet.add((codepoint.id, None))

    # add CLDR Latin-ASCII characters
    if not args.noLigaturesExpansion:
        charactersSet |= parse_cldr_latin_ascii_transliterator(args.latinAsciiFilePath)
        charactersSet |= special_cases()

    # sort for more convenient display
    charactersList = sorted(charactersSet, key=lambda characterPair: characterPair[0])

    for characterPair in charactersList:
        print_record(characterPair[0], characterPair[1])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='This script builds unaccent.rules on standard output when given the contents of UnicodeData.txt and Latin-ASCII.xml given as arguments.')
    parser.add_argument("--unicode-data-file", help="Path to formatted text file corresponding to UnicodeData.txt.", type=str, required=True, dest='unicodeDataFilePath')
    parser.add_argument("--latin-ascii-file", help="Path to XML file from Unicode Common Locale Data Repository (CLDR) corresponding to Latin-ASCII transliterator (Latin-ASCII.xml).", type=str, dest='latinAsciiFilePath')
    parser.add_argument("--no-ligatures-expansion", help="Do not expand ligatures and do not use Unicode CLDR Latin-ASCII transliterator. By default, this option is not enabled and \"--latin-ascii-file\" argument is required. If this option is enabled, \"--latin-ascii-file\" argument is optional and ignored.", action="store_true", dest='noLigaturesExpansion')
    args = parser.parse_args()

    if args.noLigaturesExpansion is False and args.latinAsciiFilePath is None:
        sys.stderr.write('You must specify the path to Latin-ASCII transliterator file with \"--latin-ascii-file\" option or use \"--no-ligatures-expansion\" option. Use \"-h\" option for help.')
        sys.exit(1)

    main(args)
