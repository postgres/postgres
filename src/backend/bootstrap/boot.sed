#
# lex.sed - sed rules to remove conflicts between the 
#               bootstrap backend interface LEX scanner and the
#               normal backend SQL LEX scanner
#
# $Header: /cvsroot/pgsql/src/backend/bootstrap/Attic/boot.sed,v 1.1.1.1 1996/07/09 06:21:14 scrappy Exp $
#
s/^yy/Int_yy/g
s/\([^a-zA-Z0-9_]\)yy/\1Int_yy/g
