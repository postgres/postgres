package org.postgresql.jdbc1;

// IMPORTANT NOTE: This file implements the JDBC 1 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 2 class in the
// org.postgresql.jdbc2 package.

import java.sql.*;
import java.util.*;
import org.postgresql.Field;

/**
 * This class provides information about the database as a whole.
 *
 * <p>Many of the methods here return lists of information in ResultSets.  You
 * can use the normal ResultSet methods such as getString and getInt to 
 * retrieve the data from these ResultSets.  If a given form of metadata is
 * not available, these methods should throw a SQLException.
 *
 * <p>Some of these methods take arguments that are String patterns.  These
 * arguments all have names such as fooPattern.  Within a pattern String,
 * "%" means match any substring of 0 or more characters, and "_" means
 * match any one character.  Only metadata entries matching the search
 * pattern are returned.  if a search pattern argument is set to a null
 * ref, it means that argument's criteria should be dropped from the
 * search.
 *
 * <p>A SQLException will be throws if a driver does not support a meta
 * data method.  In the case of methods that return a ResultSet, either
 * a ResultSet (which may be empty) is returned or a SQLException is
 * thrown.
 *
 * @see java.sql.DatabaseMetaData
 */
public class DatabaseMetaData implements java.sql.DatabaseMetaData 
{
  Connection connection;		// The connection association
  
  // These define various OID's. Hopefully they will stay constant.
  static final int iVarcharOid = 1043;	// OID for varchar
  static final int iBoolOid = 16;	// OID for bool
  static final int iInt2Oid = 21;	// OID for int2
  static final int iInt4Oid = 23;	// OID for int4
  static final int VARHDRSZ =  4;	// length for int4
  
  // This is a default value for remarks
  private static final byte defaultRemarks[]="no remarks".getBytes();
  
  public DatabaseMetaData(Connection conn)
  {
    this.connection = conn;
  }
  
  /**
   * Can all the procedures returned by getProcedures be called
   * by the current user?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean allProceduresAreCallable() throws SQLException
  {
    return true;		// For now...
  }
  
  /**
   * Can all the tables returned by getTable be SELECTed by
   * the current user?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean allTablesAreSelectable() throws SQLException
  {
    return true;		// For now...
  }
  
  /**
   * What is the URL for this database?
   *
   * @return the url or null if it cannott be generated
   * @exception SQLException if a database access error occurs
   */
  public String getURL() throws SQLException
  {
    return connection.getURL();
  }
  
  /**
   * What is our user name as known to the database?
   *
   * @return our database user name
   * @exception SQLException if a database access error occurs
   */
  public String getUserName() throws SQLException
  {
    return connection.getUserName();
  }
  
  /**
   * Is the database in read-only mode?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isReadOnly() throws SQLException
  {
    return connection.isReadOnly();
  }
  
  /**
   * Are NULL values sorted high?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean nullsAreSortedHigh() throws SQLException
  {
    return false;
  }
  
  /**
   * Are NULL values sorted low?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean nullsAreSortedLow() throws SQLException
  {
    return false;
  }
  
  /**
   * Are NULL values sorted at the start regardless of sort order?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean nullsAreSortedAtStart() throws SQLException
  {
    return false;
  }
  
  /**
   * Are NULL values sorted at the end regardless of sort order?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean nullsAreSortedAtEnd() throws SQLException
  {
    return true;
  }
  
  /**
   * What is the name of this database product - we hope that it is
   * PostgreSQL, so we return that explicitly.
   *
   * @return the database product name
   * @exception SQLException if a database access error occurs
   */
  public String getDatabaseProductName() throws SQLException
  {
    return "PostgreSQL";
  }
  
  /**
   * What is the version of this database product.
   *
   * <p>Note that PostgreSQL 6.3 has a system catalog called pg_version - 
   * however, select * from pg_version on any database retrieves
   * no rows.
   *
   * <p>For now, we will return the version 6.3 (in the hope that we change
   * this driver as often as we change the database)
   *
   * @return the database version
   * @exception SQLException if a database access error occurs
   */
  public String getDatabaseProductVersion() throws SQLException
  {
    return ("7.0.2");
  }
  
  /**
   * What is the name of this JDBC driver?  If we don't know this
   * we are doing something wrong!
   *
   * @return the JDBC driver name
   * @exception SQLException why?
   */
  public String getDriverName() throws SQLException
  {
    return "PostgreSQL Native Driver";
  }
  
  /**
   * What is the version string of this JDBC driver?  Again, this is
   * static.
   *
   * @return the JDBC driver name.
   * @exception SQLException why?
   */
  public String getDriverVersion() throws SQLException
  {
    return Integer.toString(connection.this_driver.getMajorVersion())+"."+Integer.toString(connection.this_driver.getMinorVersion());
  }
  
  /**
   * What is this JDBC driver's major version number?
   *
   * @return the JDBC driver major version
   */
  public int getDriverMajorVersion()
  {
    return connection.this_driver.getMajorVersion();
  }
  
  /**
   * What is this JDBC driver's minor version number?
   *
   * @return the JDBC driver minor version
   */
  public int getDriverMinorVersion()
  {
    return connection.this_driver.getMinorVersion();
  }
  
  /**
   * Does the database store tables in a local file?  No - it
   * stores them in a file on the server.
   * 
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean usesLocalFiles() throws SQLException
  {
    return false;
  }
  
  /**
   * Does the database use a file for each table?  Well, not really,
   * since it doesnt use local files. 
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean usesLocalFilePerTable() throws SQLException
  {
    return false;
  }
  
  /**
   * Does the database treat mixed case unquoted SQL identifiers
   * as case sensitive and as a result store them in mixed case?
   * A JDBC-Compliant driver will always return false.
   *
   * <p>Predicament - what do they mean by "SQL identifiers" - if it
   * means the names of the tables and columns, then the answers
   * given below are correct - otherwise I don't know.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsMixedCaseIdentifiers() throws SQLException
  {
    return false;
  }
  
  /**
   * Does the database treat mixed case unquoted SQL identifiers as
   * case insensitive and store them in upper case?
   *
   * @return true if so
   */
  public boolean storesUpperCaseIdentifiers() throws SQLException
  {
    return false;
  }
  
  /**
   * Does the database treat mixed case unquoted SQL identifiers as
   * case insensitive and store them in lower case?
   *
   * @return true if so
   */
  public boolean storesLowerCaseIdentifiers() throws SQLException
  {
    return true;
  }
  
  /**
   * Does the database treat mixed case unquoted SQL identifiers as
   * case insensitive and store them in mixed case?
   *
   * @return true if so
   */
  public boolean storesMixedCaseIdentifiers() throws SQLException
  {
    return false;
  }
  
  /**
   * Does the database treat mixed case quoted SQL identifiers as
   * case sensitive and as a result store them in mixed case?  A
   * JDBC compliant driver will always return true. 
   *
   * <p>Predicament - what do they mean by "SQL identifiers" - if it
   * means the names of the tables and columns, then the answers
   * given below are correct - otherwise I don't know.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsMixedCaseQuotedIdentifiers() throws SQLException
  {
    return true;
  }
  
  /**
   * Does the database treat mixed case quoted SQL identifiers as
   * case insensitive and store them in upper case?
   *
   * @return true if so
   */
  public boolean storesUpperCaseQuotedIdentifiers() throws SQLException
  {
    return false;
  }
  
  /**
   * Does the database treat mixed case quoted SQL identifiers as case
   * insensitive and store them in lower case?
   *
   * @return true if so
   */
  public boolean storesLowerCaseQuotedIdentifiers() throws SQLException
  {
    return false;
  }
  
  /**
   * Does the database treat mixed case quoted SQL identifiers as case
   * insensitive and store them in mixed case?
   *
   * @return true if so
   */
  public boolean storesMixedCaseQuotedIdentifiers() throws SQLException
  {
    return false;
  }
  
  /**
   * What is the string used to quote SQL identifiers?  This returns
   * a space if identifier quoting isn't supported.  A JDBC Compliant
   * driver will always use a double quote character.
   *
   * <p>If an SQL identifier is a table name, column name, etc. then
   * we do not support it.
   *
   * @return the quoting string
   * @exception SQLException if a database access error occurs
   */
  public String getIdentifierQuoteString() throws SQLException
  {
    return "\"";
  }
  
  /**
   * Get a comma separated list of all a database's SQL keywords that
   * are NOT also SQL92 keywords.
   *
   * <p>Within PostgreSQL, the keywords are found in
   * 	src/backend/parser/keywords.c
   *
   * <p>For SQL Keywords, I took the list provided at
   * 	<a href="http://web.dementia.org/~shadow/sql/sql3bnf.sep93.txt">
   * http://web.dementia.org/~shadow/sql/sql3bnf.sep93.txt</a>
   * which is for SQL3, not SQL-92, but it is close enough for
   * this purpose.
   *
   * @return a comma separated list of keywords we use
   * @exception SQLException if a database access error occurs
   */
  public String getSQLKeywords() throws SQLException
  {
    return "abort,acl,add,aggregate,append,archive,arch_store,backward,binary,change,cluster,copy,database,delimiters,do,extend,explain,forward,heavy,index,inherits,isnull,light,listen,load,merge,nothing,notify,notnull,oids,purge,rename,replace,retrieve,returns,rule,recipe,setof,stdin,stdout,store,vacuum,verbose,version";
  }
  
  public String getNumericFunctions() throws SQLException
  {
    // XXX-Not Implemented
    return "";
  }
  
  public String getStringFunctions() throws SQLException
  {
    // XXX-Not Implemented
    return "";
  }
  
  public String getSystemFunctions() throws SQLException
  {
    // XXX-Not Implemented
    return "";
  }
  
  public String getTimeDateFunctions() throws SQLException
  {
    // XXX-Not Implemented
    return "";
  }
  
  /**
   * This is the string that can be used to escape '_' and '%' in
   * a search string pattern style catalog search parameters
   *
   * @return the string used to escape wildcard characters
   * @exception SQLException if a database access error occurs
   */
  public String getSearchStringEscape() throws SQLException
  {
    return "\\";
  }
  
