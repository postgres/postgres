#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for Java JDBC interface
#
# IDENTIFICATION
#    $Id: Makefile,v 1.25 2000/10/12 08:55:23 peter Exp $
#
#-------------------------------------------------------------------------

subdir		= src/interfaces
top_builddir	= ../../..
include		$(top_builddir)/src/Makefile.global

CP		= cp
FIND		= find
IDL2JAVA	= idltojava -fno-cpp -fno-tie
JAR		= jar
JAVA		= java
JAVAC		= javac -g
JAVADOC		= javadoc
RM		= rm -f
TOUCH		= touch

BUILDDRIVER	= sh utils/buildDriver
CHANGELOG	= perl utils/changelog.pl

# This defines how to compile a java class
.java.class:
	$(JAVAC) $<

.SUFFIXES:	.class .java
.PHONY:		all clean doc examples msg

# This is the base directory of the driver. In 7.0, this changed from
# postgresql to org/postgresql
PGBASE		= org/postgresql

# New for 7.1: The jar filename
JARFILE		= postgresql.jar

# Yet another attempt to check the version. In theory, any JVM that fails
# this is breaking the versioning specifications released by Javasoft.
#
all:	utils/CheckVersion.class
	@$(MAKE) `$(JAVA) utils.CheckVersion`

# For 6.5.3 and 7.0+, we need to select the correct JDBC API, so prompt
# for the version.
huho:	
	@echo ------------------------------------------------------------
	@echo Due to problems with some JVMs that dont return a meaningful
	@echo version number, we have had to make the choice of what jdbc
	@echo version is built as a compile time option.
	@echo
	@echo If you are using JDK1.1.x, you will need the JDBC1.2 driver.
	@echo To compile, type:
	@echo "  make jdbc1"
	@echo
	@echo "If you are using JDK1.2 (aka Java2) you need the JDBC2."
	@echo To compile, type:
	@echo "  make jdbc2"
	@echo
	@echo "If you are using the Java2 Enterprise Edition, you can use"
	@echo "either the standard driver above, or compile the standard"
	@echo "extensions version of the driver. Type:"
	@echo "  make enterprise"
	@echo
	@echo Once you have done this, a postgresql.jar file will be
	@echo produced. This file will only work with that particular
	@echo version of virtual machine.
	@echo
	@echo ------------------------------------------------------------

msg:	
	@echo ------------------------------------------------------------
	@echo The JDBC driver has now been built. To make it available to
	@echo other applications, copy the postgresql.jar file to a public
	@echo "place (under unix this could be /usr/local/lib) and add it"
	@echo to the class path.
	@echo
	@echo Then either add -Djdbc.drivers=org.postgresql.Driver to the
	@echo commandline when running your application, or edit the
	@echo "properties file for your application (~/.hotjava/properties"
	@echo "under unix for HotJava), and add a line containing"
	@echo jdbc.drivers=org.postgresql.Driver
	@echo
	@echo More details are in the README file and in the main postgresql
	@echo documentation.
	@echo
	@echo ------------------------------------------------------------
	@echo To build the examples, type:
	@echo "JDBC1:  make examples"
	@echo "JDBC2:  make examples2"
	@echo
	@echo "To build the CORBA example (requires Java2):"
	@echo "  make corba"
	@echo
	@echo "To make the tests, type:"
	@echo "  make tests"
	@echo
	@echo "To build the GUI Shell, type:"
	@echo "  make retepsql"
	@echo
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

# These files are unique to the JDBC2 Enterprise driver
OBJ_ENTER=	$(OBJ_JDBC2) \
		$(PGBASE)/PostgresqlDataSource.class \
		$(PGBASE)/xa/ClientConnection.class \
		$(PGBASE)/xa/TwoPhaseConnection.class \
		$(PGBASE)/xa/TxConnection.class \
		$(PGBASE)/xa/XAConnectionImpl.class \
		$(PGBASE)/xa/XADataSourceImpl.class

