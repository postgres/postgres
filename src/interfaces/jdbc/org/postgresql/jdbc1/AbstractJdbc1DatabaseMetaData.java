package org.postgresql.jdbc1;


import java.sql.*;
import java.util.*;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;
import org.postgresql.core.Encoding;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;
import org.postgresql.Driver;

public abstract class AbstractJdbc1DatabaseMetaData
{

	private static final String keywords = "abort,acl,add,aggregate,append,archive," +
										   "arch_store,backward,binary,boolean,change,cluster," +
										   "copy,database,delimiter,delimiters,do,extend," +
										   "explain,forward,heavy,index,inherits,isnull," +
										   "light,listen,load,merge,nothing,notify," +
										   "notnull,oids,purge,rename,replace,retrieve," +
										   "returns,rule,recipe,setof,stdin,stdout,store," +
										   "vacuum,verbose,version";

	protected AbstractJdbc1Connection connection; // The connection association
	protected Encoding encoding;

	// These define various OID's. Hopefully they will stay constant.
	protected static final int iVarcharOid = 1043;	// OID for varchar
	protected static final int iBoolOid = 16; // OID for bool
	protected static final int iInt2Oid = 21; // OID for int2
	protected static final int iInt4Oid = 23; // OID for int4
	protected static final int VARHDRSZ = 4;	// length for int4

	private int NAMEDATALEN = 0;	// length for name datatype
	private int INDEX_MAX_KEYS = 0; // maximum number of keys in an index.

	protected int getMaxIndexKeys() throws SQLException {
		if (INDEX_MAX_KEYS == 0) {
			String from;
			if (connection.haveMinimumServerVersion("7.3")) {
				from = "pg_catalog.pg_namespace n, pg_catalog.pg_type t1, pg_catalog.pg_type t2 WHERE t1.typnamespace=n.oid AND n.nspname='pg_catalog' AND ";
			} else {
				from = "pg_type t1, pg_type t2 WHERE ";
			}
			String sql = "SELECT t1.typlen/t2.typlen FROM "+from+" t1.typelem=t2.oid AND t1.typname='oidvector'";
			ResultSet rs = connection.createStatement().executeQuery(sql);
			if (!rs.next()) {
				throw new PSQLException("postgresql.unexpected", PSQLState.UNEXPECTED_ERROR);
			}
			INDEX_MAX_KEYS = rs.getInt(1);
			rs.close();
		}
		return INDEX_MAX_KEYS;
	}

	protected int getMaxNameLength() throws SQLException {
		if (NAMEDATALEN == 0) {
			String sql;
			if (connection.haveMinimumServerVersion("7.3")) {
				sql = "SELECT t.typlen FROM pg_catalog.pg_type t, pg_catalog.pg_namespace n WHERE t.typnamespace=n.oid AND t.typname='name' AND n.nspname='pg_catalog'";
			} else {
				sql = "SELECT typlen FROM pg_type WHERE typname='name'";
			}
			ResultSet rs = connection.createStatement().executeQuery(sql);
			if (!rs.next()) {
				throw new PSQLException("postgresql.unexpected", PSQLState.UNEXPECTED_ERROR);
			}
			NAMEDATALEN = rs.getInt("typlen");
			rs.close();
		}
		return NAMEDATALEN - 1;
	}

	public AbstractJdbc1DatabaseMetaData(AbstractJdbc1Connection conn)
	{
		this.connection = conn;
		try {
			this.encoding = conn.getEncoding();
		}
		catch (SQLException sqle) {
			this.encoding = Encoding.defaultEncoding();
		}

	}

	/*
	 * Can all the procedures returned by getProcedures be called
	 * by the current user?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean allProceduresAreCallable() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("allProceduresAreCallable");
		return true;		// For now...
	}

	/*
	 * Can all the tables returned by getTable be SELECTed by
	 * the current user?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean allTablesAreSelectable() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("allTablesAreSelectable");
		return true;		// For now...
	}

	/*
	 * What is the URL for this database?
	 *
	 * @return the url or null if it cannott be generated
	 * @exception SQLException if a database access error occurs
	 */
	public String getURL() throws SQLException
	{
		String url = connection.getURL();
		if (Driver.logDebug)
			Driver.debug("getURL " + url);
		return url;
	}

	/*
	 * What is our user name as known to the database?
	 *
	 * @return our database user name
	 * @exception SQLException if a database access error occurs
	 */
	public String getUserName() throws SQLException
	{
		String userName = connection.getUserName();
		if (Driver.logDebug)
			Driver.debug("getUserName " + userName);
		return userName;
	}

