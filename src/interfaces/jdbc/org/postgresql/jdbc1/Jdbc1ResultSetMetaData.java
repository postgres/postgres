package org.postgresql.jdbc1;

import java.util.Vector;
import org.postgresql.core.Field;

public class Jdbc1ResultSetMetaData extends AbstractJdbc1ResultSetMetaData implements java.sql.ResultSetMetaData
{
	public Jdbc1ResultSetMetaData(Vector rows, Field[] fields)
	{
		super(rows, fields);
	}

}

