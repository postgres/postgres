package org.postgresql.jdbc2;

import java.util.Vector;
import org.postgresql.core.Field;

public class Jdbc2ResultSetMetaData extends AbstractJdbc2ResultSetMetaData implements java.sql.ResultSetMetaData
{
	public Jdbc2ResultSetMetaData(Vector rows, Field[] fields)
	{
		super(rows, fields);
	}
}

