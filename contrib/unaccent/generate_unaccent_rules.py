#!/usr/bin/python2
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
# [1] http://unicode.org/Public/8.0.0/ucd/UnicodeData.txt
# [2] http://unicode.org/cldr/trac/export/12304/tags/release-28/common/transforms/Latin-ASCII.xml


import re
import argparse
import sys
import xml.etree.ElementTree as ET

def print_record(codepoint, letter):
    print (unichr(codepoint) + "\t" + letter).encode("UTF-8")

class Codepoint:
    def __init__(self, id, general_category, combining_ids):
        self.id = id
        self.general_category = general_category
        self.combining_ids = combining_ids

def is_plain_letter(codepoint):
    """Return true if codepoint represents a plain ASCII letter."""
    return (codepoint.id >= ord('a') and codepoint.id <= ord('z')) or \
           (codepoint.id >= ord('A') and codepoint.id <= ord('Z'))

def is_mark(codepoint):
    """Returns true for diacritical marks (combining codepoints)."""
    return codepoint.general_category in ("Mn", "Me", "Mc")

def is_letter_with_marks(codepoint, table):
    """Returns true for plain letters combined with one or more marks."""
    # See http://www.unicode.org/reports/tr44/tr44-14.html#General_Category_Values
    return len(codepoint.combining_ids) > 1 and \
           is_plain_letter(table[codepoint.combining_ids[0]]) and \
           all(is_mark(table[i]) for i in codepoint.combining_ids[1:])

def is_letter(codepoint, table):
    """Return true for letter with or without diacritical marks."""
    return is_plain_letter(codepoint) or is_letter_with_marks(codepoint, table)

def get_plain_letter(codepoint, table):
    """Return the base codepoint without marks."""
    if is_letter_with_marks(codepoint, table):
        return table[codepoint.combining_ids[0]]
    elif is_plain_letter(codepoint):
        return codepoint
    else:
        raise "mu"

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
    rulePattern = re.compile(ur'^(?:(.)|(\\u[0-9a-fA-F]{4})) \u2192 (?:\'(.+)\'|(.+)) ;')

    # construct tree from XML
    transliterationTree = ET.parse(latinAsciiFilePath)
    transliterationTreeRoot = transliterationTree.getroot()

    for rule in transliterationTreeRoot.findall("./transforms/transform/tRule"):
        matches = rulePattern.search(rule.text)

        # The regular expression capture four groups corresponding
        # to the characters.
        #
        # Group 1: plain "src" char. Empty if group 2 is not.
        # Group 2: unicode-escaped "src" char (e.g. "\u0110"). Empty if group 1 is not.
        #
        # Group 3: plain "trg" char. Empty if group 4 is not.
        # Group 4: plain "trg" char between quotes. Empty if group 3 is not.
        if matches is not None:
            src = matches.group(1) if matches.group(1) is not None else matches.group(2).decode('unicode-escape')
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
    charactersSet.add((0x0401, u"\u0415")) # CYRILLIC CAPITAL LETTER IO
    charactersSet.add((0x0451, u"\u0435")) # CYRILLIC SMALL LETTER IO

    # Symbols of "Letterlike Symbols" Unicode Block (U+2100 to U+214F)
    charactersSet.add((0x2103, u"\xb0C")) # DEGREE CELSIUS
    charactersSet.add((0x2109, u"\xb0F")) # DEGREE FAHRENHEIT
    charactersSet.add((0x2117, "(P)")) # SOUND RECORDING COPYRIGHT

    return charactersSet

def main(args):
    # http://www.unicode.org/reports/tr44/tr44-14.html#Character_Decomposition_Mappings
    decomposition_type_pattern = re.compile(" *<[^>]*> *")

    table = {}
    all = []

    # unordered set for ensure uniqueness
    charactersSet = set()

    # read file UnicodeData.txt
    unicodeDataFile = open(args.unicodeDataFilePath, 'r')

    # read everything we need into memory
    for line in unicodeDataFile:
        fields = line.split(";")
        if len(fields) > 5:
            # http://www.unicode.org/reports/tr44/tr44-14.html#UnicodeData.txt
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
                             "".join(unichr(combining_codepoint.id)
                                     for combining_codepoint \
                                     in get_plain_letters(codepoint, table))))

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
    parser.add_argument("--unicode-data-file", help="Path to formatted text file corresponding to UnicodeData.txt. See <http://unicode.org/Public/8.0.0/ucd/UnicodeData.txt>.", type=str, required=True, dest='unicodeDataFilePath')
    parser.add_argument("--latin-ascii-file", help="Path to XML file from Unicode Common Locale Data Repository (CLDR) corresponding to Latin-ASCII transliterator (Latin-ASCII.xml). See <http://unicode.org/cldr/trac/export/12304/tags/release-28/common/transforms/Latin-ASCII.xml>.", type=str, dest='latinAsciiFilePath')
    parser.add_argument("--no-ligatures-expansion", help="Do not expand ligatures and do not use Unicode CLDR Latin-ASCII transliterator. By default, this option is not enabled and \"--latin-ascii-file\" argument is required. If this option is enabled, \"--latin-ascii-file\" argument is optional and ignored.", action="store_true", dest='noLigaturesExpansion')
    args = parser.parse_args()

    if args.noLigaturesExpansion is False and args.latinAsciiFilePath is None:
        sys.stderr.write('You must specify the path to Latin-ASCII transliterator file with \"--latin-ascii-file\" option or use \"--no-ligatures-expansion\" option. Use \"-h\" option for help.')
        sys.exit(1)

    main(args)
