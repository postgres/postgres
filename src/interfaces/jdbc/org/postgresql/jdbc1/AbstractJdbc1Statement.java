package org.postgresql.jdbc1;

import org.postgresql.core.BaseConnection;
import org.postgresql.core.BaseResultSet;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;
import org.postgresql.core.QueryExecutor;
import org.postgresql.largeobject.LargeObject;
import org.postgresql.largeobject.LargeObjectManager;
import org.postgresql.util.PGbytea;
import org.postgresql.util.PGobject;
import org.postgresql.util.PSQLException;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.UnsupportedEncodingException;
import java.math.BigDecimal;
import java.sql.CallableStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.SQLWarning;
import java.sql.Time;
import java.sql.Timestamp;
import java.sql.Types;
import java.util.Vector;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/AbstractJdbc1Statement.java,v 1.23 2003/05/29 04:48:33 barry Exp $
 * This class defines methods of the jdbc1 specification.  This class is
 * extended by org.postgresql.jdbc2.AbstractJdbc2Statement which adds the jdbc2
 * methods.  The real Statement class (for jdbc1) is org.postgresql.jdbc1.Jdbc1Statement
 */
public abstract class AbstractJdbc1Statement implements BaseStatement
{
        // The connection who created us
	protected BaseConnection connection;

	/** The warnings chain. */
	protected SQLWarning warnings = null;

	/** Maximum number of rows to return, 0 = unlimited */
	protected int maxrows = 0;

	/** Number of rows to get in a batch. */
	protected int fetchSize = 0;

	/** Timeout (in seconds) for a query (not used) */
	protected int timeout = 0;

	protected boolean replaceProcessingEnabled = true;

	/** The current results */
	protected BaseResultSet result = null;

	// Static variables for parsing SQL when replaceProcessing is true.
	private static final short IN_SQLCODE = 0;
	private static final short IN_STRING = 1;
	private static final short BACKSLASH = 2;
	private static final short ESC_TIMEDATE = 3;

	// Some performance caches
	private StringBuffer sbuf = new StringBuffer(32);

	//Used by the preparedstatement style methods
	protected String[] m_sqlFragments;
	private String[] m_origSqlFragments;
	private String[] m_executeSqlFragments;
	protected Object[] m_binds = new Object[0];

	protected String[] m_bindTypes = new String[0];
	protected String m_statementName = null;
        protected boolean m_statementIsCursor = false;

	private boolean m_useServerPrepare = false;
	private static int m_preparedCount = 1;

	//Used by the callablestatement style methods
	private static final String JDBC_SYNTAX = "{[? =] call <some_function> ([? [,?]*]) }";
	private static final String RESULT_ALIAS = "result";
	private String originalSql = "";
	private boolean isFunction;
	// functionReturnType contains the user supplied value to check
	// testReturn contains a modified version to make it easier to
	// check the getXXX methods..
	private int functionReturnType;
	private int testReturn;
	// returnTypeSet is true when a proper call to registerOutParameter has been made
	private boolean returnTypeSet;
	protected Object callResult;


