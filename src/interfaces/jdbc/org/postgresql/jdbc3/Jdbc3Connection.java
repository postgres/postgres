package org.postgresql.jdbc3;


import java.sql.SQLException;

/* $PostgreSQL: pgsql/src/interfaces/jdbc/org/postgresql/jdbc3/Jdbc3Connection.java,v 1.6 2003/11/29 19:52:11 pgsql Exp $
 * This class implements the java.sql.Connection interface for JDBC3.
 * However most of the implementation is really done in
 * org.postgresql.jdbc3.AbstractJdbc3Connection or one of it's parents
 */
public class Jdbc3Connection extends org.postgresql.jdbc3.AbstractJdbc3Connection implements java.sql.Connection
{

	public java.sql.Statement createStatement(int resultSetType, int resultSetConcurrency) throws SQLException
	{
		Jdbc3Statement s = new Jdbc3Statement(this);
		s.setResultSetType(resultSetType);
		s.setResultSetConcurrency(resultSetConcurrency);
		return s;
	}


	public java.sql.PreparedStatement prepareStatement(String sql, int resultSetType, int resultSetConcurrency) throws SQLException
	{
		Jdbc3PreparedStatement s = new Jdbc3PreparedStatement(this, sql);
		s.setResultSetType(resultSetType);
		s.setResultSetConcurrency(resultSetConcurrency);
		return s;
	}

	public java.sql.CallableStatement prepareCall(String sql, int resultSetType, int resultSetConcurrency) throws SQLException
	{
		Jdbc3CallableStatement s = new Jdbc3CallableStatement(this, sql);
		s.setResultSetType(resultSetType);
		s.setResultSetConcurrency(resultSetConcurrency);
		return s;
	}

	public java.sql.DatabaseMetaData getMetaData() throws SQLException
	{
		if (metadata == null)
			metadata = new Jdbc3DatabaseMetaData(this);
		return metadata;
	}

}