  /**
   * Get all the "extra" characters that can bew used in unquoted
   * identifier names (those beyond a-zA-Z0-9 and _)
   *
   * <p>From the file src/backend/parser/scan.l, an identifier is
   * {letter}{letter_or_digit} which makes it just those listed
   * above.
   *
   * @return a string containing the extra characters
   * @exception SQLException if a database access error occurs
   */
  public String getExtraNameCharacters() throws SQLException
  {
    return "";
  }
  
  /**
   * Is "ALTER TABLE" with an add column supported?
   * Yes for PostgreSQL 6.1
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsAlterTableWithAddColumn() throws SQLException
  {
    return true;
  }
  
  /**
   * Is "ALTER TABLE" with a drop column supported?
   * Yes for PostgreSQL 6.1
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsAlterTableWithDropColumn() throws SQLException
  {
    return true;
  }
  
  /**
   * Is column aliasing supported?
   *
   * <p>If so, the SQL AS clause can be used to provide names for
   * computed columns or to provide alias names for columns as
   * required.  A JDBC Compliant driver always returns true.
   *
   * <p>e.g.
   *
   * <br><pre>
   * select count(C) as C_COUNT from T group by C;
   *
   * </pre><br>
   * should return a column named as C_COUNT instead of count(C)
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsColumnAliasing() throws SQLException
  {
    return true;
  }
  
  /**
   * Are concatenations between NULL and non-NULL values NULL?  A
   * JDBC Compliant driver always returns true
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean nullPlusNonNullIsNull() throws SQLException
  {
    return true;
  }
  
  public boolean supportsConvert() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsConvert(int fromType, int toType) throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsTableCorrelationNames() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsDifferentTableCorrelationNames() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  /**
   * Are expressions in "ORCER BY" lists supported?
   * 
   * <br>e.g. select * from t order by a + b;
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsExpressionsInOrderBy() throws SQLException
  {
    return true;
  }
  
  /**
   * Can an "ORDER BY" clause use columns not in the SELECT?
   * I checked it, and you can't.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsOrderByUnrelated() throws SQLException
  {
    return false;
  }
  
  /**
   * Is some form of "GROUP BY" clause supported?
   * I checked it, and yes it is.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsGroupBy() throws SQLException
  {
    return true;
  }
  
  /**
   * Can a "GROUP BY" clause use columns not in the SELECT?
   * I checked it - it seems to allow it
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsGroupByUnrelated() throws SQLException
  {
    return true;
  }
  
  /**
   * Can a "GROUP BY" clause add columns not in the SELECT provided
   * it specifies all the columns in the SELECT?  Does anyone actually
   * understand what they mean here?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsGroupByBeyondSelect() throws SQLException
  {
    return true;		// For now...
  }
  
  /**
   * Is the escape character in "LIKE" clauses supported?  A
   * JDBC compliant driver always returns true.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsLikeEscapeClause() throws SQLException
  {
    return true;
  }
  
  /**
   * Are multiple ResultSets from a single execute supported?
   * Well, I implemented it, but I dont think this is possible from
   * the back ends point of view.
   * 
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsMultipleResultSets() throws SQLException
  {
    return false;
  }
  
  /**
   * Can we have multiple transactions open at once (on different
   * connections?)
   * I guess we can have, since Im relying on it.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsMultipleTransactions() throws SQLException
  {
    return true;
  }
  
  /**
   * Can columns be defined as non-nullable.  A JDBC Compliant driver
   * always returns true.
   *
   * <p>This changed from false to true in v6.2 of the driver, as this
   * support was added to the backend.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsNonNullableColumns() throws SQLException
  {
    return true;
  }
  
  /**
   * Does this driver support the minimum ODBC SQL grammar.  This
   * grammar is defined at:
   *
   * <p><a href="http://www.microsoft.com/msdn/sdk/platforms/doc/odbc/src/intropr.htm">http://www.microsoft.com/msdn/sdk/platforms/doc/odbc/src/intropr.htm</a>
   *
   * <p>In Appendix C.  From this description, we seem to support the
   * ODBC minimal (Level 0) grammar.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsMinimumSQLGrammar() throws SQLException
  {
    return true;
  }
  
  /**
   * Does this driver support the Core ODBC SQL grammar.  We need
   * SQL-92 conformance for this.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsCoreSQLGrammar() throws SQLException
  {
    return false;
  }
  
  /**
   * Does this driver support the Extended (Level 2) ODBC SQL
   * grammar.  We don't conform to the Core (Level 1), so we can't
   * conform to the Extended SQL Grammar.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsExtendedSQLGrammar() throws SQLException
  {
    return false;
  }
  
  /**
   * Does this driver support the ANSI-92 entry level SQL grammar?
   * All JDBC Compliant drivers must return true.  I think we have
   * to support outer joins for this to be true.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsANSI92EntryLevelSQL() throws SQLException
  {
    return false;
  }
  
  /**
   * Does this driver support the ANSI-92 intermediate level SQL
   * grammar?  Anyone who does not support Entry level cannot support
   * Intermediate level.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsANSI92IntermediateSQL() throws SQLException
  {
    return false;
  }
  
  /**
   * Does this driver support the ANSI-92 full SQL grammar?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsANSI92FullSQL() throws SQLException
  {
    return false;
  }
  
  /**
   * Is the SQL Integrity Enhancement Facility supported?
   * I haven't seen this mentioned anywhere, so I guess not
   * 
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsIntegrityEnhancementFacility() throws SQLException
  {
    return false;
  }
  
  /**
   * Is some form of outer join supported?  From my knowledge, nope.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsOuterJoins() throws SQLException
  {
    return false;
  }
  
  /**
   * Are full nexted outer joins supported?  Well, we dont support any
   * form of outer join, so this is no as well
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsFullOuterJoins() throws SQLException
  {
    return false;
  }
  
  /**
   * Is there limited support for outer joins?  (This will be true if
   * supportFullOuterJoins is true)
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsLimitedOuterJoins() throws SQLException
  {
    return false;
  }
  
  /**
   * What is the database vendor's preferred term for "schema" - well,
   * we do not provide support for schemas, so lets just use that
   * term.
   *
   * @return the vendor term
   * @exception SQLException if a database access error occurs
   */
  public String getSchemaTerm() throws SQLException
  {
    return "Schema";
  }
  
  /**
   * What is the database vendor's preferred term for "procedure" - 
   * I kind of like "Procedure" myself.
   *
   * @return the vendor term
   * @exception SQLException if a database access error occurs
   */
  public String getProcedureTerm() throws SQLException
  {
    return "Procedure";
  }
  
  /**
   * What is the database vendor's preferred term for "catalog"? -
   * we dont have a preferred term, so just use Catalog
   *
   * @return the vendor term
   * @exception SQLException if a database access error occurs
   */
  public String getCatalogTerm() throws SQLException
  {
    return "Catalog";
  }
  
  /**
   * Does a catalog appear at the start of a qualified table name?
   * (Otherwise it appears at the end).
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean isCatalogAtStart() throws SQLException
  {
    return false;
  }
  
  /**
   * What is the Catalog separator.  Hmmm....well, I kind of like
   * a period (so we get catalog.table definitions). - I don't think
   * PostgreSQL supports catalogs anyhow, so it makes no difference.
   *
   * @return the catalog separator string
   * @exception SQLException if a database access error occurs
   */
  public String getCatalogSeparator() throws SQLException
  {
    // PM Sep 29 97 - changed from "." as we don't support catalogs.
    return "";
  }
  
  /**
   * Can a schema name be used in a data manipulation statement?  Nope.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsSchemasInDataManipulation() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a schema name be used in a procedure call statement?  Nope.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsSchemasInProcedureCalls() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a schema be used in a table definition statement?  Nope.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsSchemasInTableDefinitions() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a schema name be used in an index definition statement?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsSchemasInIndexDefinitions() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a schema name be used in a privilege definition statement?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsSchemasInPrivilegeDefinitions() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a catalog name be used in a data manipulation statement?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsCatalogsInDataManipulation() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a catalog name be used in a procedure call statement?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsCatalogsInProcedureCalls() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a catalog name be used in a table definition statement?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsCatalogsInTableDefinitions() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a catalog name be used in an index definition?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsCatalogsInIndexDefinitions() throws SQLException
  {
    return false;
  }
  
  /**
   * Can a catalog name be used in a privilege definition statement?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsCatalogsInPrivilegeDefinitions() throws SQLException
  {
    return false;
  }
  
  /**
   * We support cursors for gets only it seems.  I dont see a method
   * to get a positioned delete.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsPositionedDelete() throws SQLException
  {
    return false;			// For now...
  }
  
  /**
   * Is positioned UPDATE supported?
   * 
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsPositionedUpdate() throws SQLException
  {
    return false;			// For now...
  }
  
  public boolean supportsSelectForUpdate() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsStoredProcedures() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsSubqueriesInComparisons() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsSubqueriesInExists() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsSubqueriesInIns() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsSubqueriesInQuantifieds() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  public boolean supportsCorrelatedSubqueries() throws SQLException
  {
    // XXX-Not Implemented
    return false;
  }
  
  /**
   * Is SQL UNION supported?  Nope.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsUnion() throws SQLException
  {
    return false;
  }
  
  /**
   * Is SQL UNION ALL supported?  Nope.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsUnionAll() throws SQLException
  {
    return false;
  }
  
  /**
   * In PostgreSQL, Cursors are only open within transactions.
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsOpenCursorsAcrossCommit() throws SQLException
  {
    return false;
  }
  
  /**
   * Do we support open cursors across multiple transactions?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsOpenCursorsAcrossRollback() throws SQLException
  {
    return false;
  }
  
  /**
   * Can statements remain open across commits?  They may, but
   * this driver cannot guarentee that.  In further reflection.
   * we are talking a Statement object jere, so the answer is
   * yes, since the Statement is only a vehicle to ExecSQL()
   *
   * @return true if they always remain open; false otherwise
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsOpenStatementsAcrossCommit() throws SQLException
  {
    return true;
  }
  
  /**
   * Can statements remain open across rollbacks?  They may, but
   * this driver cannot guarentee that.  In further contemplation,
   * we are talking a Statement object here, so the answer is yes,
   * since the Statement is only a vehicle to ExecSQL() in Connection
   *
   * @return true if they always remain open; false otherwise
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsOpenStatementsAcrossRollback() throws SQLException
  {
    return true;
  }
  
  /**
   * How many hex characters can you have in an inline binary literal
   *
   * @return the max literal length
   * @exception SQLException if a database access error occurs
   */
  public int getMaxBinaryLiteralLength() throws SQLException
  {
    return 0;				// For now...
  }
  