	public abstract BaseResultSet createResultSet(Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException;

	public AbstractJdbc1Statement (BaseConnection connection)
	{
		this.connection = connection;
	}

	public AbstractJdbc1Statement (BaseConnection connection, String p_sql) throws SQLException
	{
		this.connection = connection;
		parseSqlStmt(p_sql);  // this allows Callable stmt to override
	}

	public BaseConnection getPGConnection() {
		return connection;
	}

	public String getStatementName() {
		return m_statementName;
	}

	public int getFetchSize() throws SQLException {
		return fetchSize;
	}

	protected void parseSqlStmt (String p_sql) throws SQLException
	{
		String l_sql = p_sql;

		l_sql = replaceProcessing(l_sql);

		if (this instanceof CallableStatement)
		{
			l_sql = modifyJdbcCall(l_sql);
		}

		Vector v = new Vector();
		boolean inQuotes = false;
		int lastParmEnd = 0, i;

		for (i = 0; i < l_sql.length(); ++i)
		{
			int c = l_sql.charAt(i);

			if (c == '\'')
				inQuotes = !inQuotes;
			if (c == '?' && !inQuotes)
			{
				v.addElement(l_sql.substring (lastParmEnd, i));
				lastParmEnd = i + 1;
			}
		}
		v.addElement(l_sql.substring (lastParmEnd, l_sql.length()));

		m_sqlFragments = new String[v.size()];
		m_binds = new String[v.size() - 1];
		m_bindTypes = new String[v.size() - 1];

		for (i = 0 ; i < m_sqlFragments.length; ++i)
			m_sqlFragments[i] = (String)v.elementAt(i);

	}

  
	/*
	 * Execute a SQL statement that retruns a single ResultSet
	 *
	 * @param sql typically a static SQL SELECT statement
	 * @return a ResulSet that contains the data produced by the query
	 * @exception SQLException if a database access error occurs
	 */
	public java.sql.ResultSet executeQuery(String p_sql) throws SQLException
	{
		String l_sql = replaceProcessing(p_sql);
		m_sqlFragments = new String[] {l_sql};
		m_binds = new Object[0];
		//If we have already created a server prepared statement, we need
		//to deallocate the existing one
		if (m_statementName != null)
		{
			try
			{
                                if (!m_statementIsCursor)
                                        connection.execSQL("DEALLOCATE " + m_statementName);
			}
			catch (Exception e)
			{
			}
			finally
			{
				m_statementName = null;
                                m_statementIsCursor = false;
				m_origSqlFragments = null;
				m_executeSqlFragments = null;
			}
		}
		return executeQuery();
	}

	/*
	 * A Prepared SQL query is executed and its ResultSet is returned
	 *
	 * @return a ResultSet that contains the data produced by the
	 *		 *	query - never null
	 * @exception SQLException if a database access error occurs
	 */
	public java.sql.ResultSet executeQuery() throws SQLException
	{
                this.execute();

		while (result != null && !result.reallyResultSet())
			result = (BaseResultSet) result.getNext();
		if (result == null)
			throw new PSQLException("postgresql.stat.noresult");
		return (ResultSet) result;
	}

	/*
	 * Execute a SQL INSERT, UPDATE or DELETE statement.  In addition
	 * SQL statements that return nothing such as SQL DDL statements
	 * can be executed
	 *
	 * @param sql a SQL statement
	 * @return either a row count, or 0 for SQL commands
	 * @exception SQLException if a database access error occurs
	 */
	public int executeUpdate(String p_sql) throws SQLException
	{
		String l_sql = replaceProcessing(p_sql);
		m_sqlFragments = new String[] {l_sql};
		m_binds = new Object[0];
		//If we have already created a server prepared statement, we need
		//to deallocate the existing one
		if (m_statementName != null) {
			connection.execSQL("DEALLOCATE " + m_statementName);
			m_statementName = null;
			m_origSqlFragments = null;
			m_executeSqlFragments = null;
		}
		return executeUpdate();
	}

	/*
	 * Execute a SQL INSERT, UPDATE or DELETE statement.  In addition,
	 * SQL statements that return nothing such as SQL DDL statements can
	 * be executed.
	 *
	 * @return either the row count for INSERT, UPDATE or DELETE; or
	 *		 *	0 for SQL statements that return nothing.
	 * @exception SQLException if a database access error occurs
	 */
	public int executeUpdate() throws SQLException
	{
		this.execute();
		if (result.reallyResultSet())
			throw new PSQLException("postgresql.stat.result");
		return this.getUpdateCount();
	}

	/*
	 * Execute a SQL statement that may return multiple results. We
	 * don't have to worry about this since we do not support multiple
	 * ResultSets.	 You can use getResultSet or getUpdateCount to
	 * retrieve the result.
	 *
	 * @param sql any SQL statement
	 * @return true if the next result is a ResulSet, false if it is
	 *	an update count or there are no more results
	 * @exception SQLException if a database access error occurs
	 */
	public boolean execute(String p_sql) throws SQLException
	{
		String l_sql = replaceProcessing(p_sql);
		m_sqlFragments = new String[] {l_sql};
		m_binds = new Object[0];
		//If we have already created a server prepared statement, we need
		//to deallocate the existing one
		if (m_statementName != null) {
			connection.execSQL("DEALLOCATE " + m_statementName);
			m_statementName = null;
			m_origSqlFragments = null;
			m_executeSqlFragments = null;
		}
		return execute();
	}

	/*
	 * Some prepared statements return multiple results; the execute method
	 * handles these complex statements as well as the simpler form of
	 * statements handled by executeQuery and executeUpdate
         *
         * This method also handles the translation of the query into a cursor based
         * query if the user has specified a fetch size and set the connection
         * into a non-auto commit state.
	 *
	 * @return true if the next result is a ResultSet; false if it is an
	 *		 update count or there are no more results
	 * @exception SQLException if a database access error occurs
	 */
	public boolean execute() throws SQLException
	{
		if (isFunction && !returnTypeSet)
			throw new PSQLException("postgresql.call.noreturntype");
		if (isFunction)
		{ // set entry 1 to dummy entry..
			m_binds[0] = ""; // dummy entry which ensured that no one overrode
			m_bindTypes[0] = PG_TEXT;
			// and calls to setXXX (2,..) really went to first arg in a function call..
		}

		// New in 7.1, if we have a previous resultset then force it to close
		// This brings us nearer to compliance, and helps memory management.
		// Internal stuff will call ExecSQL directly, bypassing this.

		if (result != null)
		{
			java.sql.ResultSet rs = getResultSet();
			if (rs != null)
				rs.close();
		}

		//Use server prepared statements if directed
		if (m_useServerPrepare)
		{
			if (m_statementName == null)
			{
				m_statementName = "JDBC_STATEMENT_" + m_preparedCount++;
				m_origSqlFragments = new String[m_sqlFragments.length];
				m_executeSqlFragments = new String[m_sqlFragments.length];
				System.arraycopy(m_sqlFragments, 0, m_origSqlFragments, 0, m_sqlFragments.length);
				m_executeSqlFragments[0] = "EXECUTE " + m_statementName;
				if (m_sqlFragments.length > 1)
				{
					m_executeSqlFragments[0] = m_executeSqlFragments[0] + "(";
					for (int i = 1; i < m_bindTypes.length; i++)
					{
						m_executeSqlFragments[i] = ", ";
					}
					m_executeSqlFragments[m_bindTypes.length] = ")";
				}
				synchronized (sbuf)
				{
					sbuf.setLength(0);
					sbuf.append("PREPARE ");
					sbuf.append(m_statementName);
					if (m_origSqlFragments.length > 1)
					{
						sbuf.append("(");
						for (int i = 0; i < m_bindTypes.length - 1; i++)
						{
							sbuf.append(m_bindTypes[i]);
							sbuf.append(", ");
						}
						sbuf.append(m_bindTypes[m_bindTypes.length - 1]);
						sbuf.append(")");
					}
					sbuf.append(" AS ");
					sbuf.append(m_origSqlFragments[0]);
					for (int i = 1; i < m_origSqlFragments.length; i++)
					{
						sbuf.append(" $");
						sbuf.append(i);
						sbuf.append(" ");
						sbuf.append(m_origSqlFragments[i]);
					}
					sbuf.append("; ");

					sbuf.append(m_executeSqlFragments[0]);
					m_sqlFragments[0] = sbuf.toString();
					System.arraycopy(m_executeSqlFragments, 1, m_sqlFragments, 1, m_sqlFragments.length - 1);
				}

			}
			else
			{
				m_sqlFragments = m_executeSqlFragments;
			}
		}

                // Use a cursor if directed and in a transaction.
                else if (fetchSize > 0 && !connection.getAutoCommit())
                {
                        // The first thing to do is transform the statement text into the cursor form.
                        String[] cursorBasedSql = new String[m_sqlFragments.length];
                        // Pinch the prepared count for our own nefarious purposes.
                        String statementName = "JDBC_CURS_" + m_preparedCount++;
                        // Setup the cursor decleration.
                        // Note that we don't need a BEGIN because we've already
                        // made sure we're executing inside a transaction.
                        String cursDecl = "DECLARE " + statementName + " CURSOR FOR ";
                        String endCurs = " FETCH FORWARD " + fetchSize + " FROM " + statementName + ";";

                        // Copy the real query to the curs decleration.
                        try
                        {
                                // Need to confirm this with Barry Lind.
                                if (cursorBasedSql.length > 1)
                                        throw new IllegalStateException("cursor fetches not supported with prepared statements.");
                                for (int i = 0; i < cursorBasedSql.length; i++)
                                {
                                        if (i == 0)
                                        {
                                                if (m_sqlFragments[i].trim().toUpperCase().startsWith("DECLARE "))
                                                        throw new IllegalStateException("statement is already cursor based.");
                                                cursorBasedSql[i] = cursDecl;
                                        }

                                        if (cursorBasedSql[i] != null)
                                                cursorBasedSql[i] += m_sqlFragments[i];
                                        else
                                                cursorBasedSql[i] = m_sqlFragments[i];

                                        if (i == cursorBasedSql.length - 1)
                                        {
                                                // We have to be smart about adding the delimitting ";"
                                                if (m_sqlFragments[i].endsWith(";"))
                                                        cursorBasedSql[i] += endCurs;
                                                else
                                                        cursorBasedSql[i] += (";" + endCurs);
                                        }
                                        else if (m_sqlFragments[i].indexOf(";") > -1)
                                        {
                                                throw new IllegalStateException("multiple statements not "
                                                                                + "allowed with cursor based querys.");
                                        }
                                }

                                // Make the cursor based query the one that will be used.
                                if (org.postgresql.Driver.logDebug)
                                        org.postgresql.Driver.debug("using cursor based sql with cursor name " + statementName);

                                // Do all of this after exceptions have been thrown.
                                m_statementName = statementName;
                                m_statementIsCursor = true;
                                m_sqlFragments = cursorBasedSql;
                        }
                        catch (IllegalStateException e)
                        {
                                // Something went wrong generating the cursor based statement.
                                if (org.postgresql.Driver.logDebug)
                                        org.postgresql.Driver.debug(e.getMessage());
                        }
                }

		// New in 7.1, pass Statement so that ExecSQL can customise to it                
		result = QueryExecutor.execute(m_sqlFragments,
                                               m_binds,
                                               this);

		//If we are executing a callable statement function set the return data
		if (isFunction)
		{
			if (!result.reallyResultSet())
				throw new PSQLException("postgresql.call.noreturnval");
			if (!result.next ())
				throw new PSQLException ("postgresql.call.noreturnval");
			callResult = result.getObject(1);
			int columnType = result.getMetaData().getColumnType(1);
			if (columnType != functionReturnType)
				throw new PSQLException ("postgresql.call.wrongrtntype",
										 new Object[]{
											 "java.sql.Types=" + columnType, "java.sql.Types=" + functionReturnType });
			result.close ();
			return true;
		}
		else
		{
			return (result != null && result.reallyResultSet());
		}
	}
	
	/*
	 * setCursorName defines the SQL cursor name that will be used by
	 * subsequent execute methods.	This name can then be used in SQL
	 * positioned update/delete statements to identify the current row
	 * in the ResultSet generated by this statement.  If a database
	 * doesn't support positioned update/delete, this method is a
	 * no-op.
	 *
	 * <p><B>Note:</B> By definition, positioned update/delete execution
	 * must be done by a different Statement than the one which
	 * generated the ResultSet being used for positioning.	Also, cursor
	 * names must be unique within a Connection.
	 *
	 * <p>We throw an additional constriction.	There can only be one
	 * cursor active at any one time.
	 *
	 * @param name the new cursor name
	 * @exception SQLException if a database access error occurs
	 */
	public void setCursorName(String name) throws SQLException
	{
		connection.setCursorName(name);
	}


	/*
	 * getUpdateCount returns the current result as an update count,
	 * if the result is a ResultSet or there are no more results, -1
	 * is returned.  It should only be called once per result.
	 *
	 * @return the current result as an update count.
	 * @exception SQLException if a database access error occurs
	 */
	public int getUpdateCount() throws SQLException
	{
		if (result == null)
			return -1;
		if (isFunction)
			return 1;
		if (result.reallyResultSet())
			return -1;
		return result.getResultCount();
	}

	/*
	 * getMoreResults moves to a Statement's next result.  If it returns
	 * true, this result is a ResulSet.
	 *
	 * @return true if the next ResultSet is valid
	 * @exception SQLException if a database access error occurs
	 */
	public boolean getMoreResults() throws SQLException
	{
		result = (BaseResultSet) result.getNext();
		return (result != null && result.reallyResultSet());
	}



	/*
	 * Returns the status message from the current Result.<p>
	 * This is used internally by the driver.
	 *
	 * @return status message from backend
	 */
	public String getResultStatusString()
	{
		if (result == null)
			return null;
		return result.getStatusString();
	}

	/*
	 * The maxRows limit is set to limit the number of rows that
	 * any ResultSet can contain.  If the limit is exceeded, the
	 * excess rows are silently dropped.
	 *
	 * @return the current maximum row limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxRows() throws SQLException
	{
		return maxrows;
	}

	/*
	 * Set the maximum number of rows
	 *
	 * @param max the new max rows limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 * @see getMaxRows
	 */
	public void setMaxRows(int max) throws SQLException
	{
		maxrows = max;
	}

	/*
	 * If escape scanning is on (the default), the driver will do escape
	 * substitution before sending the SQL to the database.
	 *
	 * @param enable true to enable; false to disable
	 * @exception SQLException if a database access error occurs
	 */
	public void setEscapeProcessing(boolean enable) throws SQLException
	{
		replaceProcessingEnabled = enable;
	}

	/*
	 * The queryTimeout limit is the number of seconds the driver
	 * will wait for a Statement to execute.  If the limit is
	 * exceeded, a SQLException is thrown.
	 *
	 * @return the current query timeout limit in seconds; 0 = unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public int getQueryTimeout() throws SQLException
	{
		return timeout;
	}

	/*
	 * Sets the queryTimeout limit
	 *
	 * @param seconds - the new query timeout limit in seconds
	 * @exception SQLException if a database access error occurs
	 */
	public void setQueryTimeout(int seconds) throws SQLException
	{
		timeout = seconds;
	}

	/**
	 * This adds a warning to the warning chain.
	 * @param msg message to add
	 */
	public void addWarning(String msg)
	{
		if (warnings != null)
			warnings.setNextWarning(new SQLWarning(msg));
		else
			warnings = new SQLWarning(msg);
	}

	/*
	 * The first warning reported by calls on this Statement is
	 * returned.  A Statement's execute methods clear its SQLWarning
	 * chain.  Subsequent Statement warnings will be chained to this
	 * SQLWarning.
	 *
	 * <p>The Warning chain is automatically cleared each time a statement
	 * is (re)executed.
	 *
	 * <p><B>Note:</B>	If you are processing a ResultSet then any warnings
	 * associated with ResultSet reads will be chained on the ResultSet
	 * object.
	 *
	 * @return the first SQLWarning on null
	 * @exception SQLException if a database access error occurs
	 */
	public SQLWarning getWarnings() throws SQLException
	{
		return warnings;
	}

	/*
	 * The maxFieldSize limit (in bytes) is the maximum amount of
	 * data returned for any column value; it only applies to
	 * BINARY, VARBINARY, LONGVARBINARY, CHAR, VARCHAR and LONGVARCHAR
	 * columns.  If the limit is exceeded, the excess data is silently
	 * discarded.
	 *
	 * @return the current max column size limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxFieldSize() throws SQLException
	{
		return 8192;		// We cannot change this
	}

	/*
	 * Sets the maxFieldSize - NOT! - We throw an SQLException just
	 * to inform them to stop doing this.
	 *
	 * @param max the new max column size limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public void setMaxFieldSize(int max) throws SQLException
	{
		throw new PSQLException("postgresql.stat.maxfieldsize");
	}

	/*
	 * After this call, getWarnings returns null until a new warning
	 * is reported for this Statement.
	 *
	 * @exception SQLException if a database access error occurs
	 */
	public void clearWarnings() throws SQLException
	{
		warnings = null;
	}

	/*
	 * Cancel can be used by one thread to cancel a statement that
	 * is being executed by another thread.
	 * <p>
	 * Not implemented, this method is a no-op.
	 *
	 * @exception SQLException only because thats the spec.
	 */
	public void cancel() throws SQLException
	{
		throw new PSQLException("postgresql.unimplemented");
	}

	/*
	 * getResultSet returns the current result as a ResultSet.	It
	 * should only be called once per result.
	 *
	 * @return the current result set; null if there are no more
	 * @exception SQLException if a database access error occurs (why?)
	 */
	public java.sql.ResultSet getResultSet() throws SQLException
	{
		if (result != null && result.reallyResultSet())
			return (ResultSet) result;
		return null;
	}

	/*
	 * In many cases, it is desirable to immediately release a
	 * Statement's database and JDBC resources instead of waiting
	 * for this to happen when it is automatically closed.	The
	 * close method provides this immediate release.
	 *
	 * <p><B>Note:</B> A Statement is automatically closed when it is
	 * garbage collected.  When a Statement is closed, its current
	 * ResultSet, if one exists, is also closed.
	 *
	 * @exception SQLException if a database access error occurs (why?)
	 */
	public void close() throws SQLException
	{
		// Force the ResultSet to close
		java.sql.ResultSet rs = getResultSet();
		if (rs != null)
			rs.close();

		// If using server prepared statements deallocate them
		if (m_useServerPrepare && m_statementName != null) {
			connection.execSQL("DEALLOCATE " + m_statementName);
		}

		// Disasociate it from us (For Garbage Collection)
		result = null;
	}

	/*
	 * Filter the SQL string of Java SQL Escape clauses.
	 *
	 * Currently implemented Escape clauses are those mentioned in 11.3
	 * in the specification. Basically we look through the sql string for
	 * {d xxx}, {t xxx} or {ts xxx} in non-string sql code. When we find
	 * them, we just strip the escape part leaving only the xxx part.
	 * So, something like "select * from x where d={d '2001-10-09'}" would
	 * return "select * from x where d= '2001-10-09'".
	 */
	protected String replaceProcessing(String p_sql)
	{
		if (replaceProcessingEnabled)
		{
			// Since escape codes can only appear in SQL CODE, we keep track
			// of if we enter a string or not.
			StringBuffer newsql = new StringBuffer(p_sql.length());
			short state = IN_SQLCODE;

			int i = -1;
			int len = p_sql.length();
			while (++i < len)
			{
				char c = p_sql.charAt(i);
				switch (state)
				{
					case IN_SQLCODE:
						if (c == '\'')				  // start of a string?
							state = IN_STRING;
						else if (c == '{')			  // start of an escape code?
							if (i + 1 < len)
							{
								char next = p_sql.charAt(i + 1);
								if (next == 'd')
								{
									state = ESC_TIMEDATE;
									i++;
									break;
								}
								else if (next == 't')
								{
									state = ESC_TIMEDATE;
									i += (i + 2 < len && p_sql.charAt(i + 2) == 's') ? 2 : 1;
									break;
								}
							}
						newsql.append(c);
						break;

					case IN_STRING:
						if (c == '\'')				   // end of string?
							state = IN_SQLCODE;
						else if (c == '\\')			   // a backslash?
							state = BACKSLASH;

						newsql.append(c);
						break;

					case BACKSLASH:
						state = IN_STRING;

						newsql.append(c);
						break;

					case ESC_TIMEDATE:
						if (c == '}')
							state = IN_SQLCODE;		  // end of escape code.
						else
							newsql.append(c);
						break;
				} // end switch
			}

			return newsql.toString();
		}
		else
		{
			return p_sql;
		}
	}

	/*
	 *
	 * The following methods are postgres extensions and are defined
	 * in the interface BaseStatement
	 *
	 */

	/*
	 * Returns the Last inserted/updated oid.  Deprecated in 7.2 because
			* range of OID values is greater than a java signed int.
	 * @deprecated Replaced by getLastOID in 7.2
	 */
	public int getInsertedOID() throws SQLException
	{
		if (result == null)
			return 0;
		return (int) result.getLastOID();
	}

	/*
	 * Returns the Last inserted/updated oid.
	 * @return OID of last insert
			* @since 7.2
	 */
	public long getLastOID() throws SQLException
	{
		if (result == null)
			return 0;
		return result.getLastOID();
	}

	/*
	 * Set a parameter to SQL NULL
	 *
	 * <p><B>Note:</B> You must specify the parameters SQL type (although
	 * PostgreSQL ignores it)
	 *
	 * @param parameterIndex the first parameter is 1, etc...
	 * @param sqlType the SQL type code defined in java.sql.Types
	 * @exception SQLException if a database access error occurs
	 */
	public void setNull(int parameterIndex, int sqlType) throws SQLException
	{
        String l_pgType;
		switch (sqlType)
		{
			case Types.INTEGER:
				l_pgType = PG_INTEGER;
				break;
			case Types.TINYINT:
			case Types.SMALLINT:
				l_pgType = PG_INT2;
				break;
			case Types.BIGINT:
				l_pgType = PG_INT8;
				break;
			case Types.REAL:
			case Types.FLOAT:
				l_pgType = PG_FLOAT;
				break;
			case Types.DOUBLE:
				l_pgType = PG_DOUBLE;
				break;
			case Types.DECIMAL:
			case Types.NUMERIC:
				l_pgType = PG_NUMERIC;
				break;
			case Types.CHAR:
			case Types.VARCHAR:
			case Types.LONGVARCHAR:
				l_pgType = PG_TEXT;
				break;
			case Types.DATE:
				l_pgType = PG_DATE;
				break;
			case Types.TIME:
				l_pgType = PG_TIME;
				break;
			case Types.TIMESTAMP:
				l_pgType = PG_TIMESTAMPTZ;
				break;
			case Types.BINARY:
			case Types.VARBINARY:
			case Types.LONGVARBINARY:
				l_pgType = PG_BYTEA;
				break;
			case Types.OTHER:
				l_pgType = PG_TEXT;
				break;
			default:
				l_pgType = PG_TEXT;
		}
		bind(parameterIndex, "null", l_pgType);
	}

	/*
	 * Set a parameter to a Java boolean value.  The driver converts this
	 * to a SQL BIT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setBoolean(int parameterIndex, boolean x) throws SQLException
	{
		bind(parameterIndex, x ? "'t'" : "'f'", PG_BOOLEAN);
	}

	/*
	 * Set a parameter to a Java byte value.  The driver converts this to
	 * a SQL TINYINT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setByte(int parameterIndex, byte x) throws SQLException
	{
		bind(parameterIndex, Integer.toString(x), PG_TEXT);
	}

	/*
	 * Set a parameter to a Java short value.  The driver converts this
	 * to a SQL SMALLINT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setShort(int parameterIndex, short x) throws SQLException
	{
		bind(parameterIndex, Integer.toString(x), PG_INT2);
	}

	/*
	 * Set a parameter to a Java int value.  The driver converts this to
	 * a SQL INTEGER value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setInt(int parameterIndex, int x) throws SQLException
	{
		bind(parameterIndex, Integer.toString(x), PG_INTEGER);
	}

	/*
	 * Set a parameter to a Java long value.  The driver converts this to
	 * a SQL BIGINT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setLong(int parameterIndex, long x) throws SQLException
	{
		bind(parameterIndex, Long.toString(x), PG_INT8);
	}

	/*
	 * Set a parameter to a Java float value.  The driver converts this
	 * to a SQL FLOAT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setFloat(int parameterIndex, float x) throws SQLException
	{
		bind(parameterIndex, Float.toString(x), PG_FLOAT);
	}

	/*
	 * Set a parameter to a Java double value.	The driver converts this
	 * to a SQL DOUBLE value when it sends it to the database
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setDouble(int parameterIndex, double x) throws SQLException
	{
		bind(parameterIndex, Double.toString(x), PG_DOUBLE);
	}

	/*
	 * Set a parameter to a java.lang.BigDecimal value.  The driver
	 * converts this to a SQL NUMERIC value when it sends it to the
	 * database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setBigDecimal(int parameterIndex, BigDecimal x) throws SQLException
	{
		if (x == null)
			setNull(parameterIndex, Types.DECIMAL);
		else
		{
			bind(parameterIndex, x.toString(), PG_NUMERIC);
		}
	}

	/*
	 * Set a parameter to a Java String value.	The driver converts this
	 * to a SQL VARCHAR or LONGVARCHAR value (depending on the arguments
	 * size relative to the driver's limits on VARCHARs) when it sends it
	 * to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setString(int parameterIndex, String x) throws SQLException
	{
		setString(parameterIndex, x, PG_TEXT);
	}

	public void setString(int parameterIndex, String x, String type) throws SQLException
	{
		// if the passed string is null, then set this column to null
		if (x == null)
			setNull(parameterIndex, Types.VARCHAR);
		else
		{
			// use the shared buffer object. Should never clash but this makes
			// us thread safe!
			synchronized (sbuf)
			{
				sbuf.setLength(0);
				sbuf.ensureCapacity(x.length() + (int)(x.length() / 10));
				int i;

				sbuf.append('\'');
				for (i = 0 ; i < x.length() ; ++i)
				{
					char c = x.charAt(i);
					if (c == '\\' || c == '\'')
						sbuf.append((char)'\\');
					sbuf.append(c);
				}
				sbuf.append('\'');
				bind(parameterIndex, sbuf.toString(), type);
			}
		}
	}

	/*
	 * Set a parameter to a Java array of bytes.  The driver converts this
	 * to a SQL VARBINARY or LONGVARBINARY (depending on the argument's
	 * size relative to the driver's limits on VARBINARYs) when it sends
	 * it to the database.
	 *
	 * <p>Implementation note:
	 * <br>With org.postgresql, this creates a large object, and stores the
	 * objects oid in this column.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setBytes(int parameterIndex, byte x[]) throws SQLException
	{
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports the bytea datatype for byte arrays
			if (null == x)
			{
				setNull(parameterIndex, Types.VARBINARY);
			}
			else
			{
				setString(parameterIndex, PGbytea.toPGString(x), PG_TEXT);
			}
		}
		else
		{
			//Version 7.1 and earlier support done as LargeObjects
			LargeObjectManager lom = connection.getLargeObjectAPI();
			int oid = lom.create();
			LargeObject lob = lom.open(oid);
			lob.write(x);
			lob.close();
			setInt(parameterIndex, oid);
		}
	}

	/*
	 * Set a parameter to a java.sql.Date value.  The driver converts this
	 * to a SQL DATE value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setDate(int parameterIndex, java.sql.Date x) throws SQLException
	{
		if (null == x)
		{
			setNull(parameterIndex, Types.DATE);
		}
		else
		{
			bind(parameterIndex, "'" + x.toString() + "'", PG_DATE);
		}
	}

	/*
	 * Set a parameter to a java.sql.Time value.  The driver converts
	 * this to a SQL TIME value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...));
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setTime(int parameterIndex, Time x) throws SQLException
	{
		if (null == x)
		{
			setNull(parameterIndex, Types.TIME);
		}
		else
		{
			bind(parameterIndex, "'" + x.toString() + "'", PG_TIME);
		}
	}

	/*
	 * Set a parameter to a java.sql.Timestamp value.  The driver converts
	 * this to a SQL TIMESTAMP value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setTimestamp(int parameterIndex, Timestamp x) throws SQLException
	{
		if (null == x)
		{
			setNull(parameterIndex, Types.TIMESTAMP);
		}
		else
		{
			// Use the shared StringBuffer
			synchronized (sbuf)
			{
				sbuf.setLength(0);
				sbuf.ensureCapacity(32);
				sbuf.append("'");
				//format the timestamp
				//we do our own formating so that we can get a format
				//that works with both timestamp with time zone and
				//timestamp without time zone datatypes.
				//The format is '2002-01-01 23:59:59.123456-0130'
				//we need to include the local time and timezone offset
				//so that timestamp without time zone works correctly
				int l_year = x.getYear() + 1900;
				sbuf.append(l_year);
				sbuf.append('-');
				int l_month = x.getMonth() + 1;
				if (l_month < 10)
					sbuf.append('0');
				sbuf.append(l_month);
				sbuf.append('-');
				int l_day = x.getDate();
				if (l_day < 10)
					sbuf.append('0');
				sbuf.append(l_day);
				sbuf.append(' ');
				int l_hours = x.getHours();
				if (l_hours < 10)
					sbuf.append('0');
				sbuf.append(l_hours);
				sbuf.append(':');
				int l_minutes = x.getMinutes();
				if (l_minutes < 10)
					sbuf.append('0');
				sbuf.append(l_minutes);
				sbuf.append(':');
				int l_seconds = x.getSeconds();
				if (l_seconds < 10)
					sbuf.append('0');
				sbuf.append(l_seconds);
				// Make decimal from nanos.
				char[] l_decimal = {'0', '0', '0', '0', '0', '0', '0', '0', '0'};
				char[] l_nanos = Integer.toString(x.getNanos()).toCharArray();
				System.arraycopy(l_nanos, 0, l_decimal, l_decimal.length - l_nanos.length, l_nanos.length);
				sbuf.append('.');
				if (connection.haveMinimumServerVersion("7.2"))
				{
					sbuf.append(l_decimal, 0, 6);
				}
				else
				{
					// Because 7.1 include bug that "hh:mm:59.999" becomes "hh:mm:60.00".
					sbuf.append(l_decimal, 0, 2);
				}
				//add timezone offset
				int l_offset = -(x.getTimezoneOffset());
				int l_houros = l_offset / 60;
				if (l_houros >= 0)
				{
					sbuf.append('+');
				}
				else
				{
					sbuf.append('-');
				}
				if (l_houros > -10 && l_houros < 10)
					sbuf.append('0');
				if (l_houros >= 0)
				{
					sbuf.append(l_houros);
				}
				else
				{
					sbuf.append( -l_houros);
				}
				int l_minos = l_offset - (l_houros * 60);
				if (l_minos != 0)
				{
					if (l_minos < 10)
						sbuf.append('0');
					sbuf.append(l_minos);
				}
				sbuf.append("'");
				bind(parameterIndex, sbuf.toString(), PG_TIMESTAMPTZ);
			}

		}
	}

	/*
	 * When a very large ASCII value is input to a LONGVARCHAR parameter,
	 * it may be more practical to send it via a java.io.InputStream.
	 * JDBC will read the data from the stream as needed, until it reaches
	 * end-of-file.  The JDBC driver will do any necessary conversion from
	 * ASCII to the database char format.
	 *
	 * <P><B>Note:</B> This stream object can either be a standard Java
	 * stream object or your own subclass that implements the standard
	 * interface.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @param length the number of bytes in the stream
	 * @exception SQLException if a database access error occurs
	 */
	public void setAsciiStream(int parameterIndex, InputStream x, int length) throws SQLException
	{
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports AsciiStream for all PG text types (char, varchar, text)
			//As the spec/javadoc for this method indicate this is to be used for
			//large String values (i.e. LONGVARCHAR)  PG doesn't have a separate
			//long varchar datatype, but with toast all text datatypes are capable of
			//handling very large values.  Thus the implementation ends up calling
			//setString() since there is no current way to stream the value to the server
			try
			{
				InputStreamReader l_inStream = new InputStreamReader(x, "ASCII");
				char[] l_chars = new char[length];
				int l_charsRead = l_inStream.read(l_chars, 0, length);
				setString(parameterIndex, new String(l_chars, 0, l_charsRead), PG_TEXT);
			}
			catch (UnsupportedEncodingException l_uee)
			{
				throw new PSQLException("postgresql.unusual", l_uee);
			}
			catch (IOException l_ioe)
			{
				throw new PSQLException("postgresql.unusual", l_ioe);
			}
		}
		else
		{
			//Version 7.1 supported only LargeObjects by treating everything
			//as binary data
			setBinaryStream(parameterIndex, x, length);
		}
	}

