package org.postgresql.jdbc2;


public class Jdbc2Clob extends AbstractJdbc2Clob implements java.sql.Clob
{

	public Jdbc2Clob(org.postgresql.PGConnection conn, int oid) throws java.sql.SQLException
	{
		super(conn, oid);
	}

}
