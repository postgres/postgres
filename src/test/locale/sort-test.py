#! /usr/bin/env python

import sys, string, locale
locale.setlocale(locale.LC_ALL, "")

if len(sys.argv) <> 2:
   sys.stderr.write("Usage: sort.py filename\n")
   sys.exit(1)

infile = open(sys.argv[1], 'r')
list = infile.readlines()
infile.close()

for i in range(0, len(list)):
   list[i] = list[i][:-1] # chop!

list.sort(locale.strcoll)
print string.join(list, '\n')
