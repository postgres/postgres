#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for Java JDBC interface
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/interfaces/jdbc/Attic/Makefile,v 1.4 1998/01/13 02:19:10 scrappy Exp $
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
.PHONY:		all clean doc examples

all:	  postgresql.jar
	@echo ------------------------------------------------------------
	@echo The JDBC driver has now been built. To make it available to
	@echo other applications, copy the postgresql.jar file to a public
	@echo "place (under unix this could be /usr/local/lib) and add it"
	@echo to the class path.
	@echo
	@echo Then either add -Djdbc.drivers=postgresql.Driver to the
	@echo commandline when running your application, or edit the
	@echo "properties file (~/.hotjava/properties under unix), and"
	@echo add a line containing jdbc.drivers=postgresql.Driver
	@echo
	@echo More details are in the README file.
	@echo ------------------------------------------------------------
	@echo To build the examples, type:
	@echo "  make examples"
	@echo ------------------------------------------------------------
	@echo

# This rule builds the javadoc documentation
doc:
	export CLASSPATH=.;\
		$(JAVADOC) -public \
			postgresql \
			postgresql.fastpath \
			postgresql.largeobject

# These classes form the driver. These, and only these are placed into
# the jar file.
OBJS=	postgresql/CallableStatement.class \
	postgresql/Connection.class \
	postgresql/DatabaseMetaData.class \
	postgresql/Driver.class \
	postgresql/Field.class \
	postgresql/PG_Stream.class \
	postgresql/PreparedStatement.class \
	postgresql/ResultSet.class \
	postgresql/ResultSetMetaData.class \
	postgresql/Statement.class \
	postgresql/fastpath/Fastpath.class \
	postgresql/fastpath/FastpathArg.class \
	postgresql/geometric/PGbox.class \
	postgresql/geometric/PGcircle.class \
	postgresql/geometric/PGlseg.class \
	postgresql/geometric/PGpath.class \
	postgresql/geometric/PGpoint.class \
	postgresql/geometric/PGpolygon.class \
	postgresql/largeobject/LargeObject.class \
	postgresql/largeobject/LargeObjectManager.class \
	postgresql/util/PGobject.class \
	postgresql/util/PGtokenizer.class

postgresql.jar: $(OBJS)
	$(JAR) -c0vf $@ $$($(FIND) postgresql -name "*.class" -print)

# This rule removes any temporary and compiled files from the source tree.
clean:
	$(FIND) . -name "*~" -exec $(RM) {} \;
	$(FIND) . -name "*.class" -exec $(RM) {} \;
	$(FIND) . -name "*.html" -exec $(RM) {} \;
	$(RM) postgresql.jar
	-$(RM) -rf Package-postgresql *output

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
postgresql/PG_Stream.class:		postgresql/PG_Stream.java
postgresql/PreparedStatement.class:	postgresql/PreparedStatement.java
postgresql/ResultSet.class:		postgresql/ResultSet.java
postgresql/ResultSetMetaData.class:	postgresql/ResultSetMetaData.java
postgresql/Statement.class:		postgresql/Statement.java
postgresql/fastpath/Fastpath.class:	postgresql/fastpath/Fastpath.java
postgresql/fastpath/FastpathArg.class:	postgresql/fastpath/FastpathArg.java
postgresql/geometric/PGbox.class:	postgresql/geometric/PGbox.java
postgresql/geometric/PGcircle.class:	postgresql/geometric/PGcircle.java
postgresql/geometric/PGlseg.class:	postgresql/geometric/PGlseg.java
postgresql/geometric/PGpath.class:	postgresql/geometric/PGpath.java
postgresql/geometric/PGpoint.class:	postgresql/geometric/PGpoint.java
postgresql/geometric/PGpolygon.class:	postgresql/geometric/PGpolygon.java
postgresql/largeobject/LargeObject.class: postgresql/largeobject/LargeObject.java
postgresql/largeobject/LargeObjectManager.class: postgresql/largeobject/LargeObjectManager.java
postgresql/util/PGobject.class:		postgresql/util/PGobject.java
postgresql/util/PGtokenizer.class:	postgresql/util/PGtokenizer.java

#######################################################################
# These classes are in the example directory, and form the examples
EX=	example/basic.class \
	example/blobtest.class \
	example/datestyle.class \
	example/psql.class \
	example/ImageViewer.class

# This rule builds the examples
examples:	postgresql.jar $(EX)
	@echo ------------------------------------------------------------
	@echo The examples have been built.
	@echo
	@echo For instructions on how to use them, simply run them. For example:
	@echo
	@echo "  java example.blobtest"
	@echo
	@echo This would display instructions on how to run the example.
	@echo ------------------------------------------------------------
	@echo Available examples:
	@echo
	@echo "  example.basic        Basic JDBC useage"
	@echo "  example.blobtest     Binary Large Object tests"
	@echo "  example.datestyle    Shows how datestyles are handled"
	@echo "  example.ImageViewer  Example application storing images"
	@echo "  example.psql         Simple java implementation of psql"
	@echo ------------------------------------------------------------
	@echo

example/basic.class:			example/basic.java
example/blobtest.class:			example/blobtest.java
example/datestyle.class:		example/datestyle.java
example/psql.class:			example/psql.java
example/ImageViewer.class:		example/ImageViewer.java
#######################################################################
