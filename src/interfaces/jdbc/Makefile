#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for Java JDBC interface
#
# IDENTIFICATION
#    $Id: Makefile,v 1.19 2000/04/26 14:19:29 momjian Exp $
#
#-------------------------------------------------------------------------

FIND		= find
IDL2JAVA	= idltojava -fno-cpp -fno-tie
JAR		= jar
JAVA		= java
JAVAC		= javac -g
JAVADOC		= javadoc
RM		= rm -f
TOUCH		= touch

# This defines how to compile a java class
.java.class:
	$(JAVAC) $<

.SUFFIXES:	.class .java
.PHONY:		all clean doc examples msg

# This is the base directory of the driver. In 7.0, this changed from
# postgresql to org/postgresql
PGBASE		= org/postgresql

# In 6.5, the all rule builds the makeVersion class which then calls make using
# the jdbc1 or jdbc2 rules
all:	
	@echo ------------------------------------------------------------
	@echo Due to problems with some JVMs that dont return a meaningful
	@echo version number, we have had to make the choice of what jdbc
	@echo version is built as a compile time option.
	@echo
	@echo If you are using JDK1.1.x, you will need the JDBC1.2 driver.
	@echo To compile, type:
	@echo "  make jdbc1 jar"
	@echo
	@echo "If you are using JDK1.2 (aka Java2) you need the JDBC2."
	@echo To compile, type:
	@echo "  make jdbc2 jar"
	@echo
	@echo Once you have done this, a postgresql.jar file will be
	@echo produced. This file will only work with that particular
	@echo JVM.
	@echo
	@echo ------------------------------------------------------------

msg:	
	@echo ------------------------------------------------------------
	@echo The JDBC driver has now been built. To make it available to
	@echo other applications, copy the postgresql.jar file to a public
	@echo "place (under unix this could be /usr/local/lib) and add it"
	@echo to the class path.
	@echo
	@echo Then either add -Djdbc.drivers=postgresql.Driver to the
	@echo commandline when running your application, or edit the
	@echo "properties file for your application (~/.hotjava/properties"
	@echo "under unix for HotJava), and add a line containing"
	@echo jdbc.drivers=postgresql.Driver
	@echo
	@echo More details are in the README file and in the main postgresql
	@echo documentation.
	@echo
	@echo ------------------------------------------------------------
	@echo To build the examples, type:
	@echo "  make examples"
	@echo
	@echo "To build the CORBA example (requires Java2):"
	@echo "  make corba"
	@echo ------------------------------------------------------------
	@echo

dep depend:

# This rule builds the javadoc documentation
doc:
	export CLASSPATH=.;\
		$(JAVADOC) -public \
			org.postgresql \
			org.postgresql.fastpath \
			org.postgresql.largeobject

# These classes form the driver. These, and only these are placed into
# the jar file.
OBJ_COMMON=	$(PGBASE)/Connection.class \
		$(PGBASE)/Driver.class \
		$(PGBASE)/Field.class \
		$(PGBASE)/PG_Stream.class \
		$(PGBASE)/ResultSet.class \
		$(PGBASE)/errors.properties \
		$(PGBASE)/errors_fr.properties \
		$(PGBASE)/fastpath/Fastpath.class \
		$(PGBASE)/fastpath/FastpathArg.class \
		$(PGBASE)/geometric/PGbox.class \
		$(PGBASE)/geometric/PGcircle.class \
		$(PGBASE)/geometric/PGline.class \
		$(PGBASE)/geometric/PGlseg.class \
		$(PGBASE)/geometric/PGpath.class \
		$(PGBASE)/geometric/PGpoint.class \
		$(PGBASE)/geometric/PGpolygon.class \
		$(PGBASE)/largeobject/LargeObject.class \
		$(PGBASE)/largeobject/LargeObjectManager.class \
		$(PGBASE)/util/PGmoney.class \
		$(PGBASE)/util/PGobject.class \
		$(PGBASE)/util/PGtokenizer.class \
		$(PGBASE)/util/PSQLException.class \
		$(PGBASE)/util/Serialize.class \
		$(PGBASE)/util/UnixCrypt.class

