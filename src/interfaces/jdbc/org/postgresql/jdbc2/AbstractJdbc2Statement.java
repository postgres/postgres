package org.postgresql.jdbc2;


import java.sql.*;
import java.util.Vector;
import org.postgresql.util.PSQLException;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/AbstractJdbc2Statement.java,v 1.1 2002/07/23 03:59:55 barry Exp $
 * This class defines methods of the jdbc2 specification.  This class extends
 * org.postgresql.jdbc1.AbstractJdbc1Statement which provides the jdbc1
 * methods.  The real Statement class (for jdbc2) is org.postgresql.jdbc2.Jdbc2Statement
 */
public abstract class AbstractJdbc2Statement extends org.postgresql.jdbc1.AbstractJdbc1Statement
{

	protected Vector batch = null;
	protected int resultsettype;		 // the resultset type to return
	protected int concurrency;		 // is it updateable or not?

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
	public boolean execute(String sql) throws SQLException
	{
	        boolean l_return = super.execute(sql);

                //Now do the jdbc2 specific stuff
		//required for ResultSet.getStatement() to work
		((AbstractJdbc2ResultSet)result).setStatement((Jdbc2Statement)this);

		// Added this so that the Updateable resultset knows the query that gave this
		((AbstractJdbc2ResultSet)result).setSQLQuery(sql);

		return l_return;
	}

	// ** JDBC 2 Extensions **

	public void addBatch(String sql) throws SQLException
	{
		if (batch == null)
			batch = new Vector();
		batch.addElement(sql);
	}

	public void clearBatch() throws SQLException
	{
		if (batch != null)
			batch.removeAllElements();
	}

	public int[] executeBatch() throws SQLException
	{
		if (batch == null)
			batch = new Vector();
		int size = batch.size();
		int[] result = new int[size];
		int i = 0;
		try
		{
			for (i = 0;i < size;i++)
				result[i] = this.executeUpdate((String)batch.elementAt(i));
		}
		catch (SQLException e)
		{
			int[] resultSucceeded = new int[i];
			System.arraycopy(result, 0, resultSucceeded, 0, i);

			PBatchUpdateException updex =
				new PBatchUpdateException("postgresql.stat.batch.error",
						          new Integer(i), batch.elementAt(i), resultSucceeded);
			updex.setNextException(e);

			throw updex;
		}
		finally
		{
			batch.removeAllElements();
		}
		return result;
	}

	public void cancel() throws SQLException
	{
		((AbstractJdbc2Connection)connection).cancelQuery();
	}

	public java.sql.Connection getConnection() throws SQLException
	{
		return (java.sql.Connection)connection;
	}

	public int getFetchDirection() throws SQLException
	{
		throw new PSQLException("postgresql.psqlnotimp");
	}

	public int getFetchSize() throws SQLException
	{
		// This one can only return a valid value when were a cursor?
		throw org.postgresql.Driver.notImplemented();
	}

	public int getResultSetConcurrency() throws SQLException
	{
		return concurrency;
	}

	public int getResultSetType() throws SQLException
	{
		return resultsettype;
	}

	public void setFetchDirection(int direction) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public void setFetchSize(int rows) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public void setResultSetConcurrency(int value) throws SQLException
	{
		concurrency = value;
	}

	public void setResultSetType(int value) throws SQLException
	{
		resultsettype = value;
	}

}