	/*
	 * Is the database in read-only mode?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean isReadOnly() throws SQLException
	{
		boolean isReadOnly = connection.isReadOnly();
		if (Driver.logDebug)
			Driver.debug("isReadOnly " + isReadOnly);
		return isReadOnly;
	}

	/*
	 * Are NULL values sorted high?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean nullsAreSortedHigh() throws SQLException
	{
		boolean nullSortedHigh = connection.haveMinimumServerVersion("7.2");
		if (Driver.logDebug)
			Driver.debug("nullsAreSortedHigh " + nullSortedHigh);
		return nullSortedHigh;
	}

	/*
	 * Are NULL values sorted low?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean nullsAreSortedLow() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("nullsAreSortedLow false");
		return false;
	}

	/*
	 * Are NULL values sorted at the start regardless of sort order?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean nullsAreSortedAtStart() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("nullsAreSortedAtStart false");
		return false;
	}

	/*
	 * Are NULL values sorted at the end regardless of sort order?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean nullsAreSortedAtEnd() throws SQLException
	{
		boolean nullsAreSortedAtEnd = ! connection.haveMinimumServerVersion("7.2");
		if (Driver.logDebug)
			Driver.debug("nullsAreSortedAtEnd " + nullsAreSortedAtEnd);
		return nullsAreSortedAtEnd;
	}

	/*
	 * What is the name of this database product - we hope that it is
	 * PostgreSQL, so we return that explicitly.
	 *
	 * @return the database product name
	 * @exception SQLException if a database access error occurs
	 */
	public String getDatabaseProductName() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getDatabaseProductName PostgresSQL");
		return "PostgreSQL";
	}

	/*
	 * What is the version of this database product.
	 *
	 * @return the database version
	 * @exception SQLException if a database access error occurs
	 */
	public String getDatabaseProductVersion() throws SQLException
	{
		String versionNumber = connection.getDBVersionNumber();
		if (Driver.logDebug)
			Driver.debug("getDatabaseProductVersion " + versionNumber);
		return versionNumber;
	}

	/*
	 * What is the name of this JDBC driver?  If we don't know this
	 * we are doing something wrong!
	 *
	 * @return the JDBC driver name
	 * @exception SQLException why?
	 */
	public String getDriverName() throws SQLException
	{
		String driverName = "PostgreSQL Native Driver";
		if (Driver.logDebug)
			Driver.debug("getDriverName" + driverName);
		return driverName;
	}

	/*
	 * What is the version string of this JDBC driver?	Again, this is
	 * static.
	 *
	 * @return the JDBC driver name.
	 * @exception SQLException why?
	 */
	public String getDriverVersion() throws SQLException
	{
		String driverVersion = Driver.getVersion();
		if (Driver.logDebug)
			Driver.debug("getDriverVersion " + driverVersion);
		return driverVersion;
	}

	/*
	 * What is this JDBC driver's major version number?
	 *
	 * @return the JDBC driver major version
	 */
	public int getDriverMajorVersion()
	{
		int majorVersion = connection.this_driver.getMajorVersion();
		if (Driver.logDebug)
			Driver.debug("getMajorVersion " + majorVersion);
		return majorVersion;
	}

	/*
	 * What is this JDBC driver's minor version number?
	 *
	 * @return the JDBC driver minor version
	 */
	public int getDriverMinorVersion()
	{
		int minorVersion = connection.this_driver.getMinorVersion();
		if (Driver.logDebug)
			Driver.debug("getMinorVersion " + minorVersion);
		return minorVersion;
	}

	/*
	 * Does the database store tables in a local file?	No - it
	 * stores them in a file on the server.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean usesLocalFiles() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("usesLocalFiles " + false);
		return false;
	}

	/*
	 * Does the database use a file for each table?  Well, not really,
	 * since it doesnt use local files.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean usesLocalFilePerTable() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("usesLocalFilePerTable " + false);
		return false;
	}

	/*
	 * Does the database treat mixed case unquoted SQL identifiers
	 * as case sensitive and as a result store them in mixed case?
	 * A JDBC-Compliant driver will always return false.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsMixedCaseIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsMixedCaseIdentifiers " + false);
		return false;
	}

	/*
	 * Does the database treat mixed case unquoted SQL identifiers as
	 * case insensitive and store them in upper case?
	 *
	 * @return true if so
	 */
	public boolean storesUpperCaseIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("storesUpperCaseIdentifiers " + false);
		return false;
	}

	/*
	 * Does the database treat mixed case unquoted SQL identifiers as
	 * case insensitive and store them in lower case?
	 *
	 * @return true if so
	 */
	public boolean storesLowerCaseIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("storesLowerCaseIdentifiers " + true);
		return true;
	}

	/*
	 * Does the database treat mixed case unquoted SQL identifiers as
	 * case insensitive and store them in mixed case?
	 *
	 * @return true if so
	 */
	public boolean storesMixedCaseIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("storesMixedCaseIdentifiers " + false);
		return false;
	}

	/*
	 * Does the database treat mixed case quoted SQL identifiers as
	 * case sensitive and as a result store them in mixed case?  A
	 * JDBC compliant driver will always return true.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsMixedCaseQuotedIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsMixedCaseQuotedIdentifiers " + true);
		return true;
	}

	/*
	 * Does the database treat mixed case quoted SQL identifiers as
	 * case insensitive and store them in upper case?
	 *
	 * @return true if so
	 */
	public boolean storesUpperCaseQuotedIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("storesUpperCaseQuotedIdentifiers " + false);
		return false;
	}

	/*
	 * Does the database treat mixed case quoted SQL identifiers as case
	 * insensitive and store them in lower case?
	 *
	 * @return true if so
	 */
	public boolean storesLowerCaseQuotedIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("storesLowerCaseQuotedIdentifiers " + false);
		return false;
	}

	/*
	 * Does the database treat mixed case quoted SQL identifiers as case
	 * insensitive and store them in mixed case?
	 *
	 * @return true if so
	 */
	public boolean storesMixedCaseQuotedIdentifiers() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("storesMixedCaseQuotedIdentifiers " + false);
		return false;
	}

	/*
	 * What is the string used to quote SQL identifiers?  This returns
	 * a space if identifier quoting isn't supported.  A JDBC Compliant
	 * driver will always use a double quote character.
	 *
	 * @return the quoting string
	 * @exception SQLException if a database access error occurs
	 */
	public String getIdentifierQuoteString() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getIdentifierQuoteString \"" );
		return "\"";
	}

	/*
	 * Get a comma separated list of all a database's SQL keywords that
	 * are NOT also SQL92 keywords.
	 *
	 * <p>Within PostgreSQL, the keywords are found in
	 *	src/backend/parser/keywords.c
	 *
	 * <p>For SQL Keywords, I took the list provided at
	 *	<a href="http://web.dementia.org/~shadow/sql/sql3bnf.sep93.txt">
	 * http://web.dementia.org/~shadow/sql/sql3bnf.sep93.txt</a>
	 * which is for SQL3, not SQL-92, but it is close enough for
	 * this purpose.
	 *
	 * @return a comma separated list of keywords we use
	 * @exception SQLException if a database access error occurs
	 */
	public String getSQLKeywords() throws SQLException
	{
		return keywords;
	}

	public String getNumericFunctions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getNumericFunctions");
		return "";
	}

	public String getStringFunctions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getStringFunctions");
		return "";
	}

	public String getSystemFunctions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getSystemFunctions");
		return "";
	}

	public String getTimeDateFunctions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getTimeDateFunctions");
		return "";
	}

	/*
	 * This is the string that can be used to escape '_' and '%' in
	 * a search string pattern style catalog search parameters
	 *
	 * @return the string used to escape wildcard characters
	 * @exception SQLException if a database access error occurs
	 */
	public String getSearchStringEscape() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getSearchStringEscape");
		return "\\\\";
	}

	/*
	 * Get all the "extra" characters that can be used in unquoted
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
		if (Driver.logDebug)
			Driver.debug("getExtraNameCharacters");
		return "";
	}

	/*
	 * Is "ALTER TABLE" with an add column supported?
	 * Yes for PostgreSQL 6.1
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsAlterTableWithAddColumn() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsAlterTableWithAddColumn " + true);
		return true;
	}

	/*
	 * Is "ALTER TABLE" with a drop column supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsAlterTableWithDropColumn() throws SQLException
	{
		boolean dropColumn = connection.haveMinimumServerVersion("7.3");
		if (Driver.logDebug)
			Driver.debug("supportsAlterTableWithDropColumn " + dropColumn);
		return dropColumn;
	}

	/*
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
		if (Driver.logDebug)
			Driver.debug("supportsColumnAliasing " + true);
		return true;
	}

	/*
	 * Are concatenations between NULL and non-NULL values NULL?  A
	 * JDBC Compliant driver always returns true
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean nullPlusNonNullIsNull() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("nullPlusNonNullIsNull " + true);
		return true;
	}

	public boolean supportsConvert() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsConvert " + false);
		return false;
	}

	public boolean supportsConvert(int fromType, int toType) throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsConvert " + false);
		return false;
	}

	/*
	 * Are table correlation names supported? A JDBC Compliant
	 * driver always returns true.
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsTableCorrelationNames() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsTableCorrelationNames " + true);
		return true;
	}

	/*
	 * If table correlation names are supported, are they restricted to
	 * be different from the names of the tables?
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsDifferentTableCorrelationNames() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsDifferentTableCorrelationNames " + false);
		return false;
	}

	/*
	 * Are expressions in "ORDER BY" lists supported?
	 *
	 * <br>e.g. select * from t order by a + b;
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsExpressionsInOrderBy() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsExpressionsInOrderBy " + true);
		return true;
	}

	/*
	 * Can an "ORDER BY" clause use columns not in the SELECT?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsOrderByUnrelated() throws SQLException
	{
		boolean supportsOrderByUnrelated = connection.haveMinimumServerVersion("6.4");
		if (Driver.logDebug)
			Driver.debug("supportsOrderByUnrelated " + supportsOrderByUnrelated);
		return supportsOrderByUnrelated;
	}

	/*
	 * Is some form of "GROUP BY" clause supported?
	 * I checked it, and yes it is.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsGroupBy() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsGroupBy " + true);
		return true;
	}

	/*
	 * Can a "GROUP BY" clause use columns not in the SELECT?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsGroupByUnrelated() throws SQLException
	{
		boolean supportsGroupByUnrelated = connection.haveMinimumServerVersion("6.4");
		if (Driver.logDebug)
			Driver.debug("supportsGroupByUnrelated " + supportsGroupByUnrelated);
		return supportsGroupByUnrelated;
	}

	/*
	 * Can a "GROUP BY" clause add columns not in the SELECT provided
	 * it specifies all the columns in the SELECT?	Does anyone actually
	 * understand what they mean here?
	 *
	 * (I think this is a subset of the previous function. -- petere)
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsGroupByBeyondSelect() throws SQLException
	{
		boolean supportsGroupByBeyondSelect = connection.haveMinimumServerVersion("6.4");
		if (Driver.logDebug)
			Driver.debug("supportsGroupByUnrelated " + supportsGroupByBeyondSelect);
		return supportsGroupByBeyondSelect;
	}

	/*
	 * Is the escape character in "LIKE" clauses supported?  A
	 * JDBC compliant driver always returns true.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsLikeEscapeClause() throws SQLException
	{
		boolean supportsLikeEscapeClause = connection.haveMinimumServerVersion("7.1");
		if (Driver.logDebug)
			Driver.debug("supportsLikeEscapeClause " + supportsLikeEscapeClause);
		return supportsLikeEscapeClause;
	}

	/*
	 * Are multiple ResultSets from a single execute supported?
	 * Well, I implemented it, but I dont think this is possible from
	 * the back ends point of view.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsMultipleResultSets() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsMultipleResultSets " + false);
		return false;
	}

	/*
	 * Can we have multiple transactions open at once (on different
	 * connections?)
	 * I guess we can have, since Im relying on it.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsMultipleTransactions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsMultipleTransactions " + true);
		return true;
	}

	/*
	 * Can columns be defined as non-nullable.	A JDBC Compliant driver
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
		if (Driver.logDebug)
			Driver.debug("supportsNonNullableColumns true");
		return true;
	}

	/*
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
		if (Driver.logDebug)
			Driver.debug("supportsMinimumSQLGrammar TRUE");
		return true;
	}

	/*
	 * Does this driver support the Core ODBC SQL grammar.	We need
	 * SQL-92 conformance for this.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsCoreSQLGrammar() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsCoreSQLGrammar FALSE ");
		return false;
	}

	/*
	 * Does this driver support the Extended (Level 2) ODBC SQL
	 * grammar.  We don't conform to the Core (Level 1), so we can't
	 * conform to the Extended SQL Grammar.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsExtendedSQLGrammar() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsExtendedSQLGrammar FALSE");
		return false;
	}

	/*
	 * Does this driver support the ANSI-92 entry level SQL grammar?
	 * All JDBC Compliant drivers must return true. We currently
	 * report false until 'schema' support is added.  Then this
	 * should be changed to return true, since we will be mostly
	 * compliant (probably more compliant than many other databases)
	 * And since this is a requirement for all JDBC drivers we
	 * need to get to the point where we can return true.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsANSI92EntryLevelSQL() throws SQLException
	{
		boolean schemas = connection.haveMinimumServerVersion("7.3");
		if (Driver.logDebug)
			Driver.debug("supportsANSI92EntryLevelSQL " + schemas);
		return schemas;
	}

	/*
	 * Does this driver support the ANSI-92 intermediate level SQL
	 * grammar?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsANSI92IntermediateSQL() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsANSI92IntermediateSQL false ");
		return false;
	}

	/*
	 * Does this driver support the ANSI-92 full SQL grammar?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsANSI92FullSQL() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsANSI92FullSQL false ");
		return false;
	}

	/*
	 * Is the SQL Integrity Enhancement Facility supported?
	 * Our best guess is that this means support for constraints
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsIntegrityEnhancementFacility() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsIntegrityEnhancementFacility true ");
		return true;
	}

	/*
	 * Is some form of outer join supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsOuterJoins() throws SQLException
	{
		boolean supportsOuterJoins = connection.haveMinimumServerVersion("7.1");
		if (Driver.logDebug)
			Driver.debug("supportsOuterJoins " + supportsOuterJoins);
		return supportsOuterJoins;
	}

	/*
	 * Are full nexted outer joins supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsFullOuterJoins() throws SQLException
	{
		boolean supportsFullOuterJoins = connection.haveMinimumServerVersion("7.1");
		if (Driver.logDebug)
			Driver.debug("supportsFullOuterJoins " + supportsFullOuterJoins);
		return supportsFullOuterJoins;
	}

	/*
	 * Is there limited support for outer joins?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsLimitedOuterJoins() throws SQLException
	{
		boolean supportsLimitedOuterJoins = connection.haveMinimumServerVersion("7.1");
		if (Driver.logDebug)
			Driver.debug("supportsFullOuterJoins " + supportsLimitedOuterJoins);
		return supportsLimitedOuterJoins;
	}

	/*
	 * What is the database vendor's preferred term for "schema"?
	 * PostgreSQL doesn't have schemas, but when it does, we'll use the
	 * term "schema".
	 *
	 * @return the vendor term
	 * @exception SQLException if a database access error occurs
	 */
	public String getSchemaTerm() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getSchemaTerm schema");
		return "schema";
	}

	/*
	 * What is the database vendor's preferred term for "procedure"?
	 * Traditionally, "function" has been used.
	 *
	 * @return the vendor term
	 * @exception SQLException if a database access error occurs
	 */
	public String getProcedureTerm() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getProcedureTerm function ");
		return "function";
	}

	/*
	 * What is the database vendor's preferred term for "catalog"?
	 *
	 * @return the vendor term
	 * @exception SQLException if a database access error occurs
	 */
	public String getCatalogTerm() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("getCatalogTerm database ");
		return "database";
	}

	/*
	 * Does a catalog appear at the start of a qualified table name?
	 * (Otherwise it appears at the end).
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean isCatalogAtStart() throws SQLException
	{
		// return true here; we return false for every other catalog function
		// so it won't matter what we return here D.C.
		if (Driver.logDebug)
			Driver.debug("isCatalogAtStart not implemented");
		return true;
	}

	/*
	 * What is the Catalog separator.
	 *
	 * @return the catalog separator string
	 * @exception SQLException if a database access error occurs
	 */
	public String getCatalogSeparator() throws SQLException
	{
		// Give them something to work with here
		// everything else returns false so it won't matter what we return here D.C.
		if (Driver.logDebug)
			Driver.debug("getCatalogSeparator not implemented ");
		return ".";
	}

	/*
	 * Can a schema name be used in a data manipulation statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsSchemasInDataManipulation() throws SQLException
	{
		boolean schemas = connection.haveMinimumServerVersion("7.3");
		if (Driver.logDebug)
			Driver.debug("supportsSchemasInDataManipulation "+schemas);
		return schemas;
	}

	/*
	 * Can a schema name be used in a procedure call statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsSchemasInProcedureCalls() throws SQLException
	{
		boolean schemas = connection.haveMinimumServerVersion("7.3");
		if (Driver.logDebug)
			Driver.debug("supportsSchemasInProcedureCalls "+schemas);
		return schemas;
	}

	/*
	 * Can a schema be used in a table definition statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsSchemasInTableDefinitions() throws SQLException
	{
		boolean schemas = connection.haveMinimumServerVersion("7.3");

		if (Driver.logDebug)
			Driver.debug("supportsSchemasInTableDefinitions " + schemas);
		return schemas;
	}

	/*
	 * Can a schema name be used in an index definition statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsSchemasInIndexDefinitions() throws SQLException
	{
		boolean schemas = connection.haveMinimumServerVersion("7.3");
		if (Driver.logDebug)
			Driver.debug("supportsSchemasInIndexDefinitions "+schemas);
		return schemas;
	}

	/*
	 * Can a schema name be used in a privilege definition statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsSchemasInPrivilegeDefinitions() throws SQLException
	{
		boolean schemas = connection.haveMinimumServerVersion("7.3");
		if (Driver.logDebug)
			Driver.debug("supportsSchemasInPrivilegeDefinitions "+schemas);
		return schemas;
	}

	/*
	 * Can a catalog name be used in a data manipulation statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsCatalogsInDataManipulation() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsCatalogsInDataManipulation false");
		return false;
	}

	/*
	 * Can a catalog name be used in a procedure call statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsCatalogsInProcedureCalls() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsCatalogsInDataManipulation false");
		return false;
	}

	/*
	 * Can a catalog name be used in a table definition statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsCatalogsInTableDefinitions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsCatalogsInTableDefinitions false");
		return false;
	}

	/*
	 * Can a catalog name be used in an index definition?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsCatalogsInIndexDefinitions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsCatalogsInIndexDefinitions false");
		return false;
	}

	/*
	 * Can a catalog name be used in a privilege definition statement?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsCatalogsInPrivilegeDefinitions() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsCatalogsInPrivilegeDefinitions false");
		return false;
	}

	/*
	 * We support cursors for gets only it seems.  I dont see a method
	 * to get a positioned delete.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsPositionedDelete() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsPositionedDelete false");
		return false;			// For now...
	}

	/*
	 * Is positioned UPDATE supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsPositionedUpdate() throws SQLException
	{
		if (Driver.logDebug)
			Driver.debug("supportsPositionedUpdate false");
		return false;			// For now...
	}

	/*
	 * Is SELECT for UPDATE supported?
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsSelectForUpdate() throws SQLException
	{
		return connection.haveMinimumServerVersion("6.5");
	}

	/*
	 * Are stored procedure calls using the stored procedure escape
	 * syntax supported?
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsStoredProcedures() throws SQLException
	{
		return false;
	}

	/*
	 * Are subqueries in comparison expressions supported? A JDBC
	 * Compliant driver always returns true.
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsSubqueriesInComparisons() throws SQLException
	{
		return true;
	}

	/*
	 * Are subqueries in 'exists' expressions supported? A JDBC
	 * Compliant driver always returns true.
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsSubqueriesInExists() throws SQLException
	{
		return true;
	}

	/*
	 * Are subqueries in 'in' statements supported? A JDBC
	 * Compliant driver always returns true.
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsSubqueriesInIns() throws SQLException
	{
		return true;
	}

	/*
	 * Are subqueries in quantified expressions supported? A JDBC
	 * Compliant driver always returns true.
	 *
	 * (No idea what this is, but we support a good deal of
	 * subquerying.)
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsSubqueriesInQuantifieds() throws SQLException
	{
		return true;
	}

	/*
	 * Are correlated subqueries supported? A JDBC Compliant driver
	 * always returns true.
	 *
	 * (a.k.a. subselect in from?)
	 *
	 * @return true if so; false otherwise
	 * @exception SQLException - if a database access error occurs
	 */
	public boolean supportsCorrelatedSubqueries() throws SQLException
	{
		return connection.haveMinimumServerVersion("7.1");
	}

	/*
	 * Is SQL UNION supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsUnion() throws SQLException
	{
		return true; // since 6.3
	}

	/*
	 * Is SQL UNION ALL supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsUnionAll() throws SQLException
	{
		return connection.haveMinimumServerVersion("7.1");
	}

	/*
	 * In PostgreSQL, Cursors are only open within transactions.
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsOpenCursorsAcrossCommit() throws SQLException
	{
		return false;
	}

	/*
	 * Do we support open cursors across multiple transactions?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsOpenCursorsAcrossRollback() throws SQLException
	{
		return false;
	}

	/*
	 * Can statements remain open across commits?  They may, but
	 * this driver cannot guarentee that.  In further reflection.
	 * we are talking a Statement object here, so the answer is
	 * yes, since the Statement is only a vehicle to ExecSQL()
	 *
	 * @return true if they always remain open; false otherwise
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsOpenStatementsAcrossCommit() throws SQLException
	{
		return true;
	}

	/*
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

	/*
	 * How many hex characters can you have in an inline binary literal
	 *
	 * @return the max literal length
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxBinaryLiteralLength() throws SQLException
	{
		return 0; // no limit
	}

	/*
	 * What is the maximum length for a character literal
	 * I suppose it is 8190 (8192 - 2 for the quotes)
	 *
	 * @return the max literal length
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxCharLiteralLength() throws SQLException
	{
		return 0; // no limit
	}

	/*
	 * Whats the limit on column name length.
	 *
	 * @return the maximum column name length
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxColumnNameLength() throws SQLException
	{
		return getMaxNameLength();
	}

	/*
	 * What is the maximum number of columns in a "GROUP BY" clause?
	 *
	 * @return the max number of columns
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxColumnsInGroupBy() throws SQLException
	{
		return 0; // no limit
	}

	/*
	 * What's the maximum number of columns allowed in an index?
	 *
	 * @return max number of columns
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxColumnsInIndex() throws SQLException
	{
		return getMaxIndexKeys();
	}

	/*
	 * What's the maximum number of columns in an "ORDER BY clause?
	 *
	 * @return the max columns
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxColumnsInOrderBy() throws SQLException
	{
		return 0; // no limit
	}

	/*
	 * What is the maximum number of columns in a "SELECT" list?
	 *
	 * @return the max columns
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxColumnsInSelect() throws SQLException
	{
		return 0; // no limit
	}

	/*
	 * What is the maximum number of columns in a table? From the
	 * CREATE TABLE reference page...
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

	/*
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

	/*
	 * What is the maximum cursor name length
	 *
	 * @return max cursor name length in bytes
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxCursorNameLength() throws SQLException
	{
		return getMaxNameLength();
	}

	/*
	 * Retrieves the maximum number of bytes for an index, including all
	 * of the parts of the index.
	 *
	 * @return max index length in bytes, which includes the composite
	 * of all the constituent parts of the index; a result of zero means
	 * that there is no limit or the limit is not known
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxIndexLength() throws SQLException
	{
		return 0; // no limit (larger than an int anyway)
	}

	public int getMaxSchemaNameLength() throws SQLException
	{
		return getMaxNameLength();
	}

	/*
	 * What is the maximum length of a procedure name
	 *
	 * @return the max name length in bytes
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxProcedureNameLength() throws SQLException
	{
		return getMaxNameLength();
	}

	public int getMaxCatalogNameLength() throws SQLException
	{
		return getMaxNameLength();
	}

	/*
	 * What is the maximum length of a single row?
	 *
	 * @return max row size in bytes
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxRowSize() throws SQLException
	{
		if (connection.haveMinimumServerVersion("7.1"))
			return 1073741824;	// 1 GB
		else
			return 8192;		// XXX could be altered
	}

	/*
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

	/*
	 * What is the maximum length of a SQL statement?
	 *
	 * @return max length in bytes
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxStatementLength() throws SQLException
	{
		if (connection.haveMinimumServerVersion("7.0"))
			return 0;		// actually whatever fits in size_t
		else
			return 16384;
	}

	/*
	 * How many active statements can we have open at one time to
	 * this database?  Basically, since each Statement downloads
	 * the results as the query is executed, we can have many.	However,
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

	/*
	 * What is the maximum length of a table name
	 *
	 * @return max name length in bytes
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxTableNameLength() throws SQLException
	{
		return getMaxNameLength();
	}

	/*
	 * What is the maximum number of tables that can be specified
	 * in a SELECT?
	 *
	 * @return the maximum
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxTablesInSelect() throws SQLException
	{
		return 0; // no limit
	}

	/*
	 * What is the maximum length of a user name
	 *
	 * @return the max name length in bytes
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxUserNameLength() throws SQLException
	{
		return getMaxNameLength();
	}


	/*
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

	/*
	 * Are transactions supported?	If not, commit and rollback are noops
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

	/*
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

	/*
	 * Are both data definition and data manipulation transactions
	 * supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsDataDefinitionAndDataManipulationTransactions() throws SQLException
	{
		return true;
	}

	/*
	 * Are only data manipulation statements withing a transaction
	 * supported?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean supportsDataManipulationTransactionsOnly() throws SQLException
	{
		return false;
	}

	/*
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

	/*
	 * Is a data definition statement within a transaction ignored?
	 *
	 * @return true if so
	 * @exception SQLException if a database access error occurs
	 */
	public boolean dataDefinitionIgnoredInTransactions() throws SQLException
	{
		return false;
	}

	/**
	 * Escape single quotes with another single quote.
	 */
	protected static String escapeQuotes(String s) {
		StringBuffer sb = new StringBuffer();
		int length = s.length();
		char prevChar = ' ';
		char prevPrevChar = ' ';
		for (int i=0; i<length; i++) {
			char c = s.charAt(i);
			sb.append(c);
			if (c == '\'' && (prevChar != '\\' || (prevChar == '\\' && prevPrevChar == '\\'))) {
				sb.append("'");
			}
			prevPrevChar = prevChar;
			prevChar = c;
		}
		return sb.toString();
	}

	/*
	 * Get a description of stored procedures available in a catalog
	 *
	 * <p>Only procedure descriptions matching the schema and procedure
	 * name criteria are returned.	They are ordered by PROCEDURE_SCHEM
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
	 *	  <li> procedureResultUnknown - May return a result
	 *	<li> procedureNoResult - Does not return a result
	 *	<li> procedureReturnsResult - Returns a result
	 *	  </ul>
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
		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT NULL AS PROCEDURE_CAT, n.nspname AS PROCEDURE_SCHEM, p.proname AS PROCEDURE_NAME, NULL, NULL, NULL, d.description AS REMARKS, "+java.sql.DatabaseMetaData.procedureReturnsResult+" AS PROCEDURE_TYPE "+
				" FROM pg_catalog.pg_namespace n, pg_catalog.pg_proc p "+
				" LEFT JOIN pg_catalog.pg_description d ON (p.oid=d.objoid) "+
				" LEFT JOIN pg_catalog.pg_class c ON (d.classoid=c.oid AND c.relname='pg_proc') "+
				" LEFT JOIN pg_catalog.pg_namespace pn ON (c.relnamespace=pn.oid AND pn.nspname='pg_catalog') "+
				" WHERE p.pronamespace=n.oid ";
				if (schemaPattern != null && !"".equals(schemaPattern)) {
					sql += " AND n.nspname LIKE '"+escapeQuotes(schemaPattern)+"' ";
				}
				if (procedureNamePattern != null) {
					sql += " AND p.proname LIKE '"+escapeQuotes(procedureNamePattern)+"' ";
				}
				sql += " ORDER BY PROCEDURE_SCHEM, PROCEDURE_NAME ";
		} else if (connection.haveMinimumServerVersion("7.1")) {
			sql = "SELECT NULL AS PROCEDURE_CAT, NULL AS PROCEDURE_SCHEM, p.proname AS PROCEDURE_NAME, NULL, NULL, NULL, d.description AS REMARKS, "+java.sql.DatabaseMetaData.procedureReturnsResult+" AS PROCEDURE_TYPE "+
				" FROM pg_proc p "+
				" LEFT JOIN pg_description d ON (p.oid=d.objoid) ";
			if (connection.haveMinimumServerVersion("7.2")) {
				sql += " LEFT JOIN pg_class c ON (d.classoid=c.oid AND c.relname='pg_proc') ";
			}
			if (procedureNamePattern != null) {
				sql += " WHERE p.proname LIKE '"+escapeQuotes(procedureNamePattern)+"' ";
			}
			sql += " ORDER BY PROCEDURE_NAME ";
		} else {
			sql = "SELECT NULL AS PROCEDURE_CAT, NULL AS PROCEDURE_SCHEM, p.proname AS PROCEDURE_NAME, NULL, NULL, NULL, NULL AS REMARKS, "+java.sql.DatabaseMetaData.procedureReturnsResult+" AS PROCEDURE_TYPE "+
				" FROM pg_proc p ";
				if (procedureNamePattern != null) {
					sql += " WHERE p.proname LIKE '"+escapeQuotes(procedureNamePattern)+"' ";
				}
				sql += " ORDER BY PROCEDURE_NAME ";
		}
		return connection.createStatement().executeQuery(sql);
	}

	/*
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
	 * <li><b>TYPE_NAME</b> String => Data source specific type name
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
	 * @param schemaPattern
	 * @param procedureNamePattern a procedure name pattern
	 * @param columnNamePattern a column name pattern, this is currently ignored because postgresql does not name procedure parameters.
	 * @return each row is a stored procedure parameter or column description
	 * @exception SQLException if a database-access error occurs
	 * @see #getSearchStringEscape
	 */
	// Implementation note: This is required for Borland's JBuilder to work
	public java.sql.ResultSet getProcedureColumns(String catalog, String schemaPattern, String procedureNamePattern, String columnNamePattern) throws SQLException
	{
		Field f[] = new Field[13];
		Vector v = new Vector();		// The new ResultSet tuple stuff

		f[0] = new Field(connection, "PROCEDURE_CAT", iVarcharOid, getMaxNameLength());
		f[1] = new Field(connection, "PROCEDURE_SCHEM", iVarcharOid, getMaxNameLength());
		f[2] = new Field(connection, "PROCEDURE_NAME", iVarcharOid, getMaxNameLength());
		f[3] = new Field(connection, "COLUMN_NAME", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "COLUMN_TYPE", iInt2Oid, 2);
		f[5] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
		f[6] = new Field(connection, "TYPE_NAME", iVarcharOid, getMaxNameLength());
		f[7] = new Field(connection, "PRECISION", iInt4Oid, 4);
		f[8] = new Field(connection, "LENGTH", iInt4Oid, 4);
		f[9] = new Field(connection, "SCALE", iInt2Oid, 2);
		f[10] = new Field(connection, "RADIX", iInt2Oid, 2);
		f[11] = new Field(connection, "NULLABLE", iInt2Oid, 2);
		f[12] = new Field(connection, "REMARKS", iVarcharOid, getMaxNameLength());

		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT n.nspname,p.proname,p.prorettype,p.proargtypes, t.typtype,t.typrelid "+
				" FROM pg_catalog.pg_proc p,pg_catalog.pg_namespace n, pg_catalog.pg_type t "+
				" WHERE p.pronamespace=n.oid AND p.prorettype=t.oid ";
			if (schemaPattern != null && !"".equals(schemaPattern)) {
				sql += " AND n.nspname LIKE '"+escapeQuotes(schemaPattern)+"' ";
			}
			if (procedureNamePattern != null) {
				sql += " AND p.proname LIKE '"+escapeQuotes(procedureNamePattern)+"' ";
			}
			sql += " ORDER BY n.nspname, p.proname ";
		} else {
			sql = "SELECT NULL AS nspname,p.proname,p.prorettype,p.proargtypes, t.typtype,t.typrelid "+
				" FROM pg_proc p,pg_type t "+
				" WHERE p.prorettype=t.oid ";
			if (procedureNamePattern != null) {
				sql += " AND p.proname LIKE '"+escapeQuotes(procedureNamePattern)+"' ";
			}
			sql += " ORDER BY p.proname ";
		}

		ResultSet rs = connection.createStatement().executeQuery(sql);
		while (rs.next()) {
			byte schema[] = rs.getBytes("nspname");
			byte procedureName[] = rs.getBytes("proname");
			int returnType = rs.getInt("prorettype");
			String returnTypeType = rs.getString("typtype");
			int returnTypeRelid = rs.getInt("typrelid");
			String strArgTypes = rs.getString("proargtypes");
			StringTokenizer st = new StringTokenizer(strArgTypes);
			Vector argTypes = new Vector();
			while (st.hasMoreTokens()) {
				argTypes.addElement(new Integer(st.nextToken()));
			}

			// decide if we are returning a single column result.
			if (!returnTypeType.equals("c")) {
				byte[][] tuple = new byte[13][];
				tuple[0] = null;
				tuple[1] = schema;
				tuple[2] = procedureName;
				tuple[3] = encoding.encode("returnValue");
				tuple[4] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.procedureColumnReturn));
				tuple[5] = encoding.encode(Integer.toString(connection.getSQLType(returnType)));
				tuple[6] = encoding.encode(connection.getPGType(returnType));
				tuple[7] = null;
				tuple[8] = null;
				tuple[9] = null;
				tuple[10] = null;
				tuple[11] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.procedureNullableUnknown));
				tuple[12] = null;
				v.addElement(tuple);
			}

			// Add a row for each argument.
			for (int i=0; i<argTypes.size(); i++) {
				int argOid = ((Integer)argTypes.elementAt(i)).intValue();
				byte[][] tuple = new byte[13][];
				tuple[0] = null;
				tuple[1] = schema;
				tuple[2] = procedureName;
				tuple[3] = encoding.encode("$"+(i+1));
				tuple[4] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.procedureColumnIn));
				tuple[5] = encoding.encode(Integer.toString(connection.getSQLType(argOid)));
				tuple[6] = encoding.encode(connection.getPGType(argOid));
				tuple[7] = null;
				tuple[8] = null;
				tuple[9] = null;
				tuple[10] = null;
				tuple[11] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.procedureNullableUnknown));
				tuple[12] = null;
				v.addElement(tuple);
			}

			// if we are returning a multi-column result.
			if (returnTypeType.equals("c")) {
				String columnsql = "SELECT a.attname,a.atttypid FROM pg_catalog.pg_attribute a WHERE a.attrelid = "+returnTypeRelid+" ORDER BY a.attnum ";
				ResultSet columnrs = connection.createStatement().executeQuery(columnsql);
				while (columnrs.next()) {
					int columnTypeOid = columnrs.getInt("atttypid");
					byte[][] tuple = new byte[13][];
					tuple[0] = null;
					tuple[1] = schema;
					tuple[2] = procedureName;
					tuple[3] = columnrs.getBytes("attname");
					tuple[4] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.procedureColumnResult));
					tuple[5] = encoding.encode(Integer.toString(connection.getSQLType(columnTypeOid)));
					tuple[6] = encoding.encode(connection.getPGType(columnTypeOid));
					tuple[7] = null;
					tuple[8] = null;
					tuple[9] = null;
					tuple[10] = null;
					tuple[11] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.procedureNullableUnknown));
					tuple[12] = null;
					v.addElement(tuple);
				}
			}
		}
		rs.close();

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	/*
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
	 * "TABLE", "INDEX", "SEQUENCE", "VIEW",
	 * "SYSTEM TABLE", "SYSTEM INDEX", "SYSTEM VIEW",
	 * "SYSTEM TOAST TABLE", "SYSTEM TOAST INDEX",
	 * "TEMPORARY TABLE", and "TEMPORARY VIEW"
	 *
	 * @param catalog a catalog name; For org.postgresql, this is ignored, and
	 * should be set to null
	 * @param schemaPattern a schema name pattern
	 * @param tableNamePattern a table name pattern. For all tables this should be "%"
	 * @param types a list of table types to include; null returns
	 * all types
	 * @return each row is a table description
	 * @exception SQLException if a database-access error occurs.
	 */
	public java.sql.ResultSet getTables(String catalog, String schemaPattern, String tableNamePattern, String types[]) throws SQLException
	{
		String select;
		String orderby;
		String useSchemas;
		if (connection.haveMinimumServerVersion("7.3")) {
			useSchemas = "SCHEMAS";
			select = "SELECT NULL AS TABLE_CAT, n.nspname AS TABLE_SCHEM, c.relname AS TABLE_NAME, "+
			" CASE n.nspname LIKE 'pg\\\\_%' OR n.nspname = 'information_schema' "+
			" WHEN true THEN CASE "+
			"	WHEN n.nspname = 'pg_catalog' OR n.nspname = 'information_schema' THEN CASE c.relkind "+
			"		WHEN 'r' THEN 'SYSTEM TABLE' "+
			"		WHEN 'v' THEN 'SYSTEM VIEW' "+
			"		WHEN 'i' THEN 'SYSTEM INDEX' "+
			"		ELSE NULL "+
			"		END "+
			"	WHEN n.nspname = 'pg_toast' THEN CASE c.relkind "+
			"		WHEN 'r' THEN 'SYSTEM TOAST TABLE' "+
			"		WHEN 'i' THEN 'SYSTEM TOAST INDEX' "+
			"		ELSE NULL "+
			"		END "+
			"	ELSE CASE c.relkind "+
			"		WHEN 'r' THEN 'TEMPORARY TABLE' "+
			"		WHEN 'i' THEN 'TEMPORARY INDEX' "+
			"		ELSE NULL "+
			"		END "+
			"	END "+
			" WHEN false THEN CASE c.relkind "+
			"	WHEN 'r' THEN 'TABLE' "+
			"	WHEN 'i' THEN 'INDEX' "+
			"	WHEN 'S' THEN 'SEQUENCE' "+
			"	WHEN 'v' THEN 'VIEW' "+
			"	ELSE NULL "+
			"	END "+
			" ELSE NULL "+
			" END "+
			" AS TABLE_TYPE, d.description AS REMARKS "+
			" FROM pg_catalog.pg_namespace n, pg_catalog.pg_class c "+
			" LEFT JOIN pg_catalog.pg_description d ON (c.oid = d.objoid AND d.objsubid = 0) "+
			" LEFT JOIN pg_catalog.pg_class dc ON (d.classoid=dc.oid AND dc.relname='pg_class') "+
			" LEFT JOIN pg_catalog.pg_namespace dn ON (dn.oid=dc.relnamespace AND dn.nspname='pg_catalog') "+
			" WHERE c.relnamespace = n.oid ";
			if (schemaPattern != null && !"".equals(schemaPattern)) {
				select += " AND n.nspname LIKE '"+escapeQuotes(schemaPattern)+"' ";
			}
			orderby = " ORDER BY TABLE_TYPE,TABLE_SCHEM,TABLE_NAME ";
		} else {
			useSchemas = "NOSCHEMAS";
			String tableType =  ""+
			" CASE c.relname LIKE 'pg\\\\_%' "+
			" WHEN true THEN CASE c.relname LIKE 'pg\\\\_toast\\\\_%' "+
			"	WHEN true THEN CASE c.relkind "+
			"		WHEN 'r' THEN 'SYSTEM TOAST TABLE' "+
			"		WHEN 'i' THEN 'SYSTEM TOAST INDEX' "+
			"		ELSE NULL "+
			"		END "+
			"	WHEN false THEN CASE c.relname LIKE 'pg\\\\_temp\\\\_%' "+
			"		WHEN true THEN CASE c.relkind "+
			"			WHEN 'r' THEN 'TEMPORARY TABLE' "+
			"			WHEN 'i' THEN 'TEMPORARY INDEX' "+
			"			ELSE NULL "+
			"			END "+
			"		WHEN false THEN CASE c.relkind "+
			"			WHEN 'r' THEN 'SYSTEM TABLE' "+
			"			WHEN 'v' THEN 'SYSTEM VIEW' "+
			"			WHEN 'i' THEN 'SYSTEM INDEX' "+
			"			ELSE NULL "+
			"			END "+
			"		ELSE NULL "+
			"		END "+
			"	ELSE NULL "+
			"	END "+
			" WHEN false THEN CASE c.relkind "+
			"	WHEN 'r' THEN 'TABLE' "+
			"	WHEN 'i' THEN 'INDEX' "+
			"	WHEN 'S' THEN 'SEQUENCE' "+
			"	WHEN 'v' THEN 'VIEW' "+
			"	ELSE NULL "+
			"	END "+
			" ELSE NULL "+
			" END ";
			orderby = " ORDER BY TABLE_TYPE,TABLE_NAME ";
			if (connection.haveMinimumServerVersion("7.2")) {
				select = "SELECT NULL AS TABLE_CAT, NULL AS TABLE_SCHEM, c.relname AS TABLE_NAME, "+tableType+" AS TABLE_TYPE, d.description AS REMARKS "+
					" FROM pg_class c "+
					" LEFT JOIN pg_description d ON (c.oid=d.objoid AND d.objsubid = 0) "+
					" LEFT JOIN pg_class dc ON (d.classoid = dc.oid AND dc.relname='pg_class') "+
					" WHERE true ";
			} else if (connection.haveMinimumServerVersion("7.1")) {
				select = "SELECT NULL AS TABLE_CAT, NULL AS TABLE_SCHEM, c.relname AS TABLE_NAME, "+tableType+" AS TABLE_TYPE, d.description AS REMARKS "+
					" FROM pg_class c "+
					" LEFT JOIN pg_description d ON (c.oid=d.objoid) "+
					" WHERE true ";
			} else {
				select = "SELECT NULL AS TABLE_CAT, NULL AS TABLE_SCHEM, c.relname AS TABLE_NAME, "+tableType+" AS TABLE_TYPE, NULL AS REMARKS "+
					" FROM pg_class c "+
					" WHERE true ";
			}
		}

		if (types == null) {
			types = defaultTableTypes;
		}
		if (tableNamePattern != null) {
			select += " AND c.relname LIKE '"+escapeQuotes(tableNamePattern)+"' ";
		}
		String sql = select;
		sql += " AND (false ";
		for (int i=0; i<types.length; i++) {
			Hashtable clauses = (Hashtable)tableTypeClauses.get(types[i]);
			if (clauses != null) {
				String clause = (String)clauses.get(useSchemas);
				sql += " OR ( "+clause+" ) ";
			}
		}
		sql += ") ";
		sql += orderby;

		return connection.createStatement().executeQuery(sql);
	}

	private static final Hashtable tableTypeClauses;
	static {
		tableTypeClauses = new Hashtable();
		Hashtable ht = new Hashtable();
		tableTypeClauses.put("TABLE",ht);
		ht.put("SCHEMAS","c.relkind = 'r' AND n.nspname NOT LIKE 'pg\\\\_%' AND n.nspname <> 'information_schema'");
		ht.put("NOSCHEMAS","c.relkind = 'r' AND c.relname NOT LIKE 'pg\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("VIEW",ht);
		ht.put("SCHEMAS","c.relkind = 'v' AND n.nspname <> 'pg_catalog' AND n.nspname <> 'information_schema'");
		ht.put("NOSCHEMAS","c.relkind = 'v' AND c.relname NOT LIKE 'pg\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("INDEX",ht);
		ht.put("SCHEMAS","c.relkind = 'i' AND n.nspname NOT LIKE 'pg\\\\_%' AND n.nspname <> 'information_schema'");
		ht.put("NOSCHEMAS","c.relkind = 'i' AND c.relname NOT LIKE 'pg\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("SEQUENCE",ht);
		ht.put("SCHEMAS","c.relkind = 'S'");
		ht.put("NOSCHEMAS","c.relkind = 'S'");
		ht = new Hashtable();
		tableTypeClauses.put("SYSTEM TABLE",ht);
		ht.put("SCHEMAS","c.relkind = 'r' AND (n.nspname = 'pg_catalog' OR n.nspname = 'information_schema')");
		ht.put("NOSCHEMAS","c.relkind = 'r' AND c.relname LIKE 'pg\\\\_%' AND c.relname NOT LIKE 'pg\\\\_toast\\\\_%' AND c.relname NOT LIKE 'pg\\\\_temp\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("SYSTEM TOAST TABLE",ht);
		ht.put("SCHEMAS","c.relkind = 'r' AND n.nspname = 'pg_toast'");
		ht.put("NOSCHEMAS","c.relkind = 'r' AND c.relname LIKE 'pg\\\\_toast\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("SYSTEM TOAST INDEX",ht);
		ht.put("SCHEMAS","c.relkind = 'i' AND n.nspname = 'pg_toast'");
		ht.put("NOSCHEMAS","c.relkind = 'i' AND c.relname LIKE 'pg\\\\_toast\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("SYSTEM VIEW",ht);
		ht.put("SCHEMAS","c.relkind = 'v' AND (n.nspname = 'pg_catalog' OR n.nspname = 'information_schema') ");
		ht.put("NOSCHEMAS","c.relkind = 'v' AND c.relname LIKE 'pg\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("SYSTEM INDEX",ht);
		ht.put("SCHEMAS","c.relkind = 'i' AND (n.nspname = 'pg_catalog' OR n.nspname = 'information_schema') ");
		ht.put("NOSCHEMAS","c.relkind = 'v' AND c.relname LIKE 'pg\\\\_%' AND c.relname NOT LIKE 'pg\\\\_toast\\\\_%' AND c.relname NOT LIKE 'pg\\\\_temp\\\\_%'");
		ht = new Hashtable();
		tableTypeClauses.put("TEMPORARY TABLE",ht);
		ht.put("SCHEMAS","c.relkind = 'r' AND n.nspname LIKE 'pg\\\\_temp\\\\_%' ");
		ht.put("NOSCHEMAS","c.relkind = 'r' AND c.relname LIKE 'pg\\\\_temp\\\\_%' ");
		ht = new Hashtable();
		tableTypeClauses.put("TEMPORARY INDEX",ht);
		ht.put("SCHEMAS","c.relkind = 'i' AND n.nspname LIKE 'pg\\\\_temp\\\\_%' ");
		ht.put("NOSCHEMAS","c.relkind = 'i' AND c.relname LIKE 'pg\\\\_temp\\\\_%' ");
	}

	// These are the default tables, used when NULL is passed to getTables
	// The choice of these provide the same behaviour as psql's \d
	private static final String defaultTableTypes[] = {
				"TABLE", "VIEW", "INDEX", "SEQUENCE", "TEMPORARY TABLE"
			};

	/*
	 * Get the schema names available in this database.  The results
	 * are ordered by schema name.
	 *
	 * <P>The schema column is:
	 *	<OL>
	 *	<LI><B>TABLE_SCHEM</B> String => schema name
	 *	</OL>
	 *
	 * @return ResultSet each row has a single String column that is a
	 * schema name
	 */
	public java.sql.ResultSet getSchemas() throws SQLException
	{
		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT nspname AS TABLE_SCHEM FROM pg_catalog.pg_namespace WHERE nspname <> 'pg_toast' AND nspname NOT LIKE 'pg\\\\_temp\\\\_%' ORDER BY TABLE_SCHEM";
		} else {
			sql = "SELECT ''::text AS TABLE_SCHEM ORDER BY TABLE_SCHEM";
		}
		return connection.createStatement().executeQuery(sql);
	}

	/*
	 * Get the catalog names available in this database.  The results
	 * are ordered by catalog name.
	 *
	 * <P>The catalog column is:
	 *	<OL>
	 *	<LI><B>TABLE_CAT</B> String => catalog name
	 *	</OL>
	 *
	 * @return ResultSet each row has a single String column that is a
	 * catalog name
	 */
	public java.sql.ResultSet getCatalogs() throws SQLException
	{
		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT datname AS TABLE_CAT FROM pg_catalog.pg_database ORDER BY TABLE_CAT";
		} else {
			sql = "SELECT datname AS TABLE_CAT FROM pg_database ORDER BY TABLE_CAT";
		}
		return connection.createStatement().executeQuery(sql);
	}

	/*
	 * Get the table types available in this database.	The results
	 * are ordered by table type.
	 *
	 * <P>The table type is:
	 *	<OL>
	 *	<LI><B>TABLE_TYPE</B> String => table type.  Typical types are "TABLE",
	 *			"VIEW", "SYSTEM TABLE", "GLOBAL TEMPORARY",
	 *			"LOCAL TEMPORARY", "ALIAS", "SYNONYM".
	 *	</OL>
	 *
	 * @return ResultSet each row has a single String column that is a
	 * table type
	 */
	public java.sql.ResultSet getTableTypes() throws SQLException
	{
		String types[] = new String[tableTypeClauses.size()];
		Enumeration e = tableTypeClauses.keys();
		int i=0;
		while (e.hasMoreElements()) {
			types[i++] = (String)e.nextElement();}
		sortStringArray(types);

		Field f[] = new Field[1];
		Vector v = new Vector();
		f[0] = new Field(connection, new String("TABLE_TYPE"), iVarcharOid, getMaxNameLength());
		for (i=0; i < types.length; i++)
		{
			byte[][] tuple = new byte[1][];
			tuple[0] = encoding.encode(types[i]);
			v.addElement(tuple);
		}

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	/*
	 * Get a description of table columns available in a catalog.
	 *
	 * <P>Only column descriptions matching the catalog, schema, table
	 * and column name criteria are returned.  They are ordered by
	 * TABLE_SCHEM, TABLE_NAME and ORDINAL_POSITION.
	 *
	 * <P>Each column description has the following columns:
	 *	<OL>
	 *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
	 *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
	 *	<LI><B>TABLE_NAME</B> String => table name
	 *	<LI><B>COLUMN_NAME</B> String => column name
	 *	<LI><B>DATA_TYPE</B> short => SQL type from java.sql.Types
	 *	<LI><B>TYPE_NAME</B> String => Data source dependent type name
	 *	<LI><B>COLUMN_SIZE</B> int => column size.	For char or date
	 *		types this is the maximum number of characters, for numeric or
	 *		decimal types this is precision.
	 *	<LI><B>BUFFER_LENGTH</B> is not used.
	 *	<LI><B>DECIMAL_DIGITS</B> int => the number of fractional digits
	 *	<LI><B>NUM_PREC_RADIX</B> int => Radix (typically either 10 or 2)
	 *	<LI><B>NULLABLE</B> int => is NULL allowed?
	 *		<UL>
	 *		<LI> columnNoNulls - might not allow NULL values
	 *		<LI> columnNullable - definitely allows NULL values
	 *		<LI> columnNullableUnknown - nullability unknown
	 *		</UL>
	 *	<LI><B>REMARKS</B> String => comment describing column (may be null)
	 *	<LI><B>COLUMN_DEF</B> String => default value (may be null)
	 *	<LI><B>SQL_DATA_TYPE</B> int => unused
	 *	<LI><B>SQL_DATETIME_SUB</B> int => unused
	 *	<LI><B>CHAR_OCTET_LENGTH</B> int => for char types the
	 *		 maximum number of bytes in the column
	 *	<LI><B>ORDINAL_POSITION</B> int => index of column in table
	 *		(starting at 1)
	 *	<LI><B>IS_NULLABLE</B> String => "NO" means column definitely
	 *		does not allow NULL values; "YES" means the column might
	 *		allow NULL values.	An empty string means nobody knows.
	 *	</OL>
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
		Vector v = new Vector();		// The new ResultSet tuple stuff
		Field f[] = new Field[18];		// The field descriptors for the new ResultSet

		f[0] = new Field(connection, "TABLE_CAT", iVarcharOid, getMaxNameLength());
		f[1] = new Field(connection, "TABLE_SCHEM", iVarcharOid, getMaxNameLength());
		f[2] = new Field(connection, "TABLE_NAME", iVarcharOid, getMaxNameLength());
		f[3] = new Field(connection, "COLUMN_NAME", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
		f[5] = new Field(connection, "TYPE_NAME", iVarcharOid, getMaxNameLength());
		f[6] = new Field(connection, "COLUMN_SIZE", iInt4Oid, 4);
		f[7] = new Field(connection, "BUFFER_LENGTH", iVarcharOid, getMaxNameLength());
		f[8] = new Field(connection, "DECIMAL_DIGITS", iInt4Oid, 4);
		f[9] = new Field(connection, "NUM_PREC_RADIX", iInt4Oid, 4);
		f[10] = new Field(connection, "NULLABLE", iInt4Oid, 4);
		f[11] = new Field(connection, "REMARKS", iVarcharOid, getMaxNameLength());
		f[12] = new Field(connection, "COLUMN_DEF", iVarcharOid, getMaxNameLength());
		f[13] = new Field(connection, "SQL_DATA_TYPE", iInt4Oid, 4);
		f[14] = new Field(connection, "SQL_DATETIME_SUB", iInt4Oid, 4);
		f[15] = new Field(connection, "CHAR_OCTET_LENGTH", iVarcharOid, getMaxNameLength());
		f[16] = new Field(connection, "ORDINAL_POSITION", iInt4Oid, 4);
		f[17] = new Field(connection, "IS_NULLABLE", iVarcharOid, getMaxNameLength());

		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT n.nspname,c.relname,a.attname,a.atttypid,a.attnotnull,a.atttypmod,a.attlen,a.attnum,def.adsrc,dsc.description "+
				" FROM pg_catalog.pg_namespace n "+
				" JOIN pg_catalog.pg_class c ON (c.relnamespace = n.oid) "+
				" JOIN pg_catalog.pg_attribute a ON (a.attrelid=c.oid) "+
				" LEFT JOIN pg_catalog.pg_attrdef def ON (a.attrelid=def.adrelid AND a.attnum = def.adnum) "+
				" LEFT JOIN pg_catalog.pg_description dsc ON (c.oid=dsc.objoid AND a.attnum = dsc.objsubid) "+
				" LEFT JOIN pg_catalog.pg_class dc ON (dc.oid=dsc.classoid AND dc.relname='pg_class') "+
				" LEFT JOIN pg_catalog.pg_namespace dn ON (dc.relnamespace=dn.oid AND dn.nspname='pg_catalog') "+
				" WHERE a.attnum > 0 AND NOT a.attisdropped ";
			if (schemaPattern != null && !"".equals(schemaPattern)) {
				sql += " AND n.nspname LIKE '"+escapeQuotes(schemaPattern)+"' ";
			}
		} else if (connection.haveMinimumServerVersion("7.2")) {
			sql = "SELECT NULL::text AS nspname,c.relname,a.attname,a.atttypid,a.attnotnull,a.atttypmod,a.attlen,a.attnum,def.adsrc,dsc.description "+
				" FROM pg_class c "+
				" JOIN pg_attribute a ON (a.attrelid=c.oid) "+
				" LEFT JOIN pg_attrdef def ON (a.attrelid=def.adrelid AND a.attnum = def.adnum) "+
				" LEFT JOIN pg_description dsc ON (c.oid=dsc.objoid AND a.attnum = dsc.objsubid) "+
				" LEFT JOIN pg_class dc ON (dc.oid=dsc.classoid AND dc.relname='pg_class') "+
				" WHERE a.attnum > 0 ";
		} else if (connection.haveMinimumServerVersion("7.1")) {
			sql = "SELECT NULL::text AS nspname,c.relname,a.attname,a.atttypid,a.attnotnull,a.atttypmod,a.attlen,a.attnum,def.adsrc,dsc.description "+
				" FROM pg_class c "+
				" JOIN pg_attribute a ON (a.attrelid=c.oid) "+
				" LEFT JOIN pg_attrdef def ON (a.attrelid=def.adrelid AND a.attnum = def.adnum) "+
				" LEFT JOIN pg_description dsc ON (a.oid=dsc.objoid) "+
				" WHERE a.attnum > 0 ";
		} else {
			// if < 7.1 then don't get defaults or descriptions.
			sql = "SELECT NULL::text AS nspname,c.relname,a.attname,a.atttypid,a.attnotnull,a.atttypmod,a.attlen,a.attnum,NULL AS adsrc,NULL AS description "+
				" FROM pg_class c, pg_attribute a "+
				" WHERE a.attrelid=c.oid AND a.attnum > 0 ";
		}

		if (tableNamePattern != null && !"".equals(tableNamePattern)) {
			sql += " AND c.relname LIKE '"+escapeQuotes(tableNamePattern)+"' ";
		}
		if (columnNamePattern != null && !"".equals(columnNamePattern)) {
			sql += " AND a.attname LIKE '"+escapeQuotes(columnNamePattern)+"' ";
		}
		sql += " ORDER BY nspname,relname,attnum ";

		ResultSet rs = connection.createStatement().executeQuery(sql);
		while (rs.next())
		{
			byte[][] tuple = new byte[18][];
			int typeOid = rs.getInt("atttypid");

			tuple[0] = null;					// Catalog name, not supported
			tuple[1] = rs.getBytes("nspname");	// Schema
			tuple[2] = rs.getBytes("relname");	// Table name
			tuple[3] = rs.getBytes("attname");	// Column name
			tuple[4] = encoding.encode(Integer.toString(connection.getSQLType(typeOid)));
			String pgType = connection.getPGType(typeOid);
			tuple[5] = encoding.encode(pgType);	// Type name

			// by default no decimal_digits
			// if the type is numeric or decimal we will
			// overwrite later.
			tuple[8] = encoding.encode("0");

			if (pgType.equals("bpchar") || pgType.equals("varchar"))
			{
				int atttypmod = rs.getInt("atttypmod");
				tuple[6] = encoding.encode(Integer.toString(atttypmod != -1 ? atttypmod - VARHDRSZ : 0));
			}
			else if (pgType.equals("numeric") || pgType.equals("decimal")) 
			{
				int attypmod = rs.getInt("atttypmod") - VARHDRSZ;
				tuple[6] = encoding.encode(Integer.toString( ( attypmod >> 16 ) & 0xffff ));
				tuple[8] = encoding.encode(Integer.toString(attypmod & 0xffff));
				tuple[9] = encoding.encode("10");
			}
			else if (pgType.equals("bit") || pgType.equals("varbit")) {
				tuple[6] = rs.getBytes("atttypmod");
				tuple[9] = encoding.encode("2");
			}
			else {
				tuple[6] = rs.getBytes("attlen");
				tuple[9] = encoding.encode("10");
			}

			tuple[7] = null;						// Buffer length

			tuple[10] = encoding.encode(Integer.toString(rs.getBoolean("attnotnull") ? java.sql.DatabaseMetaData.columnNoNulls : java.sql.DatabaseMetaData.columnNullable));	// Nullable
			tuple[11] = rs.getBytes("description");				// Description (if any)
			tuple[12] = rs.getBytes("adsrc");				// Column default
			tuple[13] = null;						// sql data type (unused)
			tuple[14] = null;						// sql datetime sub (unused)
			tuple[15] = tuple[6];					// char octet length
			tuple[16] = rs.getBytes("attnum");		// ordinal position
			tuple[17] = encoding.encode(rs.getBoolean("attnotnull") ? "NO" : "YES");	// Is nullable

			v.addElement(tuple);
		}
		rs.close();

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	/*
	 * Get a description of the access rights for a table's columns.
	 *
	 * <P>Only privileges matching the column name criteria are
	 * returned.  They are ordered by COLUMN_NAME and PRIVILEGE.
	 *
	 * <P>Each privilige description has the following columns:
	 *	<OL>
	 *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
	 *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
	 *	<LI><B>TABLE_NAME</B> String => table name
	 *	<LI><B>COLUMN_NAME</B> String => column name
	 *	<LI><B>GRANTOR</B> => grantor of access (may be null)
	 *	<LI><B>GRANTEE</B> String => grantee of access
	 *	<LI><B>PRIVILEGE</B> String => name of access (SELECT,
	 *		INSERT, UPDATE, REFRENCES, ...)
	 *	<LI><B>IS_GRANTABLE</B> String => "YES" if grantee is permitted
	 *		to grant to others; "NO" if not; null if unknown
	 *	</OL>
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

		if (table == null)
			table = "%";

		if (columnNamePattern == null)
			columnNamePattern = "%";

		f[0] = new Field(connection, "TABLE_CAT", iVarcharOid, getMaxNameLength());
		f[1] = new Field(connection, "TABLE_SCHEM", iVarcharOid, getMaxNameLength());
		f[2] = new Field(connection, "TABLE_NAME", iVarcharOid, getMaxNameLength());
		f[3] = new Field(connection, "COLUMN_NAME", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "GRANTOR", iVarcharOid, getMaxNameLength());
		f[5] = new Field(connection, "GRANTEE", iVarcharOid, getMaxNameLength());
		f[6] = new Field(connection, "PRIVILEGE", iVarcharOid, getMaxNameLength());
		f[7] = new Field(connection, "IS_GRANTABLE", iVarcharOid, getMaxNameLength());

		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT n.nspname,c.relname,u.usename,c.relacl,a.attname "+
				" FROM pg_catalog.pg_namespace n, pg_catalog.pg_class c, pg_catalog.pg_user u, pg_catalog.pg_attribute a "+
				" WHERE c.relnamespace = n.oid "+
				" AND u.usesysid = c.relowner "+
				" AND c.oid = a.attrelid "+
				" AND c.relkind = 'r' "+
				" AND a.attnum > 0 AND NOT a.attisdropped ";
			if (schema != null && !"".equals(schema)) {
				sql += " AND n.nspname = '"+escapeQuotes(schema)+"' ";
			}
		} else {
			sql = "SELECT NULL::text AS nspname,c.relname,u.usename,c.relacl,a.attname "+
				"FROM pg_class c, pg_user u,pg_attribute a "+
				" WHERE u.usesysid = c.relowner "+
				" AND c.oid = a.attrelid "+
				" AND a.attnum > 0 "+
				" AND c.relkind = 'r' ";
		}

		sql += " AND c.relname = '"+escapeQuotes(table)+"' ";
		if (columnNamePattern != null && !"".equals(columnNamePattern)) {
			sql += " AND a.attname LIKE '"+escapeQuotes(columnNamePattern)+"' ";
		}
		sql += " ORDER BY attname ";

		ResultSet rs = connection.createStatement().executeQuery(sql);
		while (rs.next()) {
			byte schemaName[] = rs.getBytes("nspname");
			byte tableName[] = rs.getBytes("relname");
			byte column[] = rs.getBytes("attname");
			String owner = rs.getString("usename");
			String acl = rs.getString("relacl");
			Hashtable permissions = parseACL(acl, owner);
			String permNames[] = new String[permissions.size()];
			Enumeration e = permissions.keys();
			int i=0;
			while (e.hasMoreElements()) {
				permNames[i++] = (String)e.nextElement();
			}
			sortStringArray(permNames);
			for (i=0; i<permNames.length; i++) {
				byte[] privilege = encoding.encode(permNames[i]);
				Vector grantees = (Vector)permissions.get(permNames[i]);
				for (int j=0; j<grantees.size(); j++) {
					String grantee = (String)grantees.elementAt(j);
					String grantable = owner.equals(grantee) ? "YES" : "NO";
					byte[][] tuple = new byte[8][];
					tuple[0] = null;
					tuple[1] = schemaName;
					tuple[2] = tableName;
					tuple[3] = column;
					tuple[4] = encoding.encode(owner);
					tuple[5] = encoding.encode(grantee);
					tuple[6] = privilege;
					tuple[7] = encoding.encode(grantable);
					v.addElement(tuple);
				}
			}
		}
		rs.close();

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	/*
	* Get a description of the access rights for each table available
	* in a catalog.
	*
	* This method is currently unimplemented.
	*
	* <P>Only privileges matching the schema and table name
	* criteria are returned.  They are ordered by TABLE_SCHEM,
	* TABLE_NAME, and PRIVILEGE.
	*
	* <P>Each privilige description has the following columns:
	*	<OL>
	*	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
	*	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
	*	<LI><B>TABLE_NAME</B> String => table name
	*	<LI><B>GRANTOR</B> => grantor of access (may be null)
	*	<LI><B>GRANTEE</B> String => grantee of access
	*	<LI><B>PRIVILEGE</B> String => name of access (SELECT,
	*		INSERT, UPDATE, REFRENCES, ...)
	*	<LI><B>IS_GRANTABLE</B> String => "YES" if grantee is permitted
	*		to grant to others; "NO" if not; null if unknown
	*	</OL>
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
		Field f[] = new Field[7];
		Vector v = new Vector();

		f[0] = new Field(connection, "TABLE_CAT", iVarcharOid, getMaxNameLength());
		f[1] = new Field(connection, "TABLE_SCHEM", iVarcharOid, getMaxNameLength());
		f[2] = new Field(connection, "TABLE_NAME", iVarcharOid, getMaxNameLength());
		f[3] = new Field(connection, "GRANTOR", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "GRANTEE", iVarcharOid, getMaxNameLength());
		f[5] = new Field(connection, "PRIVILEGE", iVarcharOid, getMaxNameLength());
		f[6] = new Field(connection, "IS_GRANTABLE", iVarcharOid, getMaxNameLength());

		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT n.nspname,c.relname,u.usename,c.relacl "+
				" FROM pg_catalog.pg_namespace n, pg_catalog.pg_class c, pg_catalog.pg_user u "+
				" WHERE c.relnamespace = n.oid "+
				" AND u.usesysid = c.relowner "+
				" AND c.relkind = 'r' ";
			if (schemaPattern != null && !"".equals(schemaPattern)) {
				sql += " AND n.nspname LIKE '"+escapeQuotes(schemaPattern)+"' ";
			}
		} else {
			sql = "SELECT NULL::text AS nspname,c.relname,u.usename,c.relacl "+
				"FROM pg_class c, pg_user u "+
				" WHERE u.usesysid = c.relowner "+
				" AND c.relkind = 'r' ";
		}

		if (tableNamePattern != null && !"".equals(tableNamePattern)) {
			sql += " AND c.relname LIKE '"+escapeQuotes(tableNamePattern)+"' ";
		}
		sql += " ORDER BY nspname, relname ";

		ResultSet rs = connection.createStatement().executeQuery(sql);
		while (rs.next()) {
			byte schema[] = rs.getBytes("nspname");
			byte table[] = rs.getBytes("relname");
			String owner = rs.getString("usename");
			String acl = rs.getString("relacl");
			Hashtable permissions = parseACL(acl, owner);
			String permNames[] = new String[permissions.size()];
			Enumeration e = permissions.keys();
			int i=0;
			while (e.hasMoreElements()) {
				permNames[i++] = (String)e.nextElement();
			}
			sortStringArray(permNames);
			for (i=0; i<permNames.length; i++) {
				byte[] privilege = encoding.encode(permNames[i]);
				Vector grantees = (Vector)permissions.get(permNames[i]);
				for (int j=0; j<grantees.size(); j++) {
					String grantee = (String)grantees.elementAt(j);
					String grantable = owner.equals(grantee) ? "YES" : "NO";
					byte[][] tuple = new byte[7][];
					tuple[0] = null;
					tuple[1] = schema;
					tuple[2] = table;
					tuple[3] = encoding.encode(owner);
					tuple[4] = encoding.encode(grantee);
					tuple[5] = privilege;
					tuple[6] = encoding.encode(grantable);
					v.addElement(tuple);
				}
			}
		}
		rs.close();

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	private static void sortStringArray(String s[]) {
		for (int i=0; i<s.length-1; i++) {
			for (int j=i+1; j<s.length; j++) {
				if (s[i].compareTo(s[j]) > 0) {
					String tmp = s[i];
					s[i] = s[j];
					s[j] = tmp;
				}
			}
		}
	}

	/**
	 * Parse an String of ACLs into a Vector of ACLs.
	 */
	private static Vector parseACLArray(String aclString) {
		Vector acls = new Vector();
		if (aclString == null || aclString.length() == 0) {
			return acls;
		}
		boolean inQuotes = false;
		// start at 1 because of leading "{"
		int beginIndex = 1;
		char prevChar = ' ';
		for (int i=beginIndex; i<aclString.length(); i++) {
			
			char c = aclString.charAt(i);
			if (c == '"' && prevChar != '\\') {
				inQuotes = !inQuotes;
			} else if (c == ',' && !inQuotes) {
				acls.addElement(aclString.substring(beginIndex,i));
				beginIndex = i+1;
			}
			prevChar = c;
		}
		// add last element removing the trailing "}"
		acls.addElement(aclString.substring(beginIndex,aclString.length()-1));

		// Strip out enclosing quotes, if any.
		for (int i=0; i<acls.size(); i++) {
			String acl = (String)acls.elementAt(i);
			if (acl.startsWith("\"") && acl.endsWith("\"")) {
				acl = acl.substring(1,acl.length()-1);
				acls.setElementAt(acl,i);
			}
		}
		return acls;
	}

	/**
	 * Add the user described by the given acl to the Vectors of users
	 * with the privileges described by the acl.
	 */
	private void addACLPrivileges(String acl, Hashtable privileges) {
		int equalIndex = acl.lastIndexOf("=");
		String name = acl.substring(0,equalIndex);
		if (name.length() == 0) {
			name = "PUBLIC";
		}
		String privs = acl.substring(equalIndex+1);
		for (int i=0; i<privs.length(); i++) {
			char c = privs.charAt(i);
			String sqlpriv;
			switch (c) {
				case 'a': sqlpriv = "INSERT"; break;
				case 'r': sqlpriv = "SELECT"; break;
				case 'w': sqlpriv = "UPDATE"; break;
				case 'd': sqlpriv = "DELETE"; break;
				case 'R': sqlpriv = "RULE"; break;
				case 'x': sqlpriv = "REFERENCES"; break;
				case 't': sqlpriv = "TRIGGER"; break;
				// the folloowing can't be granted to a table, but
				// we'll keep them for completeness.
				case 'X': sqlpriv = "EXECUTE"; break;
				case 'U': sqlpriv = "USAGE"; break;
				case 'C': sqlpriv = "CREATE"; break;
				case 'T': sqlpriv = "CREATE TEMP"; break;
				default: sqlpriv = "UNKNOWN";
			}
			Vector usersWithPermission = (Vector)privileges.get(sqlpriv);
			if (usersWithPermission == null) {
				usersWithPermission = new Vector();
				privileges.put(sqlpriv,usersWithPermission);
			}
			usersWithPermission.addElement(name);
		}
	}

	/**
	 * Take the a String representing an array of ACLs and return
	 * a Hashtable mapping the SQL permission name to a Vector of
	 * usernames who have that permission.
	 */
	protected Hashtable parseACL(String aclArray, String owner) {
		if (aclArray == null || aclArray == "") {
			//null acl is a shortcut for owner having full privs
			aclArray = "{" + owner + "=arwdRxt}";
		}
		Vector acls = parseACLArray(aclArray);
		Hashtable privileges = new Hashtable();
		for (int i=0; i<acls.size(); i++) {
			String acl = (String)acls.elementAt(i);
			addACLPrivileges(acl,privileges);
		}
		return privileges;
	}

	/*
	 * Get a description of a table's optimal set of columns that
	 * uniquely identifies a row. They are ordered by SCOPE.
	 *
	 * <P>Each column description has the following columns:
	 *	<OL>
	 *	<LI><B>SCOPE</B> short => actual scope of result
	 *		<UL>
	 *		<LI> bestRowTemporary - very temporary, while using row
	 *		<LI> bestRowTransaction - valid for remainder of current transaction
	 *		<LI> bestRowSession - valid for remainder of current session
	 *		</UL>
	 *	<LI><B>COLUMN_NAME</B> String => column name
	 *	<LI><B>DATA_TYPE</B> short => SQL data type from java.sql.Types
	 *	<LI><B>TYPE_NAME</B> String => Data source dependent type name
	 *	<LI><B>COLUMN_SIZE</B> int => precision
	 *	<LI><B>BUFFER_LENGTH</B> int => not used
	 *	<LI><B>DECIMAL_DIGITS</B> short  => scale
	 *	<LI><B>PSEUDO_COLUMN</B> short => is this a pseudo column
	 *		like an Oracle ROWID
	 *		<UL>
	 *		<LI> bestRowUnknown - may or may not be pseudo column
	 *		<LI> bestRowNotPseudo - is NOT a pseudo column
	 *		<LI> bestRowPseudo - is a pseudo column
	 *		</UL>
	 *	</OL>
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
		Field f[] = new Field[8];
		Vector v = new Vector();		// The new ResultSet tuple stuff

		f[0] = new Field(connection, "SCOPE", iInt2Oid, 2);
		f[1] = new Field(connection, "COLUMN_NAME", iVarcharOid, getMaxNameLength());
		f[2] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
		f[3] = new Field(connection, "TYPE_NAME", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "COLUMN_SIZE", iInt4Oid, 4);
		f[5] = new Field(connection, "BUFFER_LENGTH", iInt4Oid, 4);
		f[6] = new Field(connection, "DECIMAL_DIGITS", iInt2Oid, 2);
		f[7] = new Field(connection, "PSEUDO_COLUMN", iInt2Oid, 2);

		/* At the moment this simply returns a table's primary key,
		 * if there is one.  I believe other unique indexes, ctid,
		 * and oid should also be considered. -KJ
		 */

		String from;
		String where = "";
		if (connection.haveMinimumServerVersion("7.3")) {
			from = " FROM pg_catalog.pg_namespace n, pg_catalog.pg_class ct, pg_catalog.pg_class ci, pg_catalog.pg_attribute a, pg_catalog.pg_index i ";
			where = " AND ct.relnamespace = n.oid ";
			if (schema != null && !"".equals(schema)) {
				where += " AND n.nspname = '"+escapeQuotes(schema)+"' ";
			}
		} else {
			from = " FROM pg_class ct, pg_class ci, pg_attribute a, pg_index i ";
		}
		String sql = "SELECT a.attname, a.atttypid "+
			from+
			" WHERE ct.oid=i.indrelid AND ci.oid=i.indexrelid "+
			" AND a.attrelid=ci.oid AND i.indisprimary "+
			" AND ct.relname = '"+escapeQuotes(table)+"' "+
			where+
			" ORDER BY a.attnum ";

		ResultSet rs = connection.createStatement().executeQuery(sql);
		while (rs.next()) {
			byte tuple[][] = new byte[8][];
			int columnTypeOid = rs.getInt("atttypid");
			tuple[0] = encoding.encode(Integer.toString(scope));
			tuple[1] = rs.getBytes("attname");
			tuple[2] = encoding.encode(Integer.toString(connection.getSQLType(columnTypeOid)));
			tuple[3] = encoding.encode(connection.getPGType(columnTypeOid));
			tuple[4] = null;
			tuple[5] = null;
			tuple[6] = null;
			tuple[7] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.bestRowNotPseudo));
			v.addElement(tuple);
		}

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	/*
	 * Get a description of a table's columns that are automatically
	 * updated when any value in a row is updated.	They are
	 * unordered.
	 *
	 * <P>Each column description has the following columns:
	 *	<OL>
	 *	<LI><B>SCOPE</B> short => is not used
	 *	<LI><B>COLUMN_NAME</B> String => column name
	 *	<LI><B>DATA_TYPE</B> short => SQL data type from java.sql.Types
	 *	<LI><B>TYPE_NAME</B> String => Data source dependent type name
	 *	<LI><B>COLUMN_SIZE</B> int => precision
	 *	<LI><B>BUFFER_LENGTH</B> int => length of column value in bytes
	 *	<LI><B>DECIMAL_DIGITS</B> short  => scale
	 *	<LI><B>PSEUDO_COLUMN</B> short => is this a pseudo column
	 *		like an Oracle ROWID
	 *		<UL>
	 *		<LI> versionColumnUnknown - may or may not be pseudo column
	 *		<LI> versionColumnNotPseudo - is NOT a pseudo column
	 *		<LI> versionColumnPseudo - is a pseudo column
	 *		</UL>
	 *	</OL>
	 *
	 * @param catalog a catalog name; "" retrieves those without a catalog
	 * @param schema a schema name; "" retrieves those without a schema
	 * @param table a table name
	 * @return ResultSet each row is a column description
	 */
	public java.sql.ResultSet getVersionColumns(String catalog, String schema, String table) throws SQLException
	{
		Field f[] = new Field[8];
		Vector v = new Vector();		// The new ResultSet tuple stuff

		f[0] = new Field(connection, "SCOPE", iInt2Oid, 2);
		f[1] = new Field(connection, "COLUMN_NAME", iVarcharOid, getMaxNameLength());
		f[2] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
		f[3] = new Field(connection, "TYPE_NAME", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "COLUMN_SIZE", iInt4Oid, 4);
		f[5] = new Field(connection, "BUFFER_LENGTH", iInt4Oid, 4);
		f[6] = new Field(connection, "DECIMAL_DIGITS", iInt2Oid, 2);
		f[7] = new Field(connection, "PSEUDO_COLUMN", iInt2Oid, 2);

		byte tuple[][] = new byte[8][];

		/* Postgresql does not have any column types that are
		 * automatically updated like some databases' timestamp type.
		 * We can't tell what rules or triggers might be doing, so we
		 * are left with the system columns that change on an update.
		 * An update may change all of the following system columns:
		 * ctid, xmax, xmin, cmax, and cmin.  Depending on if we are
		 * in a transaction and wether we roll it back or not the
		 * only guaranteed change is to ctid. -KJ
		 */

		tuple[0] = null;
		tuple[1] = encoding.encode("ctid");
		tuple[2] = encoding.encode(Integer.toString(connection.getSQLType("tid")));
		tuple[3] = encoding.encode("tid");
		tuple[4] = null;
		tuple[5] = null;
		tuple[6] = null;
		tuple[7] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.versionColumnPseudo));
		v.addElement(tuple);

		/* Perhaps we should check that the given
		 * catalog.schema.table actually exists. -KJ
		 */
		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	/*
	 * Get a description of a table's primary key columns.  They
	 * are ordered by COLUMN_NAME.
	 *
	 * <P>Each column description has the following columns:
	 *	<OL>
	 *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
	 *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
	 *	<LI><B>TABLE_NAME</B> String => table name
	 *	<LI><B>COLUMN_NAME</B> String => column name
	 *	<LI><B>KEY_SEQ</B> short => sequence number within primary key
	 *	<LI><B>PK_NAME</B> String => primary key name (may be null)
	 *	</OL>
	 *
	 * @param catalog a catalog name; "" retrieves those without a catalog
	 * @param schema a schema name pattern; "" retrieves those
	 * without a schema
	 * @param table a table name
	 * @return ResultSet each row is a primary key column description
	 */
	public java.sql.ResultSet getPrimaryKeys(String catalog, String schema, String table) throws SQLException
	{
		String select;
		String from;
		String where = "";
		if (connection.haveMinimumServerVersion("7.3")) {
			select = "SELECT NULL AS TABLE_CAT, n.nspname AS TABLE_SCHEM, ";
			from = " FROM pg_catalog.pg_namespace n, pg_catalog.pg_class ct, pg_catalog.pg_class ci, pg_catalog.pg_attribute a, pg_catalog.pg_index i ";
			where = " AND ct.relnamespace = n.oid ";
			if (schema != null && !"".equals(schema)) {
				where += " AND n.nspname = '"+escapeQuotes(schema)+"' ";
			}
		} else {
			select = "SELECT NULL AS TABLE_CAT, NULL AS TABLE_SCHEM, ";
			from = " FROM pg_class ct, pg_class ci, pg_attribute a, pg_index i ";
		}
		String sql = select+
			" ct.relname AS TABLE_NAME, "+
			" a.attname AS COLUMN_NAME, "+
			" a.attnum AS KEY_SEQ, "+
			" ci.relname AS PK_NAME "+
			from+
			" WHERE ct.oid=i.indrelid AND ci.oid=i.indexrelid "+
			" AND a.attrelid=ci.oid AND i.indisprimary ";
                        if (table != null && !"".equals(table)) {
			        sql += " AND ct.relname = '"+escapeQuotes(table)+"' ";
                        }
			sql += where+
			" ORDER BY table_name, pk_name, key_seq";
		return connection.createStatement().executeQuery(sql);
	}

	/**
	 *
	 * @param catalog
	 * @param schema
	 * @param primaryTable if provided will get the keys exported by this table
	 * @param foreignTable if provided will get the keys imported by this table
	 * @return ResultSet
	 * @throws SQLException
	 */

	protected java.sql.ResultSet getImportedExportedKeys(String primaryCatalog, String primarySchema, String primaryTable, String foreignCatalog, String foreignSchema, String foreignTable) throws SQLException
	{
		Field f[] = new Field[14];

		f[0] = new Field(connection, "PKTABLE_CAT", iVarcharOid, getMaxNameLength());
		f[1] = new Field(connection, "PKTABLE_SCHEM", iVarcharOid, getMaxNameLength());
		f[2] = new Field(connection, "PKTABLE_NAME", iVarcharOid, getMaxNameLength());
		f[3] = new Field(connection, "PKCOLUMN_NAME", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "FKTABLE_CAT", iVarcharOid, getMaxNameLength());
		f[5] = new Field(connection, "FKTABLE_SCHEM", iVarcharOid, getMaxNameLength());
		f[6] = new Field(connection, "FKTABLE_NAME", iVarcharOid, getMaxNameLength());
		f[7] = new Field(connection, "FKCOLUMN_NAME", iVarcharOid, getMaxNameLength());
		f[8] = new Field(connection, "KEY_SEQ", iInt2Oid, 2);
		f[9] = new Field(connection, "UPDATE_RULE", iInt2Oid, 2);
		f[10] = new Field(connection, "DELETE_RULE", iInt2Oid, 2);
		f[11] = new Field(connection, "FK_NAME", iVarcharOid, getMaxNameLength());
		f[12] = new Field(connection, "PK_NAME", iVarcharOid, getMaxNameLength());
		f[13] = new Field(connection, "DEFERRABILITY", iInt2Oid, 2);


		String select;
		String from;
		String where = "";

		/*
		 * The addition of the pg_constraint in 7.3 table should have really
		 * helped us out here, but it comes up just a bit short.
		 * - The conkey, confkey columns aren't really useful without
		 *   contrib/array unless we want to issues separate queries.
		 * - Unique indexes that can support foreign keys are not necessarily
		 *   added to pg_constraint.  Also multiple unique indexes covering
		 *   the same keys can be created which make it difficult to determine
		 *   the PK_NAME field.
		 */

		if (connection.haveMinimumServerVersion("7.4")) {
			String sql = "SELECT NULL::text AS PKTABLE_CAT, pkn.nspname AS PKTABLE_SCHEM, pkc.relname AS PKTABLE_NAME, pka.attname AS PKCOLUMN_NAME, "+
			"NULL::text AS FKTABLE_CAT, fkn.nspname AS FKTABLE_SCHEM, fkc.relname AS FKTABLE_NAME, fka.attname AS FKCOLUMN_NAME, "+
			"pos.n AS KEY_SEQ, "+
			"CASE con.confupdtype "+
				" WHEN 'c' THEN " + DatabaseMetaData.importedKeyCascade +
				" WHEN 'n' THEN " + DatabaseMetaData.importedKeySetNull +
				" WHEN 'd' THEN " + DatabaseMetaData.importedKeySetDefault +
				" WHEN 'r' THEN " + DatabaseMetaData.importedKeyRestrict +
				" WHEN 'a' THEN " + DatabaseMetaData.importedKeyNoAction +
				" ELSE NULL END AS UPDATE_RULE, "+
			"CASE con.confdeltype "+
				" WHEN 'c' THEN " + DatabaseMetaData.importedKeyCascade +
				" WHEN 'n' THEN " + DatabaseMetaData.importedKeySetNull +
				" WHEN 'd' THEN " + DatabaseMetaData.importedKeySetDefault +
				" WHEN 'r' THEN " + DatabaseMetaData.importedKeyRestrict +
				" WHEN 'a' THEN " + DatabaseMetaData.importedKeyNoAction +
				" ELSE NULL END AS DELETE_RULE, "+
			"con.conname AS FK_NAME, pkic.relname AS PK_NAME, "+
			"CASE "+
				" WHEN con.condeferrable AND con.condeferred THEN " + DatabaseMetaData.importedKeyInitiallyDeferred +
				" WHEN con.condeferrable THEN " + DatabaseMetaData.importedKeyInitiallyImmediate +
				" ELSE " + DatabaseMetaData.importedKeyNotDeferrable+
				" END AS DEFERRABILITY "+
			" FROM "+
			" pg_catalog.pg_namespace pkn, pg_catalog.pg_class pkc, pg_catalog.pg_attribute pka, "+
			" pg_catalog.pg_namespace fkn, pg_catalog.pg_class fkc, pg_catalog.pg_attribute fka, "+
			" pg_catalog.pg_constraint con, information_schema._pg_keypositions() pos(n), "+
			" pg_catalog.pg_depend dep, pg_catalog.pg_class pkic "+
			" WHERE pkn.oid = pkc.relnamespace AND pkc.oid = pka.attrelid AND pka.attnum = con.confkey[pos.n] AND con.confrelid = pkc.oid "+
			" AND fkn.oid = fkc.relnamespace AND fkc.oid = fka.attrelid AND fka.attnum = con.conkey[pos.n] AND con.conrelid = fkc.oid "+
			" AND con.contype = 'f' AND con.oid = dep.objid AND pkic.oid = dep.refobjid AND pkic.relkind = 'i' AND dep.classid = 'pg_constraint'::regclass::oid AND dep.refclassid = 'pg_class'::regclass::oid ";
			if (primarySchema != null && !"".equals(primarySchema)) {
				sql += " AND pkn.nspname = '"+escapeQuotes(primarySchema)+"' ";
			}
			if (foreignSchema != null && !"".equals(foreignSchema)) {
				sql += " AND fkn.nspname = '"+escapeQuotes(foreignSchema)+"' ";
			}
			if (primaryTable != null && !"".equals(primaryTable)) {
				sql += " AND pkc.relname = '"+escapeQuotes(primaryTable)+"' ";
			}
			if (foreignTable != null && !"".equals(foreignTable)) {
				sql += " AND fkc.relname = '"+escapeQuotes(foreignTable)+"' ";
			}
						
			if (primaryTable != null) {
				sql += " ORDER BY fkn.nspname,fkc.relname,pos.n";
			} else {
				sql += " ORDER BY pkn.nspname,pkc.relname,pos.n";
			}

			return connection.createStatement().executeQuery(sql);
		} else if (connection.haveMinimumServerVersion("7.3")) {
			select = "SELECT DISTINCT n1.nspname as pnspname,n2.nspname as fnspname, ";
			from = " FROM pg_catalog.pg_namespace n1 "+
				" JOIN pg_catalog.pg_class c1 ON (c1.relnamespace = n1.oid) "+
				" JOIN pg_catalog.pg_index i ON (c1.oid=i.indrelid) "+
				" JOIN pg_catalog.pg_class ic ON (i.indexrelid=ic.oid) "+
				" JOIN pg_catalog.pg_attribute a ON (ic.oid=a.attrelid), "+
				" pg_catalog.pg_namespace n2 "+
				" JOIN pg_catalog.pg_class c2 ON (c2.relnamespace=n2.oid), "+
				" pg_catalog.pg_trigger t1 "+
				" JOIN pg_catalog.pg_proc p1 ON (t1.tgfoid=p1.oid), "+
				" pg_catalog.pg_trigger t2 "+
				" JOIN pg_catalog.pg_proc p2 ON (t2.tgfoid=p2.oid) ";
			if (primarySchema != null && !"".equals(primarySchema)) {
				where += " AND n1.nspname = '"+escapeQuotes(primarySchema)+"' ";
			}
			if (foreignSchema != null && !"".equals(foreignSchema)) {
				where += " AND n2.nspname = '"+escapeQuotes(foreignSchema)+"' ";
			}
		} else {
			select = "SELECT DISTINCT NULL::text as pnspname, NULL::text as fnspname, ";
			from = " FROM pg_class c1 "+
				" JOIN pg_index i ON (c1.oid=i.indrelid) "+
				" JOIN pg_class ic ON (i.indexrelid=ic.oid) "+
				" JOIN pg_attribute a ON (ic.oid=a.attrelid), "+
				" pg_class c2, "+
				" pg_trigger t1 "+
				" JOIN pg_proc p1 ON (t1.tgfoid=p1.oid), "+
				" pg_trigger t2 "+
				" JOIN pg_proc p2 ON (t2.tgfoid=p2.oid) ";
		}

		String sql = select
			+ "c1.relname as prelname, "
			+ "c2.relname as frelname, "
			+ "t1.tgconstrname, "
			+ "a.attnum as keyseq, "
			+ "ic.relname as fkeyname, "
			+ "t1.tgdeferrable, "
			+ "t1.tginitdeferred, "
			+ "t1.tgnargs,t1.tgargs, "
			+ "p1.proname as updaterule, "
			+ "p2.proname as deleterule "
			+ from 
			+ "WHERE "
			// isolate the update rule
			+ "(t1.tgrelid=c1.oid "
			+ "AND t1.tgisconstraint "
			+ "AND t1.tgconstrrelid=c2.oid "
			+ "AND p1.proname LIKE 'RI\\\\_FKey\\\\_%\\\\_upd') "

			+ "AND "
			// isolate the delete rule
			+ "(t2.tgrelid=c1.oid "
			+ "AND t2.tgisconstraint "
			+ "AND t2.tgconstrrelid=c2.oid "
			+ "AND p2.proname LIKE 'RI\\\\_FKey\\\\_%\\\\_del') "

			+ "AND i.indisprimary "
			+ where;

		if (primaryTable != null) {
			sql += "AND c1.relname='" + escapeQuotes(primaryTable) + "' ";
		}
		if (foreignTable != null) {
			sql += "AND c2.relname='" + escapeQuotes(foreignTable) + "' ";
		}

		sql += "ORDER BY ";

		// orderby is as follows getExported, orders by FKTABLE,
		// getImported orders by PKTABLE
		// getCrossReference orders by FKTABLE, so this should work for both,
		// since when getting crossreference, primaryTable will be defined

		if (primaryTable != null) {
			if (connection.haveMinimumServerVersion("7.3")) {
				sql += "fnspname,";
			}
			sql += "frelname";
		} else {
			if (connection.haveMinimumServerVersion("7.3")) {
				sql += "pnspname,";
			}
			sql += "prelname";
		}

		sql += ",keyseq";

		ResultSet rs = connection.createStatement().executeQuery(sql);

		// returns the following columns
		// and some example data with a table defined as follows

		// create table people ( id int primary key);
		// create table policy ( id int primary key);
		// create table users  ( id int primary key, people_id int references people(id), policy_id int references policy(id))

		// prelname | frelname | tgconstrname | keyseq | fkeyName	 | tgdeferrable | tginitdeferred
		//	  1		|	 2	   |	  3		  |    4   |	 5		 |		 6		|	 7

		//	people	| users    | <unnamed>	  |    1   | people_pkey |		 f		|	 f

		// | tgnargs |						  tgargs									  | updaterule			 | deleterule
		// |	8	 |							9										  |    10				 |	  11
		// |	6	 | <unnamed>\000users\000people\000UNSPECIFIED\000people_id\000id\000 | RI_FKey_noaction_upd | RI_FKey_noaction_del

		Vector tuples = new Vector();

		while ( rs.next() )
		{
			byte tuple[][] = new byte[14][];

			tuple[1] = rs.getBytes(1); //PKTABLE_SCHEM
			tuple[5] = rs.getBytes(2); //FKTABLE_SCHEM
			tuple[2] = rs.getBytes(3); //PKTABLE_NAME
			tuple[6] = rs.getBytes(4); //FKTABLE_NAME
			String fKeyName = rs.getString(5);
			String updateRule = rs.getString(12);

			if (updateRule != null )
			{
				// Rules look like this RI_FKey_noaction_del so we want to pull out the part between the 'Key_' and the last '_' s

				String rule = updateRule.substring(8, updateRule.length() - 4);

				int action = java.sql.DatabaseMetaData.importedKeyNoAction;

				if ( rule == null || "noaction".equals(rule) )
					action = java.sql.DatabaseMetaData.importedKeyNoAction;
				if ("cascade".equals(rule))
					action = java.sql.DatabaseMetaData.importedKeyCascade;
				else if ("setnull".equals(rule))
					action = java.sql.DatabaseMetaData.importedKeySetNull;
				else if ("setdefault".equals(rule))
					action = java.sql.DatabaseMetaData.importedKeySetDefault;
				else if ("restrict".equals(rule))
					action = java.sql.DatabaseMetaData.importedKeyRestrict;

				tuple[9] = encoding.encode(Integer.toString(action));

			}

			String deleteRule = rs.getString(13);

			if ( deleteRule != null )
			{

				String rule = updateRule.substring(8, updateRule.length() - 4);

				int action = java.sql.DatabaseMetaData.importedKeyNoAction;
				if ("cascade".equals(rule))
					action = java.sql.DatabaseMetaData.importedKeyCascade;
				else if ("setnull".equals(rule))
					action = java.sql.DatabaseMetaData.importedKeySetNull;
				else if ("setdefault".equals(rule))
					action = java.sql.DatabaseMetaData.importedKeySetDefault;
				tuple[10] =  encoding.encode(Integer.toString(action)); 
			}


			int keySequence = rs.getInt(6); //KEY_SEQ

			// Parse the tgargs data
			String fkeyColumn = "";
			String pkeyColumn = "";
			String fkName = "";
			// Note, I am guessing at most of this, but it should be close
			// if not, please correct
			// the keys are in pairs and start after the first four arguments
			// the arguments are seperated by \000

			String targs = rs.getString(11);

			// args look like this
			//<unnamed>\000ww\000vv\000UNSPECIFIED\000m\000a\000n\000b\000
			// we are primarily interested in the column names which are the last items in the string

			Vector tokens = tokenize(targs, "\\000");
			if (tokens.size() > 0) {
				fkName = (String)tokens.elementAt(0);
			}

			if (fkName.startsWith("<unnamed>")) {
				fkName = targs;
			}

			int element = 4 + (keySequence - 1) * 2;
			if (tokens.size() > element) {
				fkeyColumn = (String)tokens.elementAt(element);
			}

			element++;
			if (tokens.size() > element) {
				pkeyColumn = (String)tokens.elementAt(element);
			}

			tuple[3] = encoding.encode(pkeyColumn); //PKCOLUMN_NAME
			tuple[7] = encoding.encode(fkeyColumn); //FKCOLUMN_NAME

			tuple[8] = rs.getBytes(6); //KEY_SEQ
			tuple[11] = encoding.encode(fkName); //FK_NAME this will give us a unique name for the foreign key
			tuple[12] = rs.getBytes(7); //PK_NAME

			// DEFERRABILITY
			int deferrability = java.sql.DatabaseMetaData.importedKeyNotDeferrable;
			boolean deferrable = rs.getBoolean(8);
			boolean initiallyDeferred = rs.getBoolean(9);
			if (deferrable)
			{
				if (initiallyDeferred)
					deferrability = java.sql.DatabaseMetaData.importedKeyInitiallyDeferred;
				else
					deferrability = java.sql.DatabaseMetaData.importedKeyInitiallyImmediate;
			}
			tuple[13] = encoding.encode(Integer.toString(deferrability));

			tuples.addElement(tuple);
		}

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, tuples, "OK", 1, 0);
	}

	/*
	 * Get a description of the primary key columns that are
	 * referenced by a table's foreign key columns (the primary keys
	 * imported by a table).  They are ordered by PKTABLE_CAT,
	 * PKTABLE_SCHEM, PKTABLE_NAME, and KEY_SEQ.
	 *
	 * <P>Each primary key column description has the following columns:
	 *	<OL>
	 *	<LI><B>PKTABLE_CAT</B> String => primary key table catalog
	 *		being imported (may be null)
	 *	<LI><B>PKTABLE_SCHEM</B> String => primary key table schema
	 *		being imported (may be null)
	 *	<LI><B>PKTABLE_NAME</B> String => primary key table name
	 *		being imported
	 *	<LI><B>PKCOLUMN_NAME</B> String => primary key column name
	 *		being imported
	 *	<LI><B>FKTABLE_CAT</B> String => foreign key table catalog (may be null)
	 *	<LI><B>FKTABLE_SCHEM</B> String => foreign key table schema (may be null)
	 *	<LI><B>FKTABLE_NAME</B> String => foreign key table name
	 *	<LI><B>FKCOLUMN_NAME</B> String => foreign key column name
	 *	<LI><B>KEY_SEQ</B> short => sequence number within foreign key
	 *	<LI><B>UPDATE_RULE</B> short => What happens to
	 *		 foreign key when primary is updated:
	 *		<UL>
	 *		<LI> importedKeyCascade - change imported key to agree
	 *				 with primary key update
	 *		<LI> importedKeyRestrict - do not allow update of primary
	 *				 key if it has been imported
	 *		<LI> importedKeySetNull - change imported key to NULL if
	 *				 its primary key has been updated
	 *		</UL>
	 *	<LI><B>DELETE_RULE</B> short => What happens to
	 *		the foreign key when primary is deleted.
	 *		<UL>
	 *		<LI> importedKeyCascade - delete rows that import a deleted key
	 *		<LI> importedKeyRestrict - do not allow delete of primary
	 *				 key if it has been imported
	 *		<LI> importedKeySetNull - change imported key to NULL if
	 *				 its primary key has been deleted
	 *		</UL>
	 *	<LI><B>FK_NAME</B> String => foreign key name (may be null)
	 *	<LI><B>PK_NAME</B> String => primary key name (may be null)
	 *	</OL>
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
		return getImportedExportedKeys(null,null,null,catalog, schema, table);
	}

	/*
	 * Get a description of a foreign key columns that reference a
	 * table's primary key columns (the foreign keys exported by a
	 * table).	They are ordered by FKTABLE_CAT, FKTABLE_SCHEM,
	 * FKTABLE_NAME, and KEY_SEQ.
	 *
	 * This method is currently unimplemented.
	 *
	 * <P>Each foreign key column description has the following columns:
	 *	<OL>
	 *	<LI><B>PKTABLE_CAT</B> String => primary key table catalog (may be null)
	 *	<LI><B>PKTABLE_SCHEM</B> String => primary key table schema (may be null)
	 *	<LI><B>PKTABLE_NAME</B> String => primary key table name
	 *	<LI><B>PKCOLUMN_NAME</B> String => primary key column name
	 *	<LI><B>FKTABLE_CAT</B> String => foreign key table catalog (may be null)
	 *		being exported (may be null)
	 *	<LI><B>FKTABLE_SCHEM</B> String => foreign key table schema (may be null)
	 *		being exported (may be null)
	 *	<LI><B>FKTABLE_NAME</B> String => foreign key table name
	 *		being exported
	 *	<LI><B>FKCOLUMN_NAME</B> String => foreign key column name
	 *		being exported
	 *	<LI><B>KEY_SEQ</B> short => sequence number within foreign key
	 *	<LI><B>UPDATE_RULE</B> short => What happens to
	 *		 foreign key when primary is updated:
	 *		<UL>
	 *		<LI> importedKeyCascade - change imported key to agree
	 *				 with primary key update
	 *		<LI> importedKeyRestrict - do not allow update of primary
	 *				 key if it has been imported
	 *		<LI> importedKeySetNull - change imported key to NULL if
	 *				 its primary key has been updated
	 *		</UL>
	 *	<LI><B>DELETE_RULE</B> short => What happens to
	 *		the foreign key when primary is deleted.
	 *		<UL>
	 *		<LI> importedKeyCascade - delete rows that import a deleted key
	 *		<LI> importedKeyRestrict - do not allow delete of primary
	 *				 key if it has been imported
	 *		<LI> importedKeySetNull - change imported key to NULL if
	 *				 its primary key has been deleted
	 *		</UL>
	 *	<LI><B>FK_NAME</B> String => foreign key identifier (may be null)
	 *	<LI><B>PK_NAME</B> String => primary key identifier (may be null)
	 *	</OL>
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
		return getImportedExportedKeys(catalog, schema, table, null,null,null);
	}

	/*
	 * Get a description of the foreign key columns in the foreign key
	 * table that reference the primary key columns of the primary key
	 * table (describe how one table imports another's key.) This
	 * should normally return a single foreign key/primary key pair
	 * (most tables only import a foreign key from a table once.)  They
	 * are ordered by FKTABLE_CAT, FKTABLE_SCHEM, FKTABLE_NAME, and
	 * KEY_SEQ.
	 *
	 * This method is currently unimplemented.
	 *
	 * <P>Each foreign key column description has the following columns:
	 *	<OL>
	 *	<LI><B>PKTABLE_CAT</B> String => primary key table catalog (may be null)
	 *	<LI><B>PKTABLE_SCHEM</B> String => primary key table schema (may be null)
	 *	<LI><B>PKTABLE_NAME</B> String => primary key table name
	 *	<LI><B>PKCOLUMN_NAME</B> String => primary key column name
	 *	<LI><B>FKTABLE_CAT</B> String => foreign key table catalog (may be null)
	 *		being exported (may be null)
	 *	<LI><B>FKTABLE_SCHEM</B> String => foreign key table schema (may be null)
	 *		being exported (may be null)
	 *	<LI><B>FKTABLE_NAME</B> String => foreign key table name
	 *		being exported
	 *	<LI><B>FKCOLUMN_NAME</B> String => foreign key column name
	 *		being exported
	 *	<LI><B>KEY_SEQ</B> short => sequence number within foreign key
	 *	<LI><B>UPDATE_RULE</B> short => What happens to
	 *		 foreign key when primary is updated:
	 *		<UL>
	 *		<LI> importedKeyCascade - change imported key to agree
	 *				 with primary key update
	 *		<LI> importedKeyRestrict - do not allow update of primary
	 *				 key if it has been imported
	 *		<LI> importedKeySetNull - change imported key to NULL if
	 *				 its primary key has been updated
	 *		</UL>
	 *	<LI><B>DELETE_RULE</B> short => What happens to
	 *		the foreign key when primary is deleted.
	 *		<UL>
	 *		<LI> importedKeyCascade - delete rows that import a deleted key
	 *		<LI> importedKeyRestrict - do not allow delete of primary
	 *				 key if it has been imported
	 *		<LI> importedKeySetNull - change imported key to NULL if
	 *				 its primary key has been deleted
	 *		</UL>
	 *	<LI><B>FK_NAME</B> String => foreign key identifier (may be null)
	 *	<LI><B>PK_NAME</B> String => primary key identifier (may be null)
	 *	</OL>
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
		return getImportedExportedKeys(primaryCatalog, primarySchema, primaryTable, foreignCatalog, foreignSchema, foreignTable);
	}

	/*
	 * Get a description of all the standard SQL types supported by
	 * this database. They are ordered by DATA_TYPE and then by how
	 * closely the data type maps to the corresponding JDBC SQL type.
	 *
	 * <P>Each type description has the following columns:
	 *	<OL>
	 *	<LI><B>TYPE_NAME</B> String => Type name
	 *	<LI><B>DATA_TYPE</B> short => SQL data type from java.sql.Types
	 *	<LI><B>PRECISION</B> int => maximum precision
	 *	<LI><B>LITERAL_PREFIX</B> String => prefix used to quote a literal
	 *		(may be null)
	 *	<LI><B>LITERAL_SUFFIX</B> String => suffix used to quote a literal
	 (may be null)
	 *	<LI><B>CREATE_PARAMS</B> String => parameters used in creating
	 *		the type (may be null)
	 *	<LI><B>NULLABLE</B> short => can you use NULL for this type?
	 *		<UL>
	 *		<LI> typeNoNulls - does not allow NULL values
	 *		<LI> typeNullable - allows NULL values
	 *		<LI> typeNullableUnknown - nullability unknown
	 *		</UL>
	 *	<LI><B>CASE_SENSITIVE</B> boolean=> is it case sensitive?
	 *	<LI><B>SEARCHABLE</B> short => can you use "WHERE" based on this type:
	 *		<UL>
	 *		<LI> typePredNone - No support
	 *		<LI> typePredChar - Only supported with WHERE .. LIKE
	 *		<LI> typePredBasic - Supported except for WHERE .. LIKE
	 *		<LI> typeSearchable - Supported for all WHERE ..
	 *		</UL>
	 *	<LI><B>UNSIGNED_ATTRIBUTE</B> boolean => is it unsigned?
	 *	<LI><B>FIXED_PREC_SCALE</B> boolean => can it be a money value?
	 *	<LI><B>AUTO_INCREMENT</B> boolean => can it be used for an
	 *		auto-increment value?
	 *	<LI><B>LOCAL_TYPE_NAME</B> String => localized version of type name
	 *		(may be null)
	 *	<LI><B>MINIMUM_SCALE</B> short => minimum scale supported
	 *	<LI><B>MAXIMUM_SCALE</B> short => maximum scale supported
	 *	<LI><B>SQL_DATA_TYPE</B> int => unused
	 *	<LI><B>SQL_DATETIME_SUB</B> int => unused
	 *	<LI><B>NUM_PREC_RADIX</B> int => usually 2 or 10
	 *	</OL>
	 *
	 * @return ResultSet each row is a SQL type description
	 */
	public java.sql.ResultSet getTypeInfo() throws SQLException
	{

		Field f[] = new Field[18];
		Vector v = new Vector();		// The new ResultSet tuple stuff

		f[0] = new Field(connection, "TYPE_NAME", iVarcharOid, getMaxNameLength());
		f[1] = new Field(connection, "DATA_TYPE", iInt2Oid, 2);
		f[2] = new Field(connection, "PRECISION", iInt4Oid, 4);
		f[3] = new Field(connection, "LITERAL_PREFIX", iVarcharOid, getMaxNameLength());
		f[4] = new Field(connection, "LITERAL_SUFFIX", iVarcharOid, getMaxNameLength());
		f[5] = new Field(connection, "CREATE_PARAMS", iVarcharOid, getMaxNameLength());
		f[6] = new Field(connection, "NULLABLE", iInt2Oid, 2);
		f[7] = new Field(connection, "CASE_SENSITIVE", iBoolOid, 1);
		f[8] = new Field(connection, "SEARCHABLE", iInt2Oid, 2);
		f[9] = new Field(connection, "UNSIGNED_ATTRIBUTE", iBoolOid, 1);
		f[10] = new Field(connection, "FIXED_PREC_SCALE", iBoolOid, 1);
		f[11] = new Field(connection, "AUTO_INCREMENT", iBoolOid, 1);
		f[12] = new Field(connection, "LOCAL_TYPE_NAME", iVarcharOid, getMaxNameLength());
		f[13] = new Field(connection, "MINIMUM_SCALE", iInt2Oid, 2);
		f[14] = new Field(connection, "MAXIMUM_SCALE", iInt2Oid, 2);
		f[15] = new Field(connection, "SQL_DATA_TYPE", iInt4Oid, 4);
		f[16] = new Field(connection, "SQL_DATETIME_SUB", iInt4Oid, 4);
		f[17] = new Field(connection, "NUM_PREC_RADIX", iInt4Oid, 4);

		String sql;
		if (connection.haveMinimumServerVersion("7.3")) {
			sql = "SELECT typname FROM pg_catalog.pg_type";
		} else {
			sql = "SELECT typname FROM pg_type";
		}

		ResultSet rs = connection.createStatement().executeQuery(sql);
		// cache some results, this will keep memory useage down, and speed
		// things up a little.
		byte b9[] = encoding.encode("9");
		byte b10[] = encoding.encode("10");
		byte bf[] = encoding.encode("f");
		byte bnn[] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.typeNoNulls));
		byte bts[] = encoding.encode(Integer.toString(java.sql.DatabaseMetaData.typeSearchable));

		while (rs.next())
		{
			byte[][] tuple = new byte[18][];
			String typname = rs.getString(1);
			tuple[0] = encoding.encode(typname);
			tuple[1] = encoding.encode(Integer.toString(connection.getSQLType(typname)));
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

		return (ResultSet) ((BaseStatement)connection.createStatement()).createResultSet(f, v, "OK", 1, 0);
	}

	/*
	 * Get a description of a table's indices and statistics. They are
	 * ordered by NON_UNIQUE, TYPE, INDEX_NAME, and ORDINAL_POSITION.
	 *
	 * <P>Each index column description has the following columns:
	 *	<OL>
	 *	<LI><B>TABLE_CAT</B> String => table catalog (may be null)
	 *	<LI><B>TABLE_SCHEM</B> String => table schema (may be null)
	 *	<LI><B>TABLE_NAME</B> String => table name
	 *	<LI><B>NON_UNIQUE</B> boolean => Can index values be non-unique?
	 *		false when TYPE is tableIndexStatistic
	 *	<LI><B>INDEX_QUALIFIER</B> String => index catalog (may be null);
	 *		null when TYPE is tableIndexStatistic
	 *	<LI><B>INDEX_NAME</B> String => index name; null when TYPE is
	 *		tableIndexStatistic
	 *	<LI><B>TYPE</B> short => index type:
	 *		<UL>
	 *		<LI> tableIndexStatistic - this identifies table statistics that are
	 *			 returned in conjuction with a table's index descriptions
	 *		<LI> tableIndexClustered - this is a clustered index
	 *		<LI> tableIndexHashed - this is a hashed index
	 *		<LI> tableIndexOther - this is some other style of index
	 *		</UL>
	 *	<LI><B>ORDINAL_POSITION</B> short => column sequence number
	 *		within index; zero when TYPE is tableIndexStatistic
	 *	<LI><B>COLUMN_NAME</B> String => column name; null when TYPE is
	 *		tableIndexStatistic
	 *	<LI><B>ASC_OR_DESC</B> String => column sort sequence, "A" => ascending
	 *		"D" => descending, may be null if sort sequence is not supported;
	 *		null when TYPE is tableIndexStatistic
	 *	<LI><B>CARDINALITY</B> int => When TYPE is tableIndexStatisic then
	 *		this is the number of rows in the table; otherwise it is the
	 *		number of unique values in the index.
	 *	<LI><B>PAGES</B> int => When TYPE is  tableIndexStatisic then
	 *		this is the number of pages used for the table, otherwise it
	 *		is the number of pages used for the current index.
	 *	<LI><B>FILTER_CONDITION</B> String => Filter condition, if any.
	 *		(may be null)
	 *	</OL>
	 *
	 * @param catalog a catalog name; "" retrieves those without a catalog
	 * @param schema a schema name pattern; "" retrieves those without a schema
	 * @param table a table name
	 * @param unique when true, return only indices for unique values;
	 *	   when false, return indices regardless of whether unique or not
	 * @param approximate when true, result is allowed to reflect approximate
	 *	   or out of data values; when false, results are requested to be
	 *	   accurate
	 * @return ResultSet each row is an index column description
	 */
	// Implementation note: This is required for Borland's JBuilder to work
	public java.sql.ResultSet getIndexInfo(String catalog, String schema, String tableName, boolean unique, boolean approximate) throws SQLException
	{
		String select;
		String from;
		String where = "";
		if (connection.haveMinimumServerVersion("7.3")) {
			select = "SELECT NULL AS TABLE_CAT, n.nspname AS TABLE_SCHEM, ";
			from = " FROM pg_catalog.pg_namespace n, pg_catalog.pg_class ct, pg_catalog.pg_class ci, pg_catalog.pg_index i, pg_catalog.pg_attribute a, pg_catalog.pg_am am ";
			where = " AND n.oid = ct.relnamespace ";
			if (schema != null && ! "".equals(schema)) {
				where += " AND n.nspname = '"+escapeQuotes(schema)+"' ";
			}
		} else {
			select = "SELECT NULL AS TABLE_CAT, NULL AS TABLE_SCHEM, ";
			from = " FROM pg_class ct, pg_class ci, pg_index i, pg_attribute a, pg_am am ";
		}

		String sql = select+
			" ct.relname AS TABLE_NAME, NOT i.indisunique AS NON_UNIQUE, NULL AS INDEX_QUALIFIER, ci.relname AS INDEX_NAME, "+
			" CASE i.indisclustered "+
			" WHEN true THEN "+java.sql.DatabaseMetaData.tableIndexClustered+
			" ELSE CASE am.amname "+
			"	WHEN 'hash' THEN "+java.sql.DatabaseMetaData.tableIndexHashed+
			"	ELSE "+java.sql.DatabaseMetaData.tableIndexOther+
			"	END "+
			" END AS TYPE, "+
			" a.attnum AS ORDINAL_POSITION, "+
			" a.attname AS COLUMN_NAME, "+
			" NULL AS ASC_OR_DESC, "+
			" ci.reltuples AS CARDINALITY, "+
			" ci.relpages AS PAGES, "+
			" NULL AS FILTER_CONDITION "+
			from+
			" WHERE ct.oid=i.indrelid AND ci.oid=i.indexrelid AND a.attrelid=ci.oid AND ci.relam=am.oid "+
			where+
			" AND ct.relname = '"+escapeQuotes(tableName)+"' ";

		if (unique) {
			sql += " AND i.indisunique ";
		}
		sql += " ORDER BY NON_UNIQUE, TYPE, INDEX_NAME, ORDINAL_POSITION ";
		return connection.createStatement().executeQuery(sql);
	}

	/**
	 * Tokenize based on words not on single characters.
	 */
	private static Vector tokenize(String input, String delimiter) {
		Vector result = new Vector();
		int start = 0;
		int end = input.length();
		int delimiterSize = delimiter.length();

		while (start < end) {
			int delimiterIndex = input.indexOf(delimiter,start);
			if (delimiterIndex < 0) {
				result.addElement(input.substring(start));
				break;
			} else {
				String token = input.substring(start,delimiterIndex);
				result.addElement(token);
				start = delimiterIndex + delimiterSize;
			}
		}
		return result;
	}

		

}