  /**
   * What is the maximum length for a character literal
   * I suppose it is 8190 (8192 - 2 for the quotes)
   *
   * @return the max literal length
   * @exception SQLException if a database access error occurs
   */
  public int getMaxCharLiteralLength() throws SQLException
  {
    return 8190;
  }
  
  /**
   * Whats the limit on column name length.  The description of
   * pg_class would say '32' (length of pg_class.relname) - we
   * should probably do a query for this....but....
   *
   * @return the maximum column name length
   * @exception SQLException if a database access error occurs
   */
  public int getMaxColumnNameLength() throws SQLException
  {
    return 32;
  }
  
  /**
   * What is the maximum number of columns in a "GROUP BY" clause?
   *
   * @return the max number of columns
   * @exception SQLException if a database access error occurs	
   */
  public int getMaxColumnsInGroupBy() throws SQLException
  {
    return getMaxColumnsInTable();
  }
  
  /**
   * What's the maximum number of columns allowed in an index?
   * 6.0 only allowed one column, but 6.1 introduced multi-column
   * indices, so, theoretically, its all of them.
   *
   * @return max number of columns
   * @exception SQLException if a database access error occurs
   */
  public int getMaxColumnsInIndex() throws SQLException
  {
    return getMaxColumnsInTable();
  }
  
  /**
   * What's the maximum number of columns in an "ORDER BY clause?
   * Theoretically, all of them!
   *
   * @return the max columns
   * @exception SQLException if a database access error occurs
   */
  public int getMaxColumnsInOrderBy() throws SQLException
  {
    return getMaxColumnsInTable();
  }
  
  /**
   * What is the maximum number of columns in a "SELECT" list?
   * Theoretically, all of them!
   *
   * @return the max columns
   * @exception SQLException if a database access error occurs
   */
  public int getMaxColumnsInSelect() throws SQLException
  {
    return getMaxColumnsInTable();
  }
  
  /**
   * What is the maximum number of columns in a table? From the
   * create_table(l) manual page...
   *
   * <p>"The new class is created as a heap with no initial data.  A
   * class can have no more than 1600 attributes (realistically,
   * this is limited by the fact that tuple sizes must be less than
   * 8192 bytes)..."
   *
   * @return the max columns
   * @exception SQLException if a database access error occurs
   */
  public int getMaxColumnsInTable() throws SQLException
  {
    return 1600;
  }
  
  /**
   * How many active connection can we have at a time to this
   * database?  Well, since it depends on postmaster, which just
   * does a listen() followed by an accept() and fork(), its
   * basically very high.  Unless the system runs out of processes,
   * it can be 65535 (the number of aux. ports on a TCP/IP system).
   * I will return 8192 since that is what even the largest system
   * can realistically handle,
   *
   * @return the maximum number of connections
   * @exception SQLException if a database access error occurs
   */
  public int getMaxConnections() throws SQLException
  {
    return 8192;
  }
  
  /**
   * What is the maximum cursor name length (the same as all
   * the other F***** identifiers!)
   *
   * @return max cursor name length in bytes
   * @exception SQLException if a database access error occurs
   */
  public int getMaxCursorNameLength() throws SQLException
  {
    return 32;
  }
  
  /**
   * What is the maximum length of an index (in bytes)?  Now, does
   * the spec. mean name of an index (in which case its 32, the 
   * same as a table) or does it mean length of an index element
   * (in which case its 8192, the size of a row) or does it mean
   * the number of rows it can access (in which case it 2^32 - 
   * a 4 byte OID number)?  I think its the length of an index
   * element, personally, so Im setting it to 8192.
   *
   * @return max index length in bytes
   * @exception SQLException if a database access error occurs
   */
  public int getMaxIndexLength() throws SQLException
  {
    return 8192;
  }
  
  public int getMaxSchemaNameLength() throws SQLException
  {
    // XXX-Not Implemented
    return 0;
  }
  
  /**
   * What is the maximum length of a procedure name?
   * (length of pg_proc.proname used) - again, I really
   * should do a query here to get it.
   *
   * @return the max name length in bytes
   * @exception SQLException if a database access error occurs
   */
  public int getMaxProcedureNameLength() throws SQLException
  {
    return 32;
  }
  
  public int getMaxCatalogNameLength() throws SQLException
  {
    // XXX-Not Implemented
    return 0;
  }
  
  /**
   * What is the maximum length of a single row?  (not including
   * blobs).  8192 is defined in PostgreSQL.
   *
   * @return max row size in bytes
   * @exception SQLException if a database access error occurs
   */
  public int getMaxRowSize() throws SQLException
  {
    return 8192;
  }
  
  /**
   * Did getMaxRowSize() include LONGVARCHAR and LONGVARBINARY
   * blobs?  We don't handle blobs yet
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean doesMaxRowSizeIncludeBlobs() throws SQLException
  {
    return false;
  }
  
  /**
   * What is the maximum length of a SQL statement?
   *
   * @return max length in bytes
   * @exception SQLException if a database access error occurs
   */
  public int getMaxStatementLength() throws SQLException
  {
    return 8192;
  }
  
  /**
   * How many active statements can we have open at one time to
   * this database?  Basically, since each Statement downloads
   * the results as the query is executed, we can have many.  However,
   * we can only really have one statement per connection going
   * at once (since they are executed serially) - so we return
   * one.
   *
   * @return the maximum
   * @exception SQLException if a database access error occurs
   */
  public int getMaxStatements() throws SQLException
  {
    return 1;
  }
  
  /**
   * What is the maximum length of a table name?  This was found
   * from pg_class.relname length
   *
   * @return max name length in bytes
   * @exception SQLException if a database access error occurs
   */
  public int getMaxTableNameLength() throws SQLException
  {
    return 32;
  }
  
  /**
   * What is the maximum number of tables that can be specified
   * in a SELECT?  Theoretically, this is the same number as the
   * number of tables allowable.  In practice tho, it is much smaller
   * since the number of tables is limited by the statement, we
   * return 1024 here - this is just a number I came up with (being
   * the number of tables roughly of three characters each that you
   * can fit inside a 8192 character buffer with comma separators).
   *
   * @return the maximum
   * @exception SQLException if a database access error occurs
   */
  public int getMaxTablesInSelect() throws SQLException
  {
    return 1024;
  }
  
  /**
   * What is the maximum length of a user name?  Well, we generally
   * use UNIX like user names in PostgreSQL, so I think this would
   * be 8.  However, showing the schema for pg_user shows a length
   * for username of 32.
   *
   * @return the max name length in bytes
   * @exception SQLException if a database access error occurs
   */
  public int getMaxUserNameLength() throws SQLException
  {
    return 32;
  }
  
  
  /**
   * What is the database's default transaction isolation level?  We
   * do not support this, so all transactions are SERIALIZABLE.
   *
   * @return the default isolation level
   * @exception SQLException if a database access error occurs
   * @see Connection
   */
  public int getDefaultTransactionIsolation() throws SQLException
  {
    return Connection.TRANSACTION_READ_COMMITTED;
  }
  
  /**
   * Are transactions supported?  If not, commit and rollback are noops
   * and the isolation level is TRANSACTION_NONE.  We do support
   * transactions.	
   *
   * @return true if transactions are supported
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsTransactions() throws SQLException
  {
    return true;
  }
  
  /**
   * Does the database support the given transaction isolation level?
   * We only support TRANSACTION_SERIALIZABLE and TRANSACTION_READ_COMMITTED
   * 
   * @param level the values are defined in java.sql.Connection
   * @return true if so
   * @exception SQLException if a database access error occurs
   * @see Connection
   */
  public boolean supportsTransactionIsolationLevel(int level) throws SQLException
  {
      if (level == Connection.TRANSACTION_SERIALIZABLE ||
	  level == Connection.TRANSACTION_READ_COMMITTED)
	  return true;
      else
	  return false;
  }
  
  /**
   * Are both data definition and data manipulation transactions 
   * supported?  I checked it, and could not do a CREATE TABLE
   * within a transaction, so I am assuming that we don't
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsDataDefinitionAndDataManipulationTransactions() throws SQLException
  {
    return false;
  }
  
  /**
   * Are only data manipulation statements withing a transaction
   * supported?
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean supportsDataManipulationTransactionsOnly() throws SQLException
  {
    return true;
  }
  
  /**
   * Does a data definition statement within a transaction force
   * the transaction to commit?  I think this means something like:
   *
   * <p><pre>
   * CREATE TABLE T (A INT);
   * INSERT INTO T (A) VALUES (2);
   * BEGIN;
   * UPDATE T SET A = A + 1;
   * CREATE TABLE X (A INT);
   * SELECT A FROM T INTO X;
   * COMMIT;
   * </pre><p>
   *
   * does the CREATE TABLE call cause a commit?  The answer is no.  
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean dataDefinitionCausesTransactionCommit() throws SQLException
  {
    return false;
  }
  
  /**
   * Is a data definition statement within a transaction ignored?
   * It seems to be (from experiment in previous method)
   *
   * @return true if so
   * @exception SQLException if a database access error occurs
   */
  public boolean dataDefinitionIgnoredInTransactions() throws SQLException
  {
    return true;
  }
  
