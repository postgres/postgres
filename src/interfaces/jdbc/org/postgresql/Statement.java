package org.postgresql;

import java.sql.*;
import org.postgresql.util.PSQLException;

/*
 * This class defines methods implemented by the two subclasses
 * org.postgresql.jdbc1.Statement and org.postgresql.jdbc2.Statement that are
 * unique to PostgreSQL's JDBC driver.
 *
 */

public abstract class Statement
{

	/** The warnings chain. */
	protected SQLWarning warnings = null;

	/** The current results */
	protected java.sql.ResultSet result = null;

	/** Maximum number of rows to return, 0 = unlimited */
	protected int maxrows = 0;

	/** Timeout (in seconds) for a query (not used) */
	protected int timeout = 0;

	protected boolean escapeProcessing = true;

	// Static variables for parsing SQL when escapeProcessing is true.
	private static final short IN_SQLCODE = 0;
	private static final short IN_STRING = 1;
	private static final short BACKSLASH = 2;
	private static final short ESC_TIMEDATE = 3;

	public Statement()
	{}

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
		return ((org.postgresql.ResultSet) result).getStatusString();
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
		escapeProcessing = enable;
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
		// FIXME: Cancel feature has been available since 6.4. Implement it here!
	}

	/*
	 * Returns the Last inserted/updated oid.  Deprecated in 7.2 because
         * range of OID values is greater than a java signed int.
	 * @deprecated Replaced by getLastOID in 7.2
	 */
	public int getInsertedOID() throws SQLException
	{
		if (result == null)
			return 0;
		return (int)((org.postgresql.ResultSet) result).getLastOID();
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
		return ((org.postgresql.ResultSet) result).getLastOID();
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
		if (result != null && ((org.postgresql.ResultSet) result).reallyResultSet())
			return result;
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
	protected static String escapeSQL(String sql)
	{
		// Since escape codes can only appear in SQL CODE, we keep track
		// of if we enter a string or not.
		StringBuffer newsql = new StringBuffer();
		short state = IN_SQLCODE;

		int i = -1;
		int len = sql.length();
		while (++i < len)
		{
			char c = sql.charAt(i);
			switch (state)
			{
				case IN_SQLCODE:
					if (c == '\'')				  // start of a string?
						state = IN_STRING;
					else if (c == '{')			  // start of an escape code?
						if (i + 1 < len)
						{
							char next = sql.charAt(i + 1);
							if (next == 'd')
							{
								state = ESC_TIMEDATE;
								i++;
								break;
							}
							else if (next == 't')
							{
								state = ESC_TIMEDATE;
								i += (i + 2 < len && sql.charAt(i + 2) == 's') ? 2 : 1;
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
}