	/*
	 * When a very large Unicode value is input to a LONGVARCHAR parameter,
	 * it may be more practical to send it via a java.io.InputStream.
	 * JDBC will read the data from the stream as needed, until it reaches
	 * end-of-file.  The JDBC driver will do any necessary conversion from
	 * UNICODE to the database char format.
	 *
	 * <P><B>Note:</B> This stream object can either be a standard Java
	 * stream object or your own subclass that implements the standard
	 * interface.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setUnicodeStream(int parameterIndex, InputStream x, int length) throws SQLException
	{
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports AsciiStream for all PG text types (char, varchar, text)
			//As the spec/javadoc for this method indicate this is to be used for
			//large String values (i.e. LONGVARCHAR)  PG doesn't have a separate
			//long varchar datatype, but with toast all text datatypes are capable of
			//handling very large values.  Thus the implementation ends up calling
			//setString() since there is no current way to stream the value to the server
			try
			{
				InputStreamReader l_inStream = new InputStreamReader(x, "UTF-8");
				char[] l_chars = new char[length];
				int l_charsRead = l_inStream.read(l_chars, 0, length);
				setString(parameterIndex, new String(l_chars, 0, l_charsRead), PG_TEXT);
			}
			catch (UnsupportedEncodingException l_uee)
			{
				throw new PSQLException("postgresql.unusual", l_uee);
			}
			catch (IOException l_ioe)
			{
				throw new PSQLException("postgresql.unusual", l_ioe);
			}
		}
		else
		{
			//Version 7.1 supported only LargeObjects by treating everything
			//as binary data
			setBinaryStream(parameterIndex, x, length);
		}
	}

	/*
	 * When a very large binary value is input to a LONGVARBINARY parameter,
	 * it may be more practical to send it via a java.io.InputStream.
	 * JDBC will read the data from the stream as needed, until it reaches
	 * end-of-file.
	 *
	 * <P><B>Note:</B> This stream object can either be a standard Java
	 * stream object or your own subclass that implements the standard
	 * interface.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setBinaryStream(int parameterIndex, InputStream x, int length) throws SQLException
	{
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports BinaryStream for for the PG bytea type
			//As the spec/javadoc for this method indicate this is to be used for
			//large binary values (i.e. LONGVARBINARY)	PG doesn't have a separate
			//long binary datatype, but with toast the bytea datatype is capable of
			//handling very large values.  Thus the implementation ends up calling
			//setBytes() since there is no current way to stream the value to the server
			byte[] l_bytes = new byte[length];
			int l_bytesRead;
			try
			{
				l_bytesRead = x.read(l_bytes, 0, length);
			}
			catch (IOException l_ioe)
			{
				throw new PSQLException("postgresql.unusual", l_ioe);
			}
			if (l_bytesRead == length)
			{
				setBytes(parameterIndex, l_bytes);
			}
			else
			{
				//the stream contained less data than they said
				byte[] l_bytes2 = new byte[l_bytesRead];
				System.arraycopy(l_bytes, 0, l_bytes2, 0, l_bytesRead);
				setBytes(parameterIndex, l_bytes2);
			}
		}
		else
		{
			//Version 7.1 only supported streams for LargeObjects
			//but the jdbc spec indicates that streams should be
			//available for LONGVARBINARY instead
			LargeObjectManager lom = connection.getLargeObjectAPI();
			int oid = lom.create();
			LargeObject lob = lom.open(oid);
			OutputStream los = lob.getOutputStream();
			try
			{
				// could be buffered, but then the OutputStream returned by LargeObject
				// is buffered internally anyhow, so there would be no performance
				// boost gained, if anything it would be worse!
				int c = x.read();
				int p = 0;
				while (c > -1 && p < length)
				{
					los.write(c);
					c = x.read();
					p++;
				}
				los.close();
			}
			catch (IOException se)
			{
				throw new PSQLException("postgresql.unusual", se);
			}
			// lob is closed by the stream so don't call lob.close()
			setInt(parameterIndex, oid);
		}
	}


	/*
	 * In general, parameter values remain in force for repeated used of a
	 * Statement.  Setting a parameter value automatically clears its
	 * previous value.	However, in coms cases, it is useful to immediately
	 * release the resources used by the current parameter values; this
	 * can be done by calling clearParameters
	 *
	 * @exception SQLException if a database access error occurs
	 */
	public void clearParameters() throws SQLException
	{
		int i;

		for (i = 0 ; i < m_binds.length ; i++)
		{
			m_binds[i] = null;
			m_bindTypes[i] = null;
		}
	}

