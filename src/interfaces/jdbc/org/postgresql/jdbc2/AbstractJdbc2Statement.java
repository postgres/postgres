package org.postgresql.jdbc2;


import java.io.*;
import java.math.*;
import java.sql.*;
import java.util.Vector;
import org.postgresql.Driver;
import org.postgresql.largeobject.*;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/AbstractJdbc2Statement.java,v 1.17 2003/09/09 10:49:16 barry Exp $
 * This class defines methods of the jdbc2 specification.  This class extends
 * org.postgresql.jdbc1.AbstractJdbc1Statement which provides the jdbc1
 * methods.  The real Statement class (for jdbc2) is org.postgresql.jdbc2.Jdbc2Statement
 */
public abstract class AbstractJdbc2Statement extends org.postgresql.jdbc1.AbstractJdbc1Statement
{

	protected Vector batch = null;
	protected int resultsettype;		 // the resultset type to return
	protected int concurrency;		 // is it updateable or not?

	public AbstractJdbc2Statement (AbstractJdbc2Connection c)
	{
		super(c);
		resultsettype = ResultSet.TYPE_SCROLL_INSENSITIVE;
		concurrency = ResultSet.CONCUR_READ_ONLY;
	}

	public AbstractJdbc2Statement(AbstractJdbc2Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
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
	public boolean execute() throws SQLException
	{
		boolean l_return = super.execute();
		//Now do the jdbc2 specific stuff
		//required for ResultSet.getStatement() to work and updateable resultsets
		result.setStatement(this);

		return l_return;
	}

	// ** JDBC 2 Extensions **

	public void addBatch(String p_sql) throws SQLException
	{
		if (batch == null)
			batch = new Vector();
        Object[] l_statement = new Object[] {new String[] {p_sql}, new Object[0], new String[0]};
		batch.addElement(l_statement);
	}

	public void clearBatch() throws SQLException
	{
		batch = null;
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
            //copy current state of statement
			String[] l_origSqlFragments = m_sqlFragments;
			Object[] l_origBinds = m_binds;
			String[] l_origBindTypes = m_bindTypes;

			for (i = 0;i < size;i++) {
				//set state from batch
				Object[] l_statement = (Object[])batch.elementAt(i);
				this.m_sqlFragments = (String[])l_statement[0];
				this.m_binds = (Object[])l_statement[1];
				this.m_bindTypes = (String[])l_statement[2];
				result[i] = this.executeUpdate();
			}

			//restore state of statement
			m_sqlFragments = l_origSqlFragments;
			m_binds = l_origBinds;
			m_bindTypes = l_origBindTypes;

		}
		catch (SQLException e)
		{
			int[] resultSucceeded = new int[i];
			System.arraycopy(result, 0, resultSucceeded, 0, i);

			PBatchUpdateException updex =
				new PBatchUpdateException("postgresql.stat.batch.error",
										  new Integer(i), m_sqlFragments[0], resultSucceeded);
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
		connection.cancelQuery();
	}

	public Connection getConnection() throws SQLException
	{
		return (Connection) connection;
	}

	public int getFetchDirection() throws SQLException
	{
		throw new PSQLException("postgresql.psqlnotimp", PSQLState.NOT_IMPLEMENTED);
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
		// I don't think this should happen, since it's a hint it should just
		// fail quietly.
		//   throw Driver.notImplemented();
	}

	public void setFetchSize(int rows) throws SQLException
	{
		if (rows<0) throw new PSQLException("postgresql.input.fetch.gt0");
		super.fetchSize = rows;
	}

	public void setResultSetConcurrency(int value) throws SQLException
	{
		concurrency = value;
	}

	public void setResultSetType(int value) throws SQLException
	{
		resultsettype = value;
	}

	public void addBatch() throws SQLException
	{
		if (batch == null)
			batch = new Vector();

		//we need to create copies, otherwise the values can be changed
		Object[] l_newSqlFragments = null;
		if (m_sqlFragments != null) { 
			l_newSqlFragments = new String[m_sqlFragments.length];
			System.arraycopy(m_sqlFragments,0,l_newSqlFragments,0,m_sqlFragments.length);
		}
		Object[] l_newBinds = new Object[m_binds.length];
        System.arraycopy(m_binds,0,l_newBinds,0,m_binds.length);
		String[] l_newBindTypes = new String[m_bindTypes.length];
        System.arraycopy(m_bindTypes,0,l_newBindTypes,0,m_bindTypes.length);
        Object[] l_statement = new Object[] {l_newSqlFragments, l_newBinds, l_newBindTypes};
		batch.addElement(l_statement);
	}

	public ResultSetMetaData getMetaData() throws SQLException
	{
		ResultSet rs = getResultSet();
		if (rs != null)
			return rs.getMetaData();

		// Does anyone really know what this method does?
		return null;
	}

	public void setArray(int i, java.sql.Array x) throws SQLException
	{
		setString(i, x.toString());
	}

	public void setBlob(int i, Blob x) throws SQLException
	{
		InputStream l_inStream = x.getBinaryStream();
		LargeObjectManager lom = connection.getLargeObjectAPI();
		int oid = lom.create();
		LargeObject lob = lom.open(oid);
		OutputStream los = lob.getOutputStream();
		byte[] buf = new byte[4096];
		try
		{
			// could be buffered, but then the OutputStream returned by LargeObject
			// is buffered internally anyhow, so there would be no performance
			// boost gained, if anything it would be worse!
			int bytesRemaining = (int)x.length();
			int numRead = l_inStream.read(buf, 0, Math.min(buf.length, bytesRemaining));
			while (numRead != -1 && bytesRemaining > 0)
			{
				bytesRemaining -= numRead;
				if ( numRead == buf.length )
					los.write(buf); // saves a buffer creation and copy in LargeObject since it's full
				else
					los.write(buf,0,numRead);
				numRead = l_inStream.read(buf, 0, Math.min(buf.length, bytesRemaining));
			}
		}
		catch (IOException se)
		{
			throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, se);
		}
		finally
		{
			try
			{
				los.close();
                l_inStream.close();
            }
            catch( Exception e ) {}
		}
		setInt(i, oid);
	}