  /**
   * Get a description of stored procedures available in a catalog
   * 
   * <p>Only procedure descriptions matching the schema and procedure
   * name criteria are returned.  They are ordered by PROCEDURE_SCHEM
   * and PROCEDURE_NAME
   *
   * <p>Each procedure description has the following columns:
   * <ol>
   * <li><b>PROCEDURE_CAT</b> String => procedure catalog (may be null)
   * <li><b>PROCEDURE_SCHEM</b> String => procedure schema (may be null)
   * <li><b>PROCEDURE_NAME</b> String => procedure name
   * <li><b>Field 4</b> reserved (make it null)
   * <li><b>Field 5</b> reserved (make it null)
   * <li><b>Field 6</b> reserved (make it null)
   * <li><b>REMARKS</b> String => explanatory comment on the procedure
   * <li><b>PROCEDURE_TYPE</b> short => kind of procedure
   *	<ul>
   *    <li> procedureResultUnknown - May return a result
   * 	<li> procedureNoResult - Does not return a result
   *	<li> procedureReturnsResult - Returns a result
   *    </ul>
   * </ol>
   *
   * @param catalog - a catalog name; "" retrieves those without a
   *	catalog; null means drop catalog name from criteria
   * @param schemaParrern - a schema name pattern; "" retrieves those
   *	without a schema - we ignore this parameter
   * @param procedureNamePattern - a procedure name pattern
   * @return ResultSet - each row is a procedure description
   * @exception SQLException if a database access error occurs
   */
  public java.sql.ResultSet getProcedures(String catalog, String schemaPattern, String procedureNamePattern) throws SQLException
  {
    // the field descriptors for the new ResultSet
    Field f[] = new Field[8];
    java.sql.ResultSet r;	// ResultSet for the SQL query that we need to do
    Vector v = new Vector();		// The new ResultSet tuple stuff
    
    byte remarks[] = defaultRemarks;
    
    f[0] = new Field(connection, "PROCEDURE_CAT",   iVarcharOid, 32);
    f[1] = new Field(connection, "PROCEDURE_SCHEM", iVarcharOid, 32);
    f[2] = new Field(connection, "PROCEDURE_NAME",  iVarcharOid, 32);
    f[3] = f[4] = f[5] = null;	// reserved, must be null for now
    f[6] = new Field(connection, "REMARKS",	   iVarcharOid, 8192);
    f[7] = new Field(connection, "PROCEDURE_TYPE", iInt2Oid,	2);
    
    // If the pattern is null, then set it to the default
    if(procedureNamePattern==null)
      procedureNamePattern="%";
    
    r = connection.ExecSQL("select proname, proretset from pg_proc where proname like '"+procedureNamePattern.toLowerCase()+"' order by proname");
    
    while (r.next())
      {
	byte[][] tuple = new byte[8][0];
	
	tuple[0] = null;			// Catalog name
	tuple[1] = null;			// Schema name
	tuple[2] = r.getBytes(1);		// Procedure name
	tuple[3] = tuple[4] = tuple[5] = null;	// Reserved
	tuple[6] = remarks;			// Remarks
	
	if (r.getBoolean(2))
	  tuple[7] = Integer.toString(java.sql.DatabaseMetaData.procedureReturnsResult).getBytes();
	else
	  tuple[7] = Integer.toString(java.sql.DatabaseMetaData.procedureNoResult).getBytes();
	
	v.addElement(tuple);
      }
    return new ResultSet(connection, f, v, "OK", 1);
  }
  
  /**
   * Get a description of a catalog's stored procedure parameters
   * and result columns.
   *
   * <p>Only descriptions matching the schema, procedure and parameter
   * name criteria are returned. They are ordered by PROCEDURE_SCHEM
   * and PROCEDURE_NAME. Within this, the return value, if any, is
   * first. Next are the parameter descriptions in call order. The
   * column descriptions follow in column number order.
   *
   * <p>Each row in the ResultSet is a parameter description or column 
   * description with the following fields:
   * <ol>
   * <li><b>PROCEDURE_CAT</b> String => procedure catalog (may be null)
   * <li><b>PROCEDURE_SCHE</b>M String => procedure schema (may be null)
   * <li><b>PROCEDURE_NAME</b> String => procedure name
   * <li><b>COLUMN_NAME</b> String => column/parameter name
   * <li><b>COLUMN_TYPE</b> Short => kind of column/parameter:
   * <ul><li>procedureColumnUnknown - nobody knows
   * <li>procedureColumnIn - IN parameter
   * <li>procedureColumnInOut - INOUT parameter
   * <li>procedureColumnOut - OUT parameter
   * <li>procedureColumnReturn - procedure return value
   * <li>procedureColumnResult - result column in ResultSet
   * </ul>
   * <li><b>DATA_TYPE</b> short => SQL type from java.sql.Types
   * <li><b>TYPE_NAME</b> String => SQL type name
   * <li><b>PRECISION</b> int => precision
   * <li><b>LENGTH</b> int => length in bytes of data
   * <li><b>SCALE</b> short => scale
   * <li><b>RADIX</b> short => radix
   * <li><b>NULLABLE</b> short => can it contain NULL?
   * <ul><li>procedureNoNulls - does not allow NULL values
   * <li>procedureNullable - allows NULL values
   * <li>procedureNullableUnknown - nullability unknown
   * <li><b>REMARKS</b> String => comment describing parameter/column
   * </ol>
   * @param catalog This is ignored in org.postgresql, advise this is set to null
   * @param schemaPattern This is ignored in org.postgresql, advise this is set to null
   * @param procedureNamePattern a procedure name pattern
   * @param columnNamePattern a column name pattern
   * @return each row is a stored procedure parameter or column description
   * @exception SQLException if a database-access error occurs
   * @see #getSearchStringEscape
   */
  // Implementation note: This is required for Borland's JBuilder to work
  public java.sql.ResultSet getProcedureColumns(String catalog, String schemaPattern, String procedureNamePattern, String columnNamePattern) throws SQLException
  {
    if(procedureNamePattern==null)
      procedureNamePattern="%";
    
    if(columnNamePattern==null)
      columnNamePattern="%";
    
    // for now, this returns an empty result set.
    Field f[] = new Field[13];
    ResultSet r;	// ResultSet for the SQL query that we need to do
    Vector v = new Vector();		// The new ResultSet tuple stuff
    
    f[0] = new Field(connection, "PROCEDURE_CAT", iVarcharOid, 32);
    f[1] = new Field(connection, "PROCEDURE_SCHEM", iVarcharOid, 32);
    f[2] = new Field(connection, "PROCEDURE_NAME", iVarcharOid, 32);
    f[3] = new Field(connection, "COLUMN_NAME", iVarcharOid, 32);
    f[4] = new Field(connection, "COLUMN_TYPE", iInt2Oid, 2);
    f[5] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
    f[6] = new Field(connection, "TYPE_NAME", iVarcharOid, 32);
    f[7] = new Field(connection, "PRECISION", iInt4Oid, 4);
    f[8] = new Field(connection, "LENGTH", iInt4Oid, 4);
    f[9] = new Field(connection, "SCALE", iInt2Oid, 2);
    f[10] = new Field(connection, "RADIX", iInt2Oid, 2);
    f[11] = new Field(connection, "NULLABLE", iInt2Oid, 2);
    f[12] = new Field(connection, "REMARKS", iVarcharOid, 32);
    
    // add query loop here
    
    return new ResultSet(connection, f, v, "OK", 1);
  }
  
  /**
   * Get a description of tables available in a catalog.              
   *
   * <p>Only table descriptions matching the catalog, schema, table
   * name and type criteria are returned. They are ordered by
   * TABLE_TYPE, TABLE_SCHEM and TABLE_NAME.                      
   * 
   * <p>Each table description has the following columns:     
   *
   * <ol>
   * <li><b>TABLE_CAT</b> String => table catalog (may be null)      
   * <li><b>TABLE_SCHEM</b> String => table schema (may be null)         
   * <li><b>TABLE_NAME</b> String => table name
   * <li><b>TABLE_TYPE</b> String => table type. Typical types are "TABLE",
   * "VIEW", "SYSTEM TABLE", "GLOBAL TEMPORARY", "LOCAL
   * TEMPORARY", "ALIAS", "SYNONYM".                             
   * <li><b>REMARKS</b> String => explanatory comment on the table
   * </ol>
   *
   * <p>The valid values for the types parameter are:
   * "TABLE", "INDEX", "LARGE OBJECT", "SEQUENCE", "SYSTEM TABLE" and
   * "SYSTEM INDEX"
   *
   * @param catalog a catalog name; For org.postgresql, this is ignored, and
   * should be set to null
   * @param schemaPattern a schema name pattern; For org.postgresql, this is ignored, and
   * should be set to null
   * @param tableNamePattern a table name pattern. For all tables this should be "%"
   * @param types a list of table types to include; null returns
   * all types
   * @return each row is a table description      
   * @exception SQLException if a database-access error occurs.
   */
  public java.sql.ResultSet getTables(String catalog, String schemaPattern, String tableNamePattern, String types[]) throws SQLException
  {
    // Handle default value for types
    if(types==null)
      types = defaultTableTypes;
    
    if(tableNamePattern==null)
      tableNamePattern="%";
    
    // the field descriptors for the new ResultSet
    Field f[] = new Field[5];
    java.sql.ResultSet r;	// ResultSet for the SQL query that we need to do
    Vector v = new Vector();		// The new ResultSet tuple stuff
    
    f[0] = new Field(connection, "TABLE_CAT", iVarcharOid, 32);
    f[1] = new Field(connection, "TABLE_SCHEM", iVarcharOid, 32);
    f[2] = new Field(connection, "TABLE_NAME", iVarcharOid, 32);
    f[3] = new Field(connection, "TABLE_TYPE", iVarcharOid, 32);
    f[4] = new Field(connection, "REMARKS", iVarcharOid, 32);
    
    // Now form the query
    StringBuffer sql = new StringBuffer("select relname,oid,relkind from pg_class where (");
    boolean notFirst=false;
    for(int i=0;i<types.length;i++) {
      for(int j=0;j<getTableTypes.length;j++)
	if(getTableTypes[j][0].equals(types[i])) {
          if(notFirst)
            sql.append(" or ");
	  sql.append(getTableTypes[j][1]);
	  notFirst=true;
	}
    }
    
    // Added by Stefan Andreasen <stefan@linux.kapow.dk>
    // Now take the pattern into account
    sql.append(") and relname like '");
    sql.append(tableNamePattern.toLowerCase());
    sql.append("'");
    
    // Now run the query
    r = connection.ExecSQL(sql.toString());
    
    byte remarks[];
    
    while (r.next())
      {
	byte[][] tuple = new byte[5][0];
	
	// Fetch the description for the table (if any)
	java.sql.ResultSet dr = connection.ExecSQL("select description from pg_description where objoid="+r.getInt(2));
	if(((org.postgresql.ResultSet)dr).getTupleCount()==1) {
	  dr.next();
	  remarks = dr.getBytes(1);
	} else
	  remarks = defaultRemarks;
	dr.close();
	
	String relKind;
	switch (r.getBytes(3)[0]) {
	case 'r':
	    relKind = "TABLE";
	    break;
	case 'i':
	    relKind = "INDEX";
	    break;
	case 'S':
	    relKind = "SEQUENCE";
	    break;
	default:
	    relKind = null;
	}

	tuple[0] = null;		// Catalog name
	tuple[1] = null;		// Schema name
	tuple[2] = r.getBytes(1);	// Table name	
	tuple[3] = relKind.getBytes();	// Table type
	tuple[4] = remarks;		// Remarks
	v.addElement(tuple);
      }
    r.close();
    return new ResultSet(connection, f, v, "OK", 1);
  }
  