# This rule builds the JDBC1 compliant driver
jdbc1:
	$(BUILDDRIVER) $(VERSION) org.postgresql.jdbc1.Connection JDBC1.1 $(PGBASE)/Driver.java
	-$(RM) postgresql.jar
	@$(MAKE) jdbc1real

# This rule does the real work for JDBC1.2, however do not call it directly.
# This is because the JDBC driver relies on the org.postgresql.DriverClass
# class to determine the driver version.
jdbc1real: $(OBJ_COMMON) $(OBJ_JDBC1) $(JARFILE) msg

# This rule builds the JDBC2 compliant driver
jdbc2:
	@$(MAKE) jdbc2internal msg

# This allows us to build the jdbc2 driver without displaying the msg
# refer to the retepsql rule to see why.
jdbc2internal:
	$(BUILDDRIVER) $(VERSION) org.postgresql.jdbc2.Connection Java2 $(PGBASE)/Driver.java
	-$(RM) postgresql.jar
	@$(MAKE) jdbc2real

# This rule does the real work for JDBC2, however do not call it directly.
# This is because the JDBC driver relies on the org.postgresql.DriverClass
# class to determine the driver version.
jdbc2real: $(OBJ_COMMON) $(OBJ_JDBC2) $(JARFILE)

# This rule builds the enterprise edition of the driver
enterprise:
	$(BUILDDRIVER) $(VERSION) org.postgresql.jdbc2.Connection Enterprise $(PGBASE)/Driver.java
	-$(RM) postgresql.jar
	@$(MAKE) enterprisereal

# This rule does the real work for JDBC2 Enterprise Edition, however do not
# call it directly. This is because the JDBC driver relies on the
# org.postgresql.DriverClass class to determine the driver version.
enterprisereal: $(OBJ_COMMON) $(OBJ_ENTER) $(JARFILE)