# These files are unique to the JDBC 1 (JDK 1.1) driver
OBJ_JDBC1=	$(PGBASE)/jdbc1/CallableStatement.class \
		$(PGBASE)/jdbc1/Connection.class \
		$(PGBASE)/jdbc1/DatabaseMetaData.class \
		$(PGBASE)/jdbc1/PreparedStatement.class \
		$(PGBASE)/jdbc1/ResultSet.class \
		$(PGBASE)/jdbc1/ResultSetMetaData.class \
		$(PGBASE)/jdbc1/Statement.class

# These files are unique to the JDBC 2 (JDK 2 nee 1.2) driver
OBJ_JDBC2=	$(PGBASE)/jdbc2/ResultSet.class \
		$(PGBASE)/jdbc2/PreparedStatement.class \
		$(PGBASE)/jdbc2/CallableStatement.class \
		$(PGBASE)/jdbc2/Connection.class \
		$(PGBASE)/jdbc2/DatabaseMetaData.class \
		$(PGBASE)/jdbc2/ResultSetMetaData.class \
		$(PGBASE)/jdbc2/Statement.class \
		$(PGBASE)/largeobject/PGblob.class

# This rule builds the JDBC1 compliant driver
jdbc1:
	(echo "package org.postgresql;" ;\
	 echo "public class DriverClass {" ;\
	 echo "public static String connectClass=\"org.postgresql.jdbc1.Connection\";" ;\
	 echo "}" \
	) >$(PGBASE)/DriverClass.java
	@$(MAKE) jdbc1real

jdbc1real: $(PGBASE)/DriverClass.class \
	$(OBJ_COMMON) $(OBJ_JDBC1) postgresql.jar msg

# This rule builds the JDBC2 compliant driver
jdbc2:	
	(echo "package org.postgresql;" ;\
	 echo "public class DriverClass {" ;\
	 echo "public static String connectClass=\"org.postgresql.jdbc2.Connection\";" ;\
	 echo "}" \
	) >$(PGBASE)/DriverClass.java
	@$(MAKE) jdbc2real

jdbc2real: $(PGBASE)/DriverClass.class \
	$(OBJ_COMMON) $(OBJ_JDBC2) postgresql.jar msg