	/*
	 * Set the value of a parameter using an object; use the java.lang
	 * equivalent objects for integral values.
	 *
	 * <P>The given Java object will be converted to the targetSqlType before
	 * being sent to the database.
	 *
	 * <P>note that this method may be used to pass database-specific
	 * abstract data types.  This is done by using a Driver-specific
	 * Java type and using a targetSqlType of java.sql.Types.OTHER
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the object containing the input parameter value
	 * @param targetSqlType The SQL type to be send to the database
	 * @param scale For java.sql.Types.DECIMAL or java.sql.Types.NUMERIC
	 *		 *	types this is the number of digits after the decimal.  For
	 *		 *	all other types this value will be ignored.
	 * @exception SQLException if a database access error occurs
	 */
	public void setObject(int parameterIndex, Object x, int targetSqlType, int scale) throws SQLException
	{
		if (x == null)
		{
			setNull(parameterIndex, targetSqlType);
			return ;
		}
		switch (targetSqlType)
		{
			case Types.INTEGER:
				bind(parameterIndex, x.toString(), PG_INTEGER);
				break;
			case Types.TINYINT:
			case Types.SMALLINT:
			case Types.BIGINT:
			case Types.REAL:
			case Types.FLOAT:
			case Types.DOUBLE:
			case Types.DECIMAL:
			case Types.NUMERIC:
				if (x instanceof Boolean)
					bind(parameterIndex, ((Boolean)x).booleanValue() ? "1" : "0", PG_BOOLEAN);
				else
					bind(parameterIndex, x.toString(), PG_NUMERIC);
				break;
			case Types.CHAR:
			case Types.VARCHAR:
			case Types.LONGVARCHAR:
				setString(parameterIndex, x.toString());
				break;
			case Types.DATE:
				setDate(parameterIndex, (java.sql.Date)x);
				break;
			case Types.TIME:
				setTime(parameterIndex, (Time)x);
				break;
			case Types.TIMESTAMP:
				setTimestamp(parameterIndex, (Timestamp)x);
				break;
			case Types.BIT:
				if (x instanceof Boolean)
				{
					bind(parameterIndex, ((Boolean)x).booleanValue() ? "TRUE" : "FALSE", PG_TEXT);
				}
				else
				{
					throw new PSQLException("postgresql.prep.type");
				}
				break;
			case Types.BINARY:
			case Types.VARBINARY:
			case Types.LONGVARBINARY:
				setObject(parameterIndex, x);
				break;
			case Types.OTHER:
				setString(parameterIndex, ((PGobject)x).getValue(), PG_TEXT);
				break;
			default:
				throw new PSQLException("postgresql.prep.type");
		}
	}