  // This array contains the valid values for the types argument
  // in getTables().
  //
  // Each supported type consists of it's name, and the sql where
  // clause to retrieve that value.
  //
  // IMPORTANT: the query must be enclosed in ( )
  private static final String getTableTypes[][] = {
    {"TABLE",		"(relkind='r' and relhasrules='f' and relname !~ '^pg_' and relname !~ '^xinv')"},
    {"VIEW",		"(relkind='r' and relhasrules='t' and relname !~ '^pg_' and relname !~ '^xinv')"},
    {"INDEX",		"(relkind='i' and relname !~ '^pg_' and relname !~ '^xinx')"},
    {"LARGE OBJECT",	"(relkind='r' and relname ~ '^xinv')"},
    {"SEQUENCE",	"(relkind='S' and relname !~ '^pg_')"},
    {"SYSTEM TABLE",	"(relkind='r' and relname ~ '^pg_')"},
    {"SYSTEM INDEX",	"(relkind='i' and relname ~ '^pg_')"}
  };
  
  // These are the default tables, used when NULL is passed to getTables
  // The choice of these provide the same behaviour as psql's \d
  private static final String defaultTableTypes[] = {
    "TABLE","VIEW","INDEX","SEQUENCE"
  };
  
  /**
   * Get the schema names available in this database.  The results
   * are ordered by schema name.
   *
   * <P>The schema column is:
   *  <OL>
   *	<LI><B>TABLE_SCHEM</B> String => schema name
   *  </OL>
   *
   * @return ResultSet each row has a single String column that is a
   * schema name
   */
  public java.sql.ResultSet getSchemas() throws SQLException
  {
    // We don't use schemas, so we simply return a single schema name "".
    //
    Field f[] = new Field[1];
    Vector v = new Vector();
    byte[][] tuple = new byte[1][0];
    f[0] = new Field(connection,"TABLE_SCHEM",iVarcharOid,32);
    tuple[0] = "".getBytes();
    v.addElement(tuple);
    return new ResultSet(connection,f,v,"OK",1);
  }
  
  /**
   * Get the catalog names available in this database.  The results
   * are ordered by catalog name.
   *
   * <P>The catalog column is:
   *  <OL>
   *	<LI><B>TABLE_CAT</B> String => catalog name
   *  </OL>
   *
   * @return ResultSet each row has a single String column that is a
   * catalog name
   */
  public java.sql.ResultSet getCatalogs() throws SQLException
  {
    // We don't use catalogs, so we simply return a single catalog name "".
    Field f[] = new Field[1];
    Vector v = new Vector();
    byte[][] tuple = new byte[1][0];
    f[0] = new Field(connection,"TABLE_CAT",iVarcharOid,32);
    tuple[0] = "".getBytes();
    v.addElement(tuple);
    return new ResultSet(connection,f,v,"OK",1);
  }
  
  /**
   * Get the table types available in this database.  The results
   * are ordered by table type.
   *
   * <P>The table type is:
   *  <OL>
   *	<LI><B>TABLE_TYPE</B> String => table type.  Typical types are "TABLE",
   *			"VIEW",	"SYSTEM TABLE", "GLOBAL TEMPORARY",
   *			"LOCAL TEMPORARY", "ALIAS", "SYNONYM".
   *  </OL>
   *
   * @return ResultSet each row has a single String column that is a
   * table type
   */
  public java.sql.ResultSet getTableTypes() throws SQLException
  {
    Field f[] = new Field[1];
    Vector v = new Vector();
    f[0] = new Field(connection,new String("TABLE_TYPE"),iVarcharOid,32);
    for(int i=0;i<getTableTypes.length;i++) {
      byte[][] tuple = new byte[1][0];
      tuple[0] = getTableTypes[i][0].getBytes();
      v.addElement(tuple);
    }
    return new ResultSet(connection,f,v,"OK",1);
  }
  
  /**
   * Get a description of table columns available in a catalog.
   *
   * <P>Only column descriptions matching the catalog, schema, table
   * and column name criteria are returned.  They are ordered by
   * TABLE_SCHEM, TABLE_NAME and ORDINAL_POSITION.
   *
   * <P>Each column description has the following columns:
   *  <OL>
   *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
   *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
   *	<LI><B>TABLE_NAME</B> String => table name
   *	<LI><B>COLUMN_NAME</B> String => column name
   *	<LI><B>DATA_TYPE</B> short => SQL type from java.sql.Types
   *	<LI><B>TYPE_NAME</B> String => Data source dependent type name
   *	<LI><B>COLUMN_SIZE</B> int => column size.  For char or date
   *	    types this is the maximum number of characters, for numeric or
   *	    decimal types this is precision.
   *	<LI><B>BUFFER_LENGTH</B> is not used.
   *	<LI><B>DECIMAL_DIGITS</B> int => the number of fractional digits
   *	<LI><B>NUM_PREC_RADIX</B> int => Radix (typically either 10 or 2)
   *	<LI><B>NULLABLE</B> int => is NULL allowed?
   *      <UL>
   *      <LI> columnNoNulls - might not allow NULL values
   *      <LI> columnNullable - definitely allows NULL values
   *      <LI> columnNullableUnknown - nullability unknown
   *      </UL>
   *	<LI><B>REMARKS</B> String => comment describing column (may be null)
   * 	<LI><B>COLUMN_DEF</B> String => default value (may be null)
   *	<LI><B>SQL_DATA_TYPE</B> int => unused
   *	<LI><B>SQL_DATETIME_SUB</B> int => unused
   *	<LI><B>CHAR_OCTET_LENGTH</B> int => for char types the
   *       maximum number of bytes in the column
   *	<LI><B>ORDINAL_POSITION</B> int	=> index of column in table
   *      (starting at 1)
   *	<LI><B>IS_NULLABLE</B> String => "NO" means column definitely
   *      does not allow NULL values; "YES" means the column might
   *      allow NULL values.  An empty string means nobody knows.
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schemaPattern a schema name pattern; "" retrieves those
   * without a schema
   * @param tableNamePattern a table name pattern
   * @param columnNamePattern a column name pattern
   * @return ResultSet each row is a column description
   * @see #getSearchStringEscape
   */
  public java.sql.ResultSet getColumns(String catalog, String schemaPattern, String tableNamePattern, String columnNamePattern) throws SQLException
  {
    // the field descriptors for the new ResultSet
    Field f[] = new Field[18];
    java.sql.ResultSet r;	// ResultSet for the SQL query that we need to do
    Vector v = new Vector();		// The new ResultSet tuple stuff
    
    f[0] = new Field(connection, "TABLE_CAT", iVarcharOid, 32);
    f[1] = new Field(connection, "TABLE_SCHEM", iVarcharOid, 32);
    f[2] = new Field(connection, "TABLE_NAME", iVarcharOid, 32);
    f[3] = new Field(connection, "COLUMN_NAME", iVarcharOid, 32);
    f[4] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
    f[5] = new Field(connection, "TYPE_NAME", iVarcharOid, 32);
    f[6] = new Field(connection, "COLUMN_SIZE", iInt4Oid, 4);
    f[7] = new Field(connection, "BUFFER_LENGTH", iVarcharOid, 32);
    f[8] = new Field(connection, "DECIMAL_DIGITS", iInt4Oid, 4);
    f[9] = new Field(connection, "NUM_PREC_RADIX", iInt4Oid, 4);
    f[10] = new Field(connection, "NULLABLE", iInt4Oid, 4);
    f[11] = new Field(connection, "REMARKS", iVarcharOid, 32);
    f[12] = new Field(connection, "COLUMN_DEF", iVarcharOid, 32);
    f[13] = new Field(connection, "SQL_DATA_TYPE", iInt4Oid, 4);
    f[14] = new Field(connection, "SQL_DATETIME_SUB", iInt4Oid, 4);
    f[15] = new Field(connection, "CHAR_OCTET_LENGTH", iVarcharOid, 32);
    f[16] = new Field(connection, "ORDINAL_POSITION", iInt4Oid,4);
    f[17] = new Field(connection, "IS_NULLABLE", iVarcharOid, 32);
    
    // Added by Stefan Andreasen <stefan@linux.kapow.dk>
    // If the pattern are  null then set them to %
    if (tableNamePattern == null) tableNamePattern="%";
    if (columnNamePattern == null) columnNamePattern="%";
    
    // Now form the query
    // Modified by Stefan Andreasen <stefan@linux.kapow.dk>
    r = connection.ExecSQL("select a.oid,c.relname,a.attname,a.atttypid,a.attnum,a.attnotnull,a.attlen,a.atttypmod from pg_class c, pg_attribute a where a.attrelid=c.oid and c.relname like '"+tableNamePattern.toLowerCase()+"' and a.attname like '"+columnNamePattern.toLowerCase()+"' and a.attnum>0 order by c.relname,a.attnum");
    
    byte remarks[];
    
    while(r.next()) {
	byte[][] tuple = new byte[18][0];
	
	// Fetch the description for the table (if any)
	java.sql.ResultSet dr = connection.ExecSQL("select description from pg_description where objoid="+r.getInt(1));
	if(((org.postgresql.ResultSet)dr).getTupleCount()==1) {
	  dr.next();
	  tuple[11] = dr.getBytes(1);
	} else
	  tuple[11] = defaultRemarks;
	
	dr.close();
	
	tuple[0] = "".getBytes();	// Catalog name
	tuple[1] = "".getBytes();	// Schema name
	tuple[2] = r.getBytes(2);	// Table name
	tuple[3] = r.getBytes(3);	// Column name
	
	dr = connection.ExecSQL("select typname from pg_type where oid = "+r.getString(4));
	dr.next();
	String typname=dr.getString(1);
	dr.close();
	tuple[4] = Integer.toString(Field.getSQLType(typname)).getBytes();	// Data type
	tuple[5] = typname.getBytes();	// Type name
	
	// Column size
	// Looking at the psql source,
	// I think the length of a varchar as specified when the table was created
	// should be extracted from atttypmod which contains this length + sizeof(int32)
	if (typname.equals("bpchar") || typname.equals("varchar")) {
	  int atttypmod = r.getInt(8);
	  tuple[6] = Integer.toString(atttypmod != -1 ? atttypmod - VARHDRSZ : 0).getBytes();
	} else
	  tuple[6] = r.getBytes(7);
	
	tuple[7] = null;	// Buffer length
	
	tuple[8] = "0".getBytes();	// Decimal Digits - how to get this?
	tuple[9] = "10".getBytes();	// Num Prec Radix - assume decimal
	
	// tuple[10] is below
	// tuple[11] is above
	
	tuple[12] = null;	// column default
	
	tuple[13] = null;	// sql data type (unused)
	tuple[14] = null;	// sql datetime sub (unused)
	
	tuple[15] = tuple[6];	// char octet length
	
	tuple[16] = r.getBytes(5);	// ordinal position
	
	String nullFlag = r.getString(6);
	tuple[10] = Integer.toString(nullFlag.equals("f")?java.sql.DatabaseMetaData.columnNullable:java.sql.DatabaseMetaData.columnNoNulls).getBytes();	// Nullable
	tuple[17] = (nullFlag.equals("f")?"YES":"NO").getBytes();	// is nullable
	
	v.addElement(tuple);
      }
    r.close();
    return new ResultSet(connection, f, v, "OK", 1);
  }
  
