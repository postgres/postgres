package org.postgresql.jdbc2;


public class Jdbc2Blob extends AbstractJdbc2Blob implements java.sql.Blob
{

	public Jdbc2Blob(org.postgresql.PGConnection conn, int oid) throws java.sql.SQLException
	{
		super(conn, oid);
	}

}