	public void setObject(int parameterIndex, Object x, int targetSqlType) throws SQLException
	{
		setObject(parameterIndex, x, targetSqlType, 0);
	}

	/*
	 * This stores an Object into a parameter.
	 */
	public void setObject(int parameterIndex, Object x) throws SQLException
	{
		if (x == null)
		{
			setNull(parameterIndex, Types.OTHER);
			return ;
		}
		if (x instanceof String)
			setString(parameterIndex, (String)x);
		else if (x instanceof BigDecimal)
			setBigDecimal(parameterIndex, (BigDecimal)x);
		else if (x instanceof Short)
			setShort(parameterIndex, ((Short)x).shortValue());
		else if (x instanceof Integer)
			setInt(parameterIndex, ((Integer)x).intValue());
		else if (x instanceof Long)
			setLong(parameterIndex, ((Long)x).longValue());
		else if (x instanceof Float)
			setFloat(parameterIndex, ((Float)x).floatValue());
		else if (x instanceof Double)
			setDouble(parameterIndex, ((Double)x).doubleValue());
		else if (x instanceof byte[])
			setBytes(parameterIndex, (byte[])x);
		else if (x instanceof java.sql.Date)
			setDate(parameterIndex, (java.sql.Date)x);
		else if (x instanceof Time)
			setTime(parameterIndex, (Time)x);
		else if (x instanceof Timestamp)
			setTimestamp(parameterIndex, (Timestamp)x);
		else if (x instanceof Boolean)
			setBoolean(parameterIndex, ((Boolean)x).booleanValue());
		else if (x instanceof PGobject)
			setString(parameterIndex, ((PGobject)x).getValue(), PG_TEXT);
		else
			// Try to store as a string in database
			setString(parameterIndex, x.toString(), PG_TEXT);
	}