  /**
   * Get a description of the access rights for a table's columns.
   *
   * <P>Only privileges matching the column name criteria are
   * returned.  They are ordered by COLUMN_NAME and PRIVILEGE.
   *
   * <P>Each privilige description has the following columns:
   *  <OL>
   *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
   *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
   *	<LI><B>TABLE_NAME</B> String => table name
   *	<LI><B>COLUMN_NAME</B> String => column name
   *	<LI><B>GRANTOR</B> => grantor of access (may be null)
   *	<LI><B>GRANTEE</B> String => grantee of access
   *	<LI><B>PRIVILEGE</B> String => name of access (SELECT,
   *      INSERT, UPDATE, REFRENCES, ...)
   *	<LI><B>IS_GRANTABLE</B> String => "YES" if grantee is permitted
   *      to grant to others; "NO" if not; null if unknown
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name; "" retrieves those without a schema
   * @param table a table name
   * @param columnNamePattern a column name pattern
   * @return ResultSet each row is a column privilege description
   * @see #getSearchStringEscape
   */
  public java.sql.ResultSet getColumnPrivileges(String catalog, String schema, String table, String columnNamePattern) throws SQLException
  {
    Field f[] = new Field[8];
    Vector v = new Vector();
    
    if(table==null)
      table="%";
    
    if(columnNamePattern==null)
      columnNamePattern="%";
    else
      columnNamePattern=columnNamePattern.toLowerCase();
    
    f[0] = new Field(connection,"TABLE_CAT",iVarcharOid,32);
    f[1] = new Field(connection,"TABLE_SCHEM",iVarcharOid,32);
    f[2] = new Field(connection,"TABLE_NAME",iVarcharOid,32);
    f[3] = new Field(connection,"COLUMN_NAME",iVarcharOid,32);
    f[4] = new Field(connection,"GRANTOR",iVarcharOid,32);
    f[5] = new Field(connection,"GRANTEE",iVarcharOid,32);
    f[6] = new Field(connection,"PRIVILEGE",iVarcharOid,32);
    f[7] = new Field(connection,"IS_GRANTABLE",iVarcharOid,32);
    
    // This is taken direct from the psql source
    java.sql.ResultSet r = connection.ExecSQL("SELECT relname, relacl FROM pg_class, pg_user WHERE ( relkind = 'r' OR relkind = 'i') and relname !~ '^pg_' and relname !~ '^xin[vx][0-9]+' and usesysid = relowner and relname like '"+table.toLowerCase()+"' ORDER BY relname");
    while(r.next()) {
      byte[][] tuple = new byte[8][0];
      tuple[0] = tuple[1]= "".getBytes();
      DriverManager.println("relname=\""+r.getString(1)+"\" relacl=\""+r.getString(2)+"\"");
      
      // For now, don't add to the result as relacl needs to be processed.
      //v.addElement(tuple);
    }
    
    return new ResultSet(connection,f,v,"OK",1);
  }
  
  /**
   * Get a description of the access rights for each table available
   * in a catalog.
   *
   * <P>Only privileges matching the schema and table name
   * criteria are returned.  They are ordered by TABLE_SCHEM,
   * TABLE_NAME, and PRIVILEGE.
   *
   * <P>Each privilige description has the following columns:
   *  <OL>
   *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
   *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
   *	<LI><B>TABLE_NAME</B> String => table name
   *	<LI><B>COLUMN_NAME</B> String => column name
   *	<LI><B>GRANTOR</B> => grantor of access (may be null)
   *	<LI><B>GRANTEE</B> String => grantee of access
   *	<LI><B>PRIVILEGE</B> String => name of access (SELECT,
   *      INSERT, UPDATE, REFRENCES, ...)
   *	<LI><B>IS_GRANTABLE</B> String => "YES" if grantee is permitted
   *      to grant to others; "NO" if not; null if unknown
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schemaPattern a schema name pattern; "" retrieves those
   * without a schema
   * @param tableNamePattern a table name pattern
   * @return ResultSet each row is a table privilege description
   * @see #getSearchStringEscape
   */
  public java.sql.ResultSet getTablePrivileges(String catalog, String schemaPattern, String tableNamePattern) throws SQLException
  {
    // XXX-Not Implemented
    return null;
  }
  
  /**
   * Get a description of a table's optimal set of columns that
   * uniquely identifies a row. They are ordered by SCOPE.
   *
   * <P>Each column description has the following columns:
   *  <OL>
   *	<LI><B>SCOPE</B> short => actual scope of result
   *      <UL>
   *      <LI> bestRowTemporary - very temporary, while using row
   *      <LI> bestRowTransaction - valid for remainder of current transaction
   *      <LI> bestRowSession - valid for remainder of current session
   *      </UL>
   *	<LI><B>COLUMN_NAME</B> String => column name
   *	<LI><B>DATA_TYPE</B> short => SQL data type from java.sql.Types
   *	<LI><B>TYPE_NAME</B> String => Data source dependent type name
   *	<LI><B>COLUMN_SIZE</B> int => precision
   *	<LI><B>BUFFER_LENGTH</B> int => not used
   *	<LI><B>DECIMAL_DIGITS</B> short	 => scale
   *	<LI><B>PSEUDO_COLUMN</B> short => is this a pseudo column
   *      like an Oracle ROWID
   *      <UL>
   *      <LI> bestRowUnknown - may or may not be pseudo column
   *      <LI> bestRowNotPseudo - is NOT a pseudo column
   *      <LI> bestRowPseudo - is a pseudo column
   *      </UL>
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name; "" retrieves those without a schema
   * @param table a table name
   * @param scope the scope of interest; use same values as SCOPE
   * @param nullable include columns that are nullable?
   * @return ResultSet each row is a column description
   */
  // Implementation note: This is required for Borland's JBuilder to work
  public java.sql.ResultSet getBestRowIdentifier(String catalog, String schema, String table, int scope, boolean nullable) throws SQLException
  {
    // for now, this returns an empty result set.
    Field f[] = new Field[8];
    ResultSet r;	// ResultSet for the SQL query that we need to do
    Vector v = new Vector();		// The new ResultSet tuple stuff
    
    f[0] = new Field(connection, "SCOPE", iInt2Oid, 2);
    f[1] = new Field(connection, "COLUMN_NAME", iVarcharOid, 32);
    f[2] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
    f[3] = new Field(connection, "TYPE_NAME", iVarcharOid, 32);
    f[4] = new Field(connection, "COLUMN_SIZE", iInt4Oid, 4);
    f[5] = new Field(connection, "BUFFER_LENGTH", iInt4Oid, 4);
    f[6] = new Field(connection, "DECIMAL_DIGITS", iInt2Oid, 2);
    f[7] = new Field(connection, "PSEUDO_COLUMN", iInt2Oid, 2);
    
    return new ResultSet(connection, f, v, "OK", 1);
  }
  
