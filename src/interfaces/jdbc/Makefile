#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for Java JDBC interface
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/jdbc/Attic/Makefile,v 1.2 1997/09/29 20:11:42 scrappy Exp $
#
#-------------------------------------------------------------------------

# These are commented out, but would be included in the postgresql source

FIND		= find
JAR		= jar
JAVA		= java
JAVAC		= javac
JAVADOC		= javadoc
RM		= rm -f

# This defines how to compile a java class
.java.class:
	$(JAVAC) $<

.SUFFIXES:	.class .java
.PHONY:		all clean doc

all:	  postgresql.jar

doc:
	$(JAVADOC) -public postgresql

OBJS=	postgresql/CallableStatement.class \
	postgresql/Connection.class \
	postgresql/DatabaseMetaData.class \
	postgresql/Driver.class \
	postgresql/Field.class \
	postgresql/PG_Object.class \
	postgresql/PG_Stream.class \
	postgresql/PGbox.class \
	postgresql/PGcircle.class \
	postgresql/PGlobj.class \
	postgresql/PGlseg.class \
	postgresql/PGpath.class \
	postgresql/PGpoint.class \
	postgresql/PGpolygon.class \
	postgresql/PGtokenizer.class \
	postgresql/PreparedStatement.class \
	postgresql/ResultSet.class \
	postgresql/ResultSetMetaData.class \
	postgresql/Statement.class

postgresql.jar: $(OBJS)
	$(JAR) -c0vf $@ $^

# This rule removes any temporary and compiled files from the source tree.
clean:
	$(FIND) . -name "*~" -exec $(RM) {} \;
	$(FIND) . -name "*.class" -exec $(RM) {} \;
	$(RM) postgresql.jar

#######################################################################
# This helps make workout what classes are from what source files
#
# Java is unlike C in that one source file can generate several
# _Different_ file names
#
postgresql/CallableStatement.class:	postgresql/CallableStatement.java
postgresql/Connection.class:		postgresql/Connection.java
postgresql/DatabaseMetaData.class:	postgresql/DatabaseMetaData.java
postgresql/Driver.class:		postgresql/Driver.java
postgresql/Field.class:			postgresql/Field.java
postgresql/PG_Object.class:		postgresql/PG_Object.java
postgresql/PG_Stream.class:		postgresql/PG_Stream.java
postgresql/PGbox.class:			postgresql/PGbox.java
postgresql/PGcircle.class:		postgresql/PGcircle.java
postgresql/PGlobj.class:		postgresql/PGlobj.java
postgresql/PGlseg.class:		postgresql/PGlseg.java
postgresql/PGpath.class:		postgresql/PGpath.java
postgresql/PGpoint.class:		postgresql/PGpoint.java
postgresql/PGpolygon.class:		postgresql/PGpolygon.java
postgresql/PGtokenizer.class:		postgresql/PGtokenizer.java
postgresql/PreparedStatement.class:	postgresql/PreparedStatement.java
postgresql/ResultSet.class:		postgresql/ResultSet.java
postgresql/ResultSetMetaData.class:	postgresql/ResultSetMetaData.java
postgresql/Statement.class:		postgresql/Statement.java