	/*
	 * Before executing a stored procedure call you must explicitly
	 * call registerOutParameter to register the java.sql.Type of each
	 * out parameter.
	 *
	 * <p>Note: When reading the value of an out parameter, you must use
	 * the getXXX method whose Java type XXX corresponds to the
	 * parameter's registered SQL type.
	 *
	 * ONLY 1 RETURN PARAMETER if {?= call ..} syntax is used
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @param sqlType SQL type code defined by java.sql.Types; for
	 * parameters of type Numeric or Decimal use the version of
	 * registerOutParameter that accepts a scale value
	 * @exception SQLException if a database-access error occurs.
	 */
	public void registerOutParameter(int parameterIndex, int sqlType) throws SQLException
	{
		if (parameterIndex != 1)
			throw new PSQLException ("postgresql.call.noinout");
		if (!isFunction)
			throw new PSQLException ("postgresql.call.procasfunc", originalSql);

		// functionReturnType contains the user supplied value to check
		// testReturn contains a modified version to make it easier to
		// check the getXXX methods..
		functionReturnType = sqlType;
		testReturn = sqlType;
		if (functionReturnType == Types.CHAR ||
				functionReturnType == Types.LONGVARCHAR)
			testReturn = Types.VARCHAR;
		else if (functionReturnType == Types.FLOAT)
			testReturn = Types.REAL; // changes to streamline later error checking
		returnTypeSet = true;
	}