	public void setCharacterStream(int i, java.io.Reader x, int length) throws SQLException
	{
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports CharacterStream for for the PG text types
			//As the spec/javadoc for this method indicate this is to be used for
			//large text values (i.e. LONGVARCHAR)	PG doesn't have a separate
			//long varchar datatype, but with toast all the text datatypes are capable of
			//handling very large values.  Thus the implementation ends up calling
			//setString() since there is no current way to stream the value to the server
			char[] l_chars = new char[length];
			int l_charsRead;
			try
			{
				l_charsRead = x.read(l_chars, 0, length);
			}
			catch (IOException l_ioe)
			{
				throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, l_ioe);
			}
			setString(i, new String(l_chars, 0, l_charsRead));
		}
		else
		{
			//Version 7.1 only supported streams for LargeObjects
			//but the jdbc spec indicates that streams should be
			//available for LONGVARCHAR instead
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
				throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, se);
			}
			// lob is closed by the stream so don't call lob.close()
			setInt(i, oid);
		}
	}

	public void setClob(int i, Clob x) throws SQLException
	{
		InputStream l_inStream = x.getAsciiStream();
		int l_length = (int) x.length();
		LargeObjectManager lom = connection.getLargeObjectAPI();
		int oid = lom.create();
		LargeObject lob = lom.open(oid);
		OutputStream los = lob.getOutputStream();
		try
		{
			// could be buffered, but then the OutputStream returned by LargeObject
			// is buffered internally anyhow, so there would be no performance
			// boost gained, if anything it would be worse!
			int c = l_inStream.read();
			int p = 0;
			while (c > -1 && p < l_length)
			{
				los.write(c);
				c = l_inStream.read();
				p++;
			}
			los.close();
		}
		catch (IOException se)
		{
			throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, se);
		}
		// lob is closed by the stream so don't call lob.close()
		setInt(i, oid);
	}

	public void setNull(int i, int t, String s) throws SQLException
	{
		setNull(i, t);
	}

	public void setRef(int i, Ref x) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public void setDate(int i, java.sql.Date d, java.util.Calendar cal) throws SQLException
	{
		if (cal == null)
			setDate(i, d);
		else
		{
			cal.setTime(d);
			setDate(i, new java.sql.Date(cal.getTime().getTime()));
		}
	}

	public void setTime(int i, Time t, java.util.Calendar cal) throws SQLException
	{
		if (cal == null)
			setTime(i, t);
		else
		{
			cal.setTime(t);
			setTime(i, new java.sql.Time(cal.getTime().getTime()));
		}
	}

	public void setTimestamp(int i, Timestamp t, java.util.Calendar cal) throws SQLException
	{
		if (cal == null)
			setTimestamp(i, t);
		else
		{
			cal.setTime(t);
			setTimestamp(i, new java.sql.Timestamp(cal.getTime().getTime()));
		}
	}

	// ** JDBC 2 Extensions for CallableStatement**

	public java.sql.Array getArray(int i) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public java.math.BigDecimal getBigDecimal(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.NUMERIC, "BigDecimal");
		return ((BigDecimal)callResult);
	}

	public Blob getBlob(int i) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public Clob getClob(int i) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public Object getObject(int i, java.util.Map map) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public Ref getRef(int i) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public java.sql.Date getDate(int i, java.util.Calendar cal) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public Time getTime(int i, java.util.Calendar cal) throws SQLException
	{
		throw Driver.notImplemented();
	}

	public Timestamp getTimestamp(int i, java.util.Calendar cal) throws SQLException
	{
		throw Driver.notImplemented();
	}

	// no custom types allowed yet..
	public void registerOutParameter(int parameterIndex, int sqlType, String typeName) throws SQLException
	{
		throw Driver.notImplemented();
	}


	//This is needed by AbstractJdbc2ResultSet to determine if the query is updateable or not
	protected String[] getSqlFragments()
	{
		return m_sqlFragments;
	}

}