# If you have problems with this rule, replace the $( ) with ` ` as some
# shells (mainly sh under Solaris) doesn't recognise $( )
#
# Note:	This works by storing all compiled classes under the $(PGBASE)
#	directory. We use this later for compiling the dual-mode driver.
#
postgresql.jar: $(OBJ) $(OBJ_COMMON)
	$(JAR) -c0f $@ `$(FIND) $(PGBASE) -name "*.class" -print` \
		$(wildcard $(PGBASE)/*.properties)

# This rule removes any temporary and compiled files from the source tree.
clean:
	$(FIND) . -name "*~" -exec $(RM) {} \;
	$(FIND) . -name "*.class" -exec $(RM) {} \;
	$(FIND) . -name "*.html" -exec $(RM) {} \;
	-$(RM) -rf stock example/corba/stock.built
	-$(RM) postgresql.jar
	-$(RM) -rf Package-postgresql *output

#######################################################################
# This helps make workout what classes are from what source files
#
# Java is unlike C in that one source file can generate several
# _Different_ file names
#
$(PGBASE)/Connection.class:		$(PGBASE)/Connection.java
$(PGBASE)/DatabaseMetaData.class:	$(PGBASE)/DatabaseMetaData.java
$(PGBASE)/Driver.class:		$(PGBASE)/Driver.java
$(PGBASE)/Field.class:			$(PGBASE)/Field.java
$(PGBASE)/PG_Stream.class:		$(PGBASE)/PG_Stream.java
$(PGBASE)/PreparedStatement.class:	$(PGBASE)/PreparedStatement.java
$(PGBASE)/ResultSet.class:		$(PGBASE)/ResultSet.java
$(PGBASE)/ResultSetMetaData.class:	$(PGBASE)/ResultSetMetaData.java
$(PGBASE)/Statement.class:		$(PGBASE)/Statement.java
$(PGBASE)/fastpath/Fastpath.class:	$(PGBASE)/fastpath/Fastpath.java
$(PGBASE)/fastpath/FastpathArg.class:	$(PGBASE)/fastpath/FastpathArg.java
$(PGBASE)/geometric/PGbox.class:	$(PGBASE)/geometric/PGbox.java
$(PGBASE)/geometric/PGcircle.class:	$(PGBASE)/geometric/PGcircle.java
$(PGBASE)/geometric/PGlseg.class:	$(PGBASE)/geometric/PGlseg.java
$(PGBASE)/geometric/PGpath.class:	$(PGBASE)/geometric/PGpath.java
$(PGBASE)/geometric/PGpoint.class:	$(PGBASE)/geometric/PGpoint.java
$(PGBASE)/geometric/PGpolygon.class:	$(PGBASE)/geometric/PGpolygon.java
$(PGBASE)/largeobject/LargeObject.class: $(PGBASE)/largeobject/LargeObject.java
$(PGBASE)/largeobject/LargeObjectManager.class: $(PGBASE)/largeobject/LargeObjectManager.java
$(PGBASE)/util/PGmoney.class:		$(PGBASE)/util/PGmoney.java
$(PGBASE)/util/PGobject.class:		$(PGBASE)/util/PGobject.java
$(PGBASE)/util/PGtokenizer.class:	$(PGBASE)/util/PGtokenizer.java
$(PGBASE)/util/Serialize.class:	$(PGBASE)/util/Serialize.java
$(PGBASE)/util/UnixCrypt.class:	$(PGBASE)/util/UnixCrypt.java

#######################################################################
# These classes are in the example directory, and form the examples
EX=	example/basic.class \
	example/blobtest.class \
	example/datestyle.class \
	example/psql.class \
	example/ImageViewer.class \
	example/metadata.class \
	example/threadsafe.class
#	example/Objects.class

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
	@echo "  example.Objects      Demonstrates Object Serialisation"
	@echo " "
	@echo These are not really examples, but tests various parts of the driver
	@echo "  example.metadata     Tests various metadata methods"
	@echo "  example.threadsafe   Tests the driver's thread safety"
	@echo ------------------------------------------------------------
	@echo

example/basic.class:			example/basic.java
example/blobtest.class:			example/blobtest.java
example/datestyle.class:		example/datestyle.java
example/psql.class:			example/psql.java
example/ImageViewer.class:		example/ImageViewer.java
example/threadsafe.class:		example/threadsafe.java
example/metadata.class:			example/metadata.java

#######################################################################
#
# CORBA		This extensive example shows how to integrate PostgreSQL
#		JDBC & CORBA.

CORBASRC = $(wildcard example/corba/*.java)
CORBAOBJ = $(subst .java,.class,$(CORBASRC))

corba: jdbc2 example/corba/stock.built $(CORBAOBJ)
	@echo -------------------------------------------------------
	@echo The corba example has been built. Before running, you
	@echo will need to read the example/corba/readme file on how
	@echo to run the example.
	@echo

#
# This compiles our idl file and the stubs
#
# Note: The idl file is in example/corba, but it builds a directory under
# the current one. For safety, we delete that directory before running
# idltojava
#
example/corba/stock.built: example/corba/stock.idl
	-rm -rf stock
	$(IDL2JAVA) $<
	$(JAVAC) stock/*.java
	$(TOUCH) $@

# tip: we cant use $(wildcard stock/*.java) in the above rule as a race
#      condition occurs, where javac is passed no arguments
#######################################################################
