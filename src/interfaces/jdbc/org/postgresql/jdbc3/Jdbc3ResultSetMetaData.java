package org.postgresql.jdbc3;

public class Jdbc3ResultSetMetaData extends org.postgresql.jdbc2.AbstractJdbc2ResultSetMetaData implements java.sql.ResultSetMetaData
{

	public Jdbc3ResultSetMetaData(java.util.Vector rows, org.postgresql.Field[] fields)
	{
		super(rows, fields);
	}

}