  /**
   * Get a description of a table's columns that are automatically
   * updated when any value in a row is updated.  They are
   * unordered.
   *
   * <P>Each column description has the following columns:
   *  <OL>
   *	<LI><B>SCOPE</B> short => is not used
   *	<LI><B>COLUMN_NAME</B> String => column name
   *	<LI><B>DATA_TYPE</B> short => SQL data type from java.sql.Types
   *	<LI><B>TYPE_NAME</B> String => Data source dependent type name
   *	<LI><B>COLUMN_SIZE</B> int => precision
   *	<LI><B>BUFFER_LENGTH</B> int => length of column value in bytes
   *	<LI><B>DECIMAL_DIGITS</B> short	 => scale
   *	<LI><B>PSEUDO_COLUMN</B> short => is this a pseudo column
   *      like an Oracle ROWID
   *      <UL>
   *      <LI> versionColumnUnknown - may or may not be pseudo column
   *      <LI> versionColumnNotPseudo - is NOT a pseudo column
   *      <LI> versionColumnPseudo - is a pseudo column
   *      </UL>
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name; "" retrieves those without a schema
   * @param table a table name
   * @return ResultSet each row is a column description
   */
 public java.sql.ResultSet getVersionColumns(String catalog, String schema, String table) throws SQLException
  {
    // XXX-Not Implemented
    return null;
  }
  
  /**
   * Get a description of a table's primary key columns.  They
   * are ordered by COLUMN_NAME.
   *
   * <P>Each column description has the following columns:
   *  <OL>
   *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
   *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
   *	<LI><B>TABLE_NAME</B> String => table name
   *	<LI><B>COLUMN_NAME</B> String => column name
   *	<LI><B>KEY_SEQ</B> short => sequence number within primary key
   *	<LI><B>PK_NAME</B> String => primary key name (may be null)
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name pattern; "" retrieves those
   * without a schema
   * @param table a table name
   * @return ResultSet each row is a primary key column description
   */
  public java.sql.ResultSet getPrimaryKeys(String catalog, String schema, String table) throws SQLException
  {
    return connection.createStatement().executeQuery("SELECT " +
                                                     "'' as TABLE_CAT," +
                                                     "'' AS TABLE_SCHEM," +
                                                     "bc.relname AS TABLE_NAME," +
                                                     "a.attname AS COLUMN_NAME," +
                                                     "a.attnum as KEY_SEQ,"+
                                                     "ic.relname as PK_NAME " +
                                                     " FROM pg_class bc, pg_class ic, pg_index i, pg_attribute a" +
                                                     " WHERE bc.relkind = 'r' " + //    -- not indices
                                                     "  and upper(bc.relname) = upper('"+table+"')" +
                                                     "  and i.indrelid = bc.oid" +
                                                     "  and i.indexrelid = ic.oid" +
                                                     "  and ic.oid = a.attrelid" +
                                                     "  and i.indisprimary='t' " +
                                                     " ORDER BY table_name, pk_name, key_seq"
                                                     );
  }
  
  /**
   * Get a description of the primary key columns that are
   * referenced by a table's foreign key columns (the primary keys
   * imported by a table).  They are ordered by PKTABLE_CAT,
   * PKTABLE_SCHEM, PKTABLE_NAME, and KEY_SEQ.
   *
   * <P>Each primary key column description has the following columns:
   *  <OL>
   *	<LI><B>PKTABLE_CAT</B> String => primary key table catalog
   *      being imported (may be null)
   *	<LI><B>PKTABLE_SCHEM</B> String => primary key table schema
   *      being imported (may be null)
   *	<LI><B>PKTABLE_NAME</B> String => primary key table name
   *      being imported
   *	<LI><B>PKCOLUMN_NAME</B> String => primary key column name
   *      being imported
   *	<LI><B>FKTABLE_CAT</B> String => foreign key table catalog (may be null)
   *	<LI><B>FKTABLE_SCHEM</B> String => foreign key table schema (may be null)
   *	<LI><B>FKTABLE_NAME</B> String => foreign key table name
   *	<LI><B>FKCOLUMN_NAME</B> String => foreign key column name
   *	<LI><B>KEY_SEQ</B> short => sequence number within foreign key
   *	<LI><B>UPDATE_RULE</B> short => What happens to
   *       foreign key when primary is updated:
   *      <UL>
   *      <LI> importedKeyCascade - change imported key to agree
   *               with primary key update
   *      <LI> importedKeyRestrict - do not allow update of primary
   *               key if it has been imported
   *      <LI> importedKeySetNull - change imported key to NULL if
   *               its primary key has been updated
   *      </UL>
   *	<LI><B>DELETE_RULE</B> short => What happens to
   *      the foreign key when primary is deleted.
   *      <UL>
   *      <LI> importedKeyCascade - delete rows that import a deleted key
   *      <LI> importedKeyRestrict - do not allow delete of primary
   *               key if it has been imported
   *      <LI> importedKeySetNull - change imported key to NULL if
   *               its primary key has been deleted
   *      </UL>
   *	<LI><B>FK_NAME</B> String => foreign key name (may be null)
   *	<LI><B>PK_NAME</B> String => primary key name (may be null)
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name pattern; "" retrieves those
   * without a schema
   * @param table a table name
   * @return ResultSet each row is a primary key column description
   * @see #getExportedKeys
   */
  public java.sql.ResultSet getImportedKeys(String catalog, String schema, String table) throws SQLException
  {
    // XXX-Not Implemented
    return null;
  }
  
  /**
   * Get a description of a foreign key columns that reference a
   * table's primary key columns (the foreign keys exported by a
   * table).  They are ordered by FKTABLE_CAT, FKTABLE_SCHEM,
   * FKTABLE_NAME, and KEY_SEQ.
   *
   * <P>Each foreign key column description has the following columns:
   *  <OL>
   *	<LI><B>PKTABLE_CAT</B> String => primary key table catalog (may be null)
   *	<LI><B>PKTABLE_SCHEM</B> String => primary key table schema (may be null)
   *	<LI><B>PKTABLE_NAME</B> String => primary key table name
   *	<LI><B>PKCOLUMN_NAME</B> String => primary key column name
   *	<LI><B>FKTABLE_CAT</B> String => foreign key table catalog (may be null)
   *      being exported (may be null)
   *	<LI><B>FKTABLE_SCHEM</B> String => foreign key table schema (may be null)
   *      being exported (may be null)
   *	<LI><B>FKTABLE_NAME</B> String => foreign key table name
   *      being exported
   *	<LI><B>FKCOLUMN_NAME</B> String => foreign key column name
   *      being exported
   *	<LI><B>KEY_SEQ</B> short => sequence number within foreign key
   *	<LI><B>UPDATE_RULE</B> short => What happens to
   *       foreign key when primary is updated:
   *      <UL>
   *      <LI> importedKeyCascade - change imported key to agree
   *               with primary key update
   *      <LI> importedKeyRestrict - do not allow update of primary
   *               key if it has been imported
   *      <LI> importedKeySetNull - change imported key to NULL if
   *               its primary key has been updated
   *      </UL>
   *	<LI><B>DELETE_RULE</B> short => What happens to
   *      the foreign key when primary is deleted.
   *      <UL>
   *      <LI> importedKeyCascade - delete rows that import a deleted key
   *      <LI> importedKeyRestrict - do not allow delete of primary
   *               key if it has been imported
   *      <LI> importedKeySetNull - change imported key to NULL if
   *               its primary key has been deleted
   *      </UL>
   *	<LI><B>FK_NAME</B> String => foreign key identifier (may be null)
   *	<LI><B>PK_NAME</B> String => primary key identifier (may be null)
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name pattern; "" retrieves those
   * without a schema
   * @param table a table name
   * @return ResultSet each row is a foreign key column description
   * @see #getImportedKeys
   */
  public java.sql.ResultSet getExportedKeys(String catalog, String schema, String table) throws SQLException
  {
    // XXX-Not Implemented
    return null;
  }
  
  /**
   * Get a description of the foreign key columns in the foreign key
   * table that reference the primary key columns of the primary key
   * table (describe how one table imports another's key.) This
   * should normally return a single foreign key/primary key pair
   * (most tables only import a foreign key from a table once.)  They
   * are ordered by FKTABLE_CAT, FKTABLE_SCHEM, FKTABLE_NAME, and
   * KEY_SEQ.
   *
   * <P>Each foreign key column description has the following columns:
   *  <OL>
   *	<LI><B>PKTABLE_CAT</B> String => primary key table catalog (may be null)
   *	<LI><B>PKTABLE_SCHEM</B> String => primary key table schema (may be null)
   *	<LI><B>PKTABLE_NAME</B> String => primary key table name
   *	<LI><B>PKCOLUMN_NAME</B> String => primary key column name
   *	<LI><B>FKTABLE_CAT</B> String => foreign key table catalog (may be null)
   *      being exported (may be null)
   *	<LI><B>FKTABLE_SCHEM</B> String => foreign key table schema (may be null)
   *      being exported (may be null)
   *	<LI><B>FKTABLE_NAME</B> String => foreign key table name
   *      being exported
   *	<LI><B>FKCOLUMN_NAME</B> String => foreign key column name
   *      being exported
   *	<LI><B>KEY_SEQ</B> short => sequence number within foreign key
   *	<LI><B>UPDATE_RULE</B> short => What happens to
   *       foreign key when primary is updated:
   *      <UL>
   *      <LI> importedKeyCascade - change imported key to agree
   *               with primary key update
   *      <LI> importedKeyRestrict - do not allow update of primary
   *               key if it has been imported
   *      <LI> importedKeySetNull - change imported key to NULL if
   *               its primary key has been updated
   *      </UL>
   *	<LI><B>DELETE_RULE</B> short => What happens to
   *      the foreign key when primary is deleted.
   *      <UL>
   *      <LI> importedKeyCascade - delete rows that import a deleted key
   *      <LI> importedKeyRestrict - do not allow delete of primary
   *               key if it has been imported
   *      <LI> importedKeySetNull - change imported key to NULL if
   *               its primary key has been deleted
   *      </UL>
   *	<LI><B>FK_NAME</B> String => foreign key identifier (may be null)
   *	<LI><B>PK_NAME</B> String => primary key identifier (may be null)
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name pattern; "" retrieves those
   * without a schema
   * @param table a table name
   * @return ResultSet each row is a foreign key column description
   * @see #getImportedKeys
   */
  public java.sql.ResultSet getCrossReference(String primaryCatalog, String primarySchema, String primaryTable, String foreignCatalog, String foreignSchema, String foreignTable) throws SQLException
  {
    // XXX-Not Implemented
    return null;
  }
  
