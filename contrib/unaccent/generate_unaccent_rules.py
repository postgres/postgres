#!/usr/bin/python
#
# This script builds unaccent.rules on standard output when given the
# contents of UnicodeData.txt[1] on standard input.  Optionally includes
# ligature expansion, if --expand-ligatures is given on the command line.
#
# The approach is to use the Unicode decomposition data to identify
# precomposed codepoints that are equivalent to a ligature of several
# letters, or a base letter with any number of diacritical marks.
# There is also a small set of special cases for codepoints that we
# traditionally support even though Unicode doesn't consider them to
# be ligatures or letters with marks.
#
# [1] http://unicode.org/Public/7.0.0/ucd/UnicodeData.txt

import re
import sys

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

def main(expand_ligatures):
    # http://www.unicode.org/reports/tr44/tr44-14.html#Character_Decomposition_Mappings
    decomposition_type_pattern = re.compile(" *<[^>]*> *")

    table = {}
    all = []

    # read everything we need into memory
    for line in sys.stdin.readlines():
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
                print_record(codepoint.id,
                             chr(get_plain_letter(codepoint, table).id))
            elif expand_ligatures and is_ligature(codepoint, table):
                print_record(codepoint.id,
                             "".join(unichr(combining_codepoint.id)
                                     for combining_codepoint \
                                     in get_plain_letters(codepoint, table)))

    # some special cases
    print_record(0x00d8, "O") # LATIN CAPITAL LETTER O WITH STROKE
    print_record(0x00f8, "o") # LATIN SMALL LETTER O WITH STROKE
    print_record(0x0110, "D") # LATIN CAPITAL LETTER D WITH STROKE
    print_record(0x0111, "d") # LATIN SMALL LETTER D WITH STROKE
    print_record(0x0131, "i") # LATIN SMALL LETTER DOTLESS I
    print_record(0x0126, "H") # LATIN CAPITAL LETTER H WITH STROKE
    print_record(0x0127, "h") # LATIN SMALL LETTER H WITH STROKE
    print_record(0x0141, "L") # LATIN CAPITAL LETTER L WITH STROKE
    print_record(0x0142, "l") # LATIN SMALL LETTER L WITH STROKE
    print_record(0x0149, "'n") # LATIN SMALL LETTER N PRECEDED BY APOSTROPHE
    print_record(0x0166, "T") # LATIN CAPITAL LETTER T WITH STROKE
    print_record(0x0167, "t") # LATIN SMALL LETTER t WITH STROKE
    print_record(0x0401, u"\u0415") # CYRILLIC CAPITAL LETTER IO
    print_record(0x0451, u"\u0435") # CYRILLIC SMALL LETTER IO
    if expand_ligatures:
        print_record(0x00c6, "AE") # LATIN CAPITAL LETTER AE
        print_record(0x00df, "ss") # LATIN SMALL LETTER SHARP S
        print_record(0x00e6, "ae") # LATIN SMALL LETTER AE
        print_record(0x0152, "OE") # LATIN CAPITAL LIGATURE OE
        print_record(0x0153, "oe") # LATIN SMALL LIGATURE OE

if __name__ == "__main__":
    main(len(sys.argv) == 2 and sys.argv[1] == "--expand-ligatures")