# We use the old `cmd` notation here as some older shells (mainly sh under
# Solaris) don't recognise the newer $(cmd) syntax.
#
$(JARFILE): $(OBJ) $(OBJ_COMMON)
	$(JAR) -c0f $@ `$(FIND) $(PGBASE) -name "*.class" -print` \
		$(wildcard $(PGBASE)/*.properties)

# This rule removes any temporary and compiled files from the source tree.
#
clean:
	$(FIND) . -name "*~" -exec $(RM) {} \;
	$(FIND) . -name "*.class" -exec $(RM) {} \;
	-$(RM) -rf stock example/corba/stock.built
	-$(RM) postgresql.jar org/postgresql/Driver.java
	-$(RM) -rf Package-postgresql *output
	-$(RM) retepsql.jar manifest

# New for 7.1
install:	$(JARFILE)
	$(CP) $(JARFILE) $(libdir)

# This converts CHANGELOG into an html format - used by peter@retep.org.uk
# to produce an online version
changelog: changelog.html
changelog.html: CHANGELOG
	$(CHANGELOG) $< >$@

#######################################################################
# This helps make workout what classes are from what source files
#
# Java is unlike C in that one source file can generate several
# _Different_ file names
#
$(PGBASE)/Connection.class:		$(PGBASE)/Connection.java
$(PGBASE)/DatabaseMetaData.class:	$(PGBASE)/DatabaseMetaData.java
$(PGBASE)/Driver.class:			$(PGBASE)/Driver.java
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
	example/psql.class \
	example/ImageViewer.class

# These are only valid for JDBC2
EX2=	example/blobtest.class

# These are really test classes not true examples
TESTS=	example/metadata.class \
	example/threadsafe.class \
	example/Unicode.class

# Non functional/obsolete examples
#	example/datestyle.class \
#	example/Objects.class

# This rule builds the examples
examples:	postgresql.jar $(EX)
	@echo ------------------------------------------------------------
	@echo The examples have been built.
	@echo
	@echo For instructions on how to use them, simply run them. For example:
	@echo
	@echo "  java example.basic"
	@echo
	@echo This would display instructions on how to run the example.
	@echo ------------------------------------------------------------
	@echo Available examples:
	@echo
	@echo "  example.basic        Basic JDBC useage"
	@echo "  example.datestyle    Shows how datestyles are handled"
	@echo "  example.ImageViewer  Example application storing images"
	@echo "  example.psql         Simple java implementation of psql"
	@echo " "
	@echo ------------------------------------------------------------
	@echo

examples2:	$(EX2) examples
	@echo "The following JDBC2 only examples have also been built:"
	@echo
	@echo "  example.blobtest     Binary Large Object tests"
	@echo
	@echo ------------------------------------------------------------
	@echo

tests:	$(TESTS)
	@echo ------------------------------------------------------------
	@echo The following tests have been built:
	@echo "  example.metadata     Tests various metadata methods"
	@echo "  example.threadsafe   Tests the driver's thread safety"
	@echo "  example.Unicode      Tests unicode charset support"
	@echo ------------------------------------------------------------
	@echo

example/basic.class:			example/basic.java
example/blobtest.class:			example/blobtest.java
example/datestyle.class:		example/datestyle.java
example/psql.class:			example/psql.java
example/ImageViewer.class:		example/ImageViewer.java
example/threadsafe.class:		example/threadsafe.java
example/metadata.class:			example/metadata.java
example/Unicode.class:                  example/Unicode.java

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
#
# JPGSql	This isn't really an example, but an entire frontend
#		for PostgreSQL via Java & JDBC.
#
# Requirements:	Java2 platform (JDK1.2.x or 1.3)
#
retepsql:	jdbc2internal postgresql.jar
	-$(RM) retepsql.jar
	@$(MAKE) retepsql.jar
	@echo
	@echo "The retepsql application has now been built. To run, simply"
	@echo "type:"
	@echo
	@echo "  java -jar retepsql.jar"
	@echo
	@echo "Note: Some operating systems recognise .jar as an executable,"
	@echo "      so on those (and Windows is one of them), you can simply"
	@echo "      double click the jar file to start it."

# All my classes have this prefix
RETEP=		uk/org/retep

# These classes form part of my personal swing library. I have placed these
# into the public domain, so there are no license issues... enjoy...
RETEPUTILS=	$(RETEP)/swing/SwingApplication.class \
		$(RETEP)/swing/SwingApplication.class \

# This is my postgresql frontend. As it's never been released before, I've
# decided not only to publish it under the same licence as the JDBC driver,
# but also to put it along side the driver. To me it makes sense as it's the
# best example I have on showing how to use the driver ;-)
RETEPSQL=	$(RETEP)/sql/DBConnection.class \
		$(RETEP)/sql/RetepSQL.class \

# Some ancilary files which are included in the jar file
RETEPSQLAUX=	$(RETEP)/icons/ \

# The big rule, this builds the jpgsql.jar file which contains the entire
# application. DONT call this rule directly, but use the retepsql one, as
# it ensures that jdbc is also compiled first.
#
# Note: We include the postgresql.jar contents in this as well. Also the
# manifest entry MUST be immediately after $@ (note the -m option to jar).
#
retepsql.jar:	$(RETEPUTILS) \
		$(RETEPSQL)
	(echo "Manifest-Version: 1.0"; \
	 echo "Created-By: 1.2 (Sun Microsystems Inc.)"; \
	 echo "Main-Class: uk.org.retep.sql.RetepSQL"; \
	) >manifest
	$(JAR) -c0fm $@ manifest \
		`$(FIND) $(PGBASE) -name "*.class" -print` \
		$(wildcard $(PGBASE)/*.properties) \
		`$(FIND) $(RETEP) -name "*.class" -print` \
		$(RETEPSQLAUX)

#######################################################################
