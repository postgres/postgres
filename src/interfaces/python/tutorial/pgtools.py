#! /usr/local/bin/python
# pgtools.py - valuable functions for PostGreSQL tutorial
# written 1995 by Pascal ANDRE

import sys

# number of rows 
scr_size = 24

# waits for a key
def wait_key():
	print "Press <enter>"
	sys.stdin.read(1)
	
# displays a table for a select query result
def display(fields, result):
	# gets cols width
	fmt = []
	sep = '+'
	head = '|'
	for i in range(0, len(fields)):
		max = len(fields[i])
		for j in range(0, len(result)):
			if i < len(result[j]):
				if len(result[j][i]) > max:
					max = len(result[j][i])
		fmt.append(" %%%ds |" % max)
		for j in range(0, max):
			sep = sep + '-'
		sep = sep + '--+'
	for i in range(0, len(fields)):
		head = head + fmt[i] % fields[i]
	print sep + '\n' + head + '\n' + sep
	pos = 6
	for i in range(0, len(result)):
		str = '|'
		for j in range(0, len(result[i])):
			str = str + fmt[j] % result[i][j]
		print str
		pos = pos + 1
		if pos == scr_size:
			print sep
			wait_key()
			print sep + '\n' + head + '\n' + sep
			pos = 6
	print sep
	wait_key()