	/*
	 * You must also specify the scale for numeric/decimal types:
	 *
	 * <p>Note: When reading the value of an out parameter, you must use
	 * the getXXX method whose Java type XXX corresponds to the
	 * parameter's registered SQL type.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @param sqlType use either java.sql.Type.NUMERIC or java.sql.Type.DECIMAL
	 * @param scale a value greater than or equal to zero representing the
	 * desired number of digits to the right of the decimal point
	 * @exception SQLException if a database-access error occurs.
	 */
	public void registerOutParameter(int parameterIndex, int sqlType,
									 int scale) throws SQLException
	{
		registerOutParameter (parameterIndex, sqlType); // ignore for now..
	}

	/*
	 * An OUT parameter may have the value of SQL NULL; wasNull
	 * reports whether the last value read has this special value.
	 *
	 * <p>Note: You must first call getXXX on a parameter to read its
	 * value and then call wasNull() to see if the value was SQL NULL.
	 * @return true if the last parameter read was SQL NULL
	 * @exception SQLException if a database-access error occurs.
	 */
	public boolean wasNull() throws SQLException
	{
		// check to see if the last access threw an exception
		return (callResult == null);
	}

	/*
	 * Get the value of a CHAR, VARCHAR, or LONGVARCHAR parameter as a
	 * Java String.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public String getString(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.VARCHAR, "String");
		return (String)callResult;
	}


	/*
	 * Get the value of a BIT parameter as a Java boolean.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is false
	 * @exception SQLException if a database-access error occurs.
	 */
	public boolean getBoolean(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.BIT, "Boolean");
		if (callResult == null)
			return false;
		return ((Boolean)callResult).booleanValue ();
	}

	/*
	 * Get the value of a TINYINT parameter as a Java byte.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public byte getByte(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.TINYINT, "Byte");
		if (callResult == null)
			return 0;
		return (byte)((Integer)callResult).intValue ();
	}

	/*
	 * Get the value of a SMALLINT parameter as a Java short.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public short getShort(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.SMALLINT, "Short");
		if (callResult == null)
			return 0;
		return (short)((Integer)callResult).intValue ();
	}


	/*
	 * Get the value of an INTEGER parameter as a Java int.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public int getInt(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.INTEGER, "Int");
		if (callResult == null)
			return 0;
		return ((Integer)callResult).intValue ();
	}

	/*
	 * Get the value of a BIGINT parameter as a Java long.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public long getLong(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.BIGINT, "Long");
		if (callResult == null)
			return 0;
		return ((Long)callResult).longValue ();
	}

	/*
	 * Get the value of a FLOAT parameter as a Java float.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public float getFloat(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.REAL, "Float");
		if (callResult == null)
			return 0;
		return ((Float)callResult).floatValue ();
	}

	/*
	 * Get the value of a DOUBLE parameter as a Java double.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public double getDouble(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.DOUBLE, "Double");
		if (callResult == null)
			return 0;
		return ((Double)callResult).doubleValue ();
	}

	/*
	 * Get the value of a NUMERIC parameter as a java.math.BigDecimal
	 * object.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @param scale a value greater than or equal to zero representing the
	 * desired number of digits to the right of the decimal point
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 * @deprecated in Java2.0
	 */
	public BigDecimal getBigDecimal(int parameterIndex, int scale)
	throws SQLException
	{
		checkIndex (parameterIndex, Types.NUMERIC, "BigDecimal");
		return ((BigDecimal)callResult);
	}

	/*
	 * Get the value of a SQL BINARY or VARBINARY parameter as a Java
	 * byte[]
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public byte[] getBytes(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.VARBINARY, Types.BINARY, "Bytes");
		return ((byte [])callResult);
	}


	/*
	 * Get the value of a SQL DATE parameter as a java.sql.Date object
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public java.sql.Date getDate(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.DATE, "Date");
		return (java.sql.Date)callResult;
	}

	/*
	 * Get the value of a SQL TIME parameter as a java.sql.Time object.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public java.sql.Time getTime(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.TIME, "Time");
		return (java.sql.Time)callResult;
	}

	/*
	 * Get the value of a SQL TIMESTAMP parameter as a java.sql.Timestamp object.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public java.sql.Timestamp getTimestamp(int parameterIndex)
	throws SQLException
	{
		checkIndex (parameterIndex, Types.TIMESTAMP, "Timestamp");
		return (java.sql.Timestamp)callResult;
	}

	// getObject returns a Java object for the parameter.
	// See the JDBC spec's "Dynamic Programming" chapter for details.
	/*
	 * Get the value of a parameter as a Java object.
	 *
	 * <p>This method returns a Java object whose type coresponds to the
	 * SQL type that was registered for this parameter using
	 * registerOutParameter.
	 *
	 * <P>Note that this method may be used to read datatabase-specific,
	 * abstract data types. This is done by specifying a targetSqlType
	 * of java.sql.types.OTHER, which allows the driver to return a
	 * database-specific Java type.
	 *
	 * <p>See the JDBC spec's "Dynamic Programming" chapter for details.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return A java.lang.Object holding the OUT parameter value.
	 * @exception SQLException if a database-access error occurs.
	 */
	public Object getObject(int parameterIndex)
	throws SQLException
	{
		checkIndex (parameterIndex);
		return callResult;
	}

	//This method is implemeted in jdbc2
	public int getResultSetConcurrency() throws SQLException
	{
		return 0;
	}

	/*
	 * Returns the SQL statement with the current template values
	 * substituted.
	 */
	public String toString()
	{
		if (m_sqlFragments == null)
			return super.toString();

		synchronized (sbuf)
		{
			sbuf.setLength(0);
			int i;

			for (i = 0 ; i < m_binds.length ; ++i)
			{
				sbuf.append (m_sqlFragments[i]);
				if (m_binds[i] == null)
					sbuf.append( '?' );
				else
					sbuf.append (m_binds[i]);
			}
			sbuf.append(m_sqlFragments[m_binds.length]);
			return sbuf.toString();
		}
	}

	/*
	 * There are a lot of setXXX classes which all basically do
	 * the same thing.	We need a method which actually does the
	 * set for us.
	 *
	 * @param paramIndex the index into the inString
	 * @param s a string to be stored
	 * @exception SQLException if something goes wrong
	 */
	protected void bind(int paramIndex, Object s, String type) throws SQLException
	{
		if (paramIndex < 1 || paramIndex > m_binds.length)
			throw new PSQLException("postgresql.prep.range");
		if (paramIndex == 1 && isFunction) // need to registerOut instead
			throw new PSQLException ("postgresql.call.funcover");
		m_binds[paramIndex - 1] = s;
		m_bindTypes[paramIndex - 1] = type;
	}

	/**
	 * this method will turn a string of the form
	 * {? = call <some_function> (?, [?,..]) }
	 * into the PostgreSQL format which is
	 * select <some_function> (?, [?, ...]) as result
	 * or select * from <some_function> (?, [?, ...]) as result (7.3)
	 *
	 */
	private String modifyJdbcCall(String p_sql) throws SQLException
	{
		//Check that this is actually a call which should start with a {
        //if not do nothing and treat this as a standard prepared sql
		if (!p_sql.trim().startsWith("{")) {
			return p_sql;
		}

		// syntax checking is not complete only a few basics :(
		originalSql = p_sql; // save for error msgs..
		String l_sql = p_sql;
		int index = l_sql.indexOf ("="); // is implied func or proc?
		boolean isValid = true;
		if (index > -1)
		{
			isFunction = true;
			isValid = l_sql.indexOf ("?") < index; // ? before =
		}
		l_sql = l_sql.trim ();
		if (l_sql.startsWith ("{") && l_sql.endsWith ("}"))
		{
			l_sql = l_sql.substring (1, l_sql.length() - 1);
		}
		else
			isValid = false;
		index = l_sql.indexOf ("call");
		if (index == -1 || !isValid)
			throw new PSQLException ("postgresql.call.malformed",
									 new Object[]{l_sql, JDBC_SYNTAX});
		l_sql = l_sql.replace ('{', ' '); // replace these characters
		l_sql = l_sql.replace ('}', ' ');
		l_sql = l_sql.replace (';', ' ');

		// this removes the 'call' string and also puts a hidden '?'
		// at the front of the line for functions, this will
		// allow the registerOutParameter to work correctly
		// because in the source sql there was one more ? for the return
		// value that is not needed by the postgres syntax.  But to make
		// sure that the parameter numbers are the same as in the original
		// sql we add a dummy parameter in this case
		l_sql = (isFunction ? "?" : "") + l_sql.substring (index + 4);
		if (connection.haveMinimumServerVersion("7.3")) {
			l_sql = "select * from " + l_sql + " as " + RESULT_ALIAS + ";";
		} else {
			l_sql = "select " + l_sql + " as " + RESULT_ALIAS + ";";
		}
		return l_sql;
	}

	/** helperfunction for the getXXX calls to check isFunction and index == 1
	 * Compare BOTH type fields against the return type.
	 */
	protected void checkIndex (int parameterIndex, int type1, int type2, String getName)
	throws SQLException
	{
		checkIndex (parameterIndex);		
		if (type1 != this.testReturn && type2 != this.testReturn)
			throw new PSQLException("postgresql.call.wrongget",
						new Object[]{"java.sql.Types=" + testReturn,
							     getName,
							     "java.sql.Types=" + type1});
	}

	/** helperfunction for the getXXX calls to check isFunction and index == 1
	 */
	protected void checkIndex (int parameterIndex, int type, String getName)
	throws SQLException
	{
		checkIndex (parameterIndex);
		if (type != this.testReturn)
			throw new PSQLException("postgresql.call.wrongget",
						new Object[]{"java.sql.Types=" + testReturn,
							     getName,
							     "java.sql.Types=" + type});
	}

	/** helperfunction for the getXXX calls to check isFunction and index == 1
	 * @param parameterIndex index of getXXX (index)
	 * check to make sure is a function and index == 1
	 */
	private void checkIndex (int parameterIndex) throws SQLException
	{
		if (!isFunction)
			throw new PSQLException("postgresql.call.noreturntype");
		if (parameterIndex != 1)
			throw new PSQLException("postgresql.call.noinout");
	}



    public void setUseServerPrepare(boolean flag) throws SQLException {
        //Server side prepared statements were introduced in 7.3
        if (connection.haveMinimumServerVersion("7.3")) {
			//If turning server prepared statements off deallocate statement
			//and reset statement name
			if (m_useServerPrepare != flag && !flag)
				connection.execSQL("DEALLOCATE " + m_statementName);
			m_statementName = null;
			m_useServerPrepare = flag;
		} else {
			//This is a pre 7.3 server so no op this method
			//which means we will never turn on the flag to use server
			//prepared statements and thus regular processing will continue
		}
	}

	public boolean isUseServerPrepare()
	{
		return m_useServerPrepare;
	}


	private static final String PG_TEXT = "text";
	private static final String PG_INTEGER = "integer";
	private static final String PG_INT2 = "int2";
	private static final String PG_INT8 = "int8";
	private static final String PG_NUMERIC = "numeric";
	private static final String PG_FLOAT = "float";
	private static final String PG_DOUBLE = "double precision";
	private static final String PG_BOOLEAN = "boolean";
	private static final String PG_DATE = "date";
	private static final String PG_TIME = "time";
	private static final String PG_TIMESTAMPTZ = "timestamptz";
    private static final String PG_BYTEA = "bytea";


}