  /**
   * Get a description of all the standard SQL types supported by
   * this database. They are ordered by DATA_TYPE and then by how
   * closely the data type maps to the corresponding JDBC SQL type.
   *
   * <P>Each type description has the following columns:
   *  <OL>
   *	<LI><B>TYPE_NAME</B> String => Type name
   *	<LI><B>DATA_TYPE</B> short => SQL data type from java.sql.Types
   *	<LI><B>PRECISION</B> int => maximum precision
   *	<LI><B>LITERAL_PREFIX</B> String => prefix used to quote a literal
   *      (may be null)
   *	<LI><B>LITERAL_SUFFIX</B> String => suffix used to quote a literal
   (may be null)
   *	<LI><B>CREATE_PARAMS</B> String => parameters used in creating
   *      the type (may be null)
   *	<LI><B>NULLABLE</B> short => can you use NULL for this type?
   *      <UL>
   *      <LI> typeNoNulls - does not allow NULL values
   *      <LI> typeNullable - allows NULL values
   *      <LI> typeNullableUnknown - nullability unknown
   *      </UL>
   *	<LI><B>CASE_SENSITIVE</B> boolean=> is it case sensitive?
   *	<LI><B>SEARCHABLE</B> short => can you use "WHERE" based on this type:
   *      <UL>
   *      <LI> typePredNone - No support
   *      <LI> typePredChar - Only supported with WHERE .. LIKE
   *      <LI> typePredBasic - Supported except for WHERE .. LIKE
   *      <LI> typeSearchable - Supported for all WHERE ..
   *      </UL>
   *	<LI><B>UNSIGNED_ATTRIBUTE</B> boolean => is it unsigned?
   *	<LI><B>FIXED_PREC_SCALE</B> boolean => can it be a money value?
   *	<LI><B>AUTO_INCREMENT</B> boolean => can it be used for an
   *      auto-increment value?
   *	<LI><B>LOCAL_TYPE_NAME</B> String => localized version of type name
   *      (may be null)
   *	<LI><B>MINIMUM_SCALE</B> short => minimum scale supported
   *	<LI><B>MAXIMUM_SCALE</B> short => maximum scale supported
   *	<LI><B>SQL_DATA_TYPE</B> int => unused
   *	<LI><B>SQL_DATETIME_SUB</B> int => unused
   *	<LI><B>NUM_PREC_RADIX</B> int => usually 2 or 10
   *  </OL>
   *
   * @return ResultSet each row is a SQL type description
   */
  public java.sql.ResultSet getTypeInfo() throws SQLException
  {
    java.sql.ResultSet rs = connection.ExecSQL("select typname from pg_type");
    if(rs!=null) {
      Field f[] = new Field[18];
      ResultSet r;	// ResultSet for the SQL query that we need to do
      Vector v = new Vector();		// The new ResultSet tuple stuff
      
      f[0] = new Field(connection, "TYPE_NAME", iVarcharOid, 32);
      f[1] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
      f[2] = new Field(connection, "PRECISION", iInt4Oid, 4);
      f[3] = new Field(connection, "LITERAL_PREFIX", iVarcharOid, 32);
      f[4] = new Field(connection, "LITERAL_SUFFIX", iVarcharOid, 32);
      f[5] = new Field(connection, "CREATE_PARAMS", iVarcharOid, 32);
      f[6] = new Field(connection, "NULLABLE", iInt2Oid, 2);
      f[7] = new Field(connection, "CASE_SENSITIVE", iBoolOid, 1);
      f[8] = new Field(connection, "SEARCHABLE", iInt2Oid, 2);
      f[9] = new Field(connection, "UNSIGNED_ATTRIBUTE", iBoolOid, 1);
      f[10] = new Field(connection, "FIXED_PREC_SCALE", iBoolOid, 1);
      f[11] = new Field(connection, "AUTO_INCREMENT", iBoolOid, 1);
      f[12] = new Field(connection, "LOCAL_TYPE_NAME", iVarcharOid, 32);
      f[13] = new Field(connection, "MINIMUM_SCALE", iInt2Oid, 2);
      f[14] = new Field(connection, "MAXIMUM_SCALE", iInt2Oid, 2);
      f[15] = new Field(connection, "SQL_DATA_TYPE", iInt4Oid, 4);
      f[16] = new Field(connection, "SQL_DATETIME_SUB", iInt4Oid, 4);
      f[17] = new Field(connection, "NUM_PREC_RADIX", iInt4Oid, 4);
      
      // cache some results, this will keep memory useage down, and speed
      // things up a little.
      byte b9[]  = "9".getBytes();
      byte b10[] = "10".getBytes();
      byte bf[]  = "f".getBytes();
      byte bnn[] = Integer.toString(typeNoNulls).getBytes();
      byte bts[] = Integer.toString(typeSearchable).getBytes();
      
      while(rs.next()) {
	byte[][] tuple = new byte[18][];
	String typname=rs.getString(1);
	tuple[0] = typname.getBytes();
	tuple[1] = Integer.toString(Field.getSQLType(typname)).getBytes();
	tuple[2] = b9;	// for now
	tuple[6] = bnn; // for now
	tuple[7] = bf; // false for now - not case sensitive
	tuple[8] = bts;
	tuple[9] = bf; // false for now - it's signed
	tuple[10] = bf; // false for now - must handle money
	tuple[11] = bf; // false for now - handle autoincrement
	// 12 - LOCAL_TYPE_NAME is null
	// 13 & 14 ?
	// 15 & 16 are unused so we return null
	tuple[17] = b10; // everything is base 10
	v.addElement(tuple);
      }
      rs.close();
      return new ResultSet(connection, f, v, "OK", 1);
    }
    
    return null;
  }
  
  /**
   * Get a description of a table's indices and statistics. They are
   * ordered by NON_UNIQUE, TYPE, INDEX_NAME, and ORDINAL_POSITION.
   *
   * <P>Each index column description has the following columns:
   *  <OL>
   *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
   *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
   *	<LI><B>TABLE_NAME</B> String => table name
   *	<LI><B>NON_UNIQUE</B> boolean => Can index values be non-unique?
   *      false when TYPE is tableIndexStatistic
   *	<LI><B>INDEX_QUALIFIER</B> String => index catalog (may be null);
   *      null when TYPE is tableIndexStatistic
   *	<LI><B>INDEX_NAME</B> String => index name; null when TYPE is
   *      tableIndexStatistic
   *	<LI><B>TYPE</B> short => index type:
   *      <UL>
   *      <LI> tableIndexStatistic - this identifies table statistics that are
   *           returned in conjuction with a table's index descriptions
   *      <LI> tableIndexClustered - this is a clustered index
   *      <LI> tableIndexHashed - this is a hashed index
   *      <LI> tableIndexOther - this is some other style of index
   *      </UL>
   *	<LI><B>ORDINAL_POSITION</B> short => column sequence number
   *      within index; zero when TYPE is tableIndexStatistic
   *	<LI><B>COLUMN_NAME</B> String => column name; null when TYPE is
   *      tableIndexStatistic
   *	<LI><B>ASC_OR_DESC</B> String => column sort sequence, "A" => ascending
   *      "D" => descending, may be null if sort sequence is not supported;
   *      null when TYPE is tableIndexStatistic
   *	<LI><B>CARDINALITY</B> int => When TYPE is tableIndexStatisic then
   *      this is the number of rows in the table; otherwise it is the
   *      number of unique values in the index.
   *	<LI><B>PAGES</B> int => When TYPE is  tableIndexStatisic then
   *      this is the number of pages used for the table, otherwise it
   *      is the number of pages used for the current index.
   *	<LI><B>FILTER_CONDITION</B> String => Filter condition, if any.
   *      (may be null)
   *  </OL>
   *
   * @param catalog a catalog name; "" retrieves those without a catalog
   * @param schema a schema name pattern; "" retrieves those without a schema
   * @param table a table name
   * @param unique when true, return only indices for unique values;
   *     when false, return indices regardless of whether unique or not
   * @param approximate when true, result is allowed to reflect approximate
   *     or out of data values; when false, results are requested to be
   *     accurate
   * @return ResultSet each row is an index column description
   */
  // Implementation note: This is required for Borland's JBuilder to work
  public java.sql.ResultSet getIndexInfo(String catalog, String schema, String table, boolean unique, boolean approximate) throws SQLException
  {
    // for now, this returns an empty result set.
    Field f[] = new Field[13];
    ResultSet r;	// ResultSet for the SQL query that we need to do
    Vector v = new Vector();		// The new ResultSet tuple stuff
    
    f[0] = new Field(connection, "TABLE_CAT", iVarcharOid, 32);
    f[1] = new Field(connection, "TABLE_SCHEM", iVarcharOid, 32);
    f[2] = new Field(connection, "TABLE_NAME", iVarcharOid, 32);
    f[3] = new Field(connection, "NON_UNIQUE", iBoolOid, 1);
    f[4] = new Field(connection, "INDEX_QUALIFIER", iVarcharOid, 32);
    f[5] = new Field(connection, "INDEX_NAME", iVarcharOid, 32);
    f[6] = new Field(connection, "TYPE", iInt2Oid, 2);
    f[7] = new Field(connection, "ORDINAL_POSITION", iInt2Oid, 2);
    f[8] = new Field(connection, "COLUMN_NAME", iVarcharOid, 32);
    f[9] = new Field(connection, "ASC_OR_DESC", iVarcharOid, 32);
    f[10] = new Field(connection, "CARDINALITY", iInt4Oid, 4);
    f[11] = new Field(connection, "PAGES", iInt4Oid, 4);
    f[12] = new Field(connection, "FILTER_CONDITION", iVarcharOid, 32);
    
    return new ResultSet(connection, f, v, "OK", 1);
  }
}

