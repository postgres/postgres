/*-------------------------------------------------------------------------
 *
 * PGmoney.java
 *     This implements a class that handles the PostgreSQL money and cash types
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/util/Attic/PGmoney.java,v 1.6 2003/09/13 04:02:16 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.util;


import java.io.Serializable;
import java.sql.SQLException;

public class PGmoney extends PGobject implements Serializable, Cloneable
{
	/*
	 * The value of the field
	 */
	public double val;

	/*
	 * @param value of field
	 */
	public PGmoney(double value)
	{
		this();
		val = value;
	}

	public PGmoney(String value) throws SQLException
	{
		this();
		setValue(value);
	}

	/*
	 * Required by the driver
	 */
	public PGmoney()
	{
		setType("money");
	}

	public void setValue(String s) throws SQLException
	{
		try
		{
			String s1;
			boolean negative;

			negative = (s.charAt(0) == '(') ;

			// Remove any () (for negative) & currency symbol
			s1 = PGtokenizer.removePara(s).substring(1);

			// Strip out any , in currency
			int pos = s1.indexOf(',');
			while (pos != -1)
			{
				s1 = s1.substring(0, pos) + s1.substring(pos + 1);
				pos = s1.indexOf(',');
			}

			val = Double.valueOf(s1).doubleValue();
			val = negative ? -val : val;

		}
		catch (NumberFormatException e)
		{
			throw new PSQLException("postgresql.money", PSQLState.NUMERIC_CONSTANT_OUT_OF_RANGE, e);
		}
	}

	public boolean equals(Object obj)
	{
		if (obj instanceof PGmoney)
		{
			PGmoney p = (PGmoney)obj;
			return val == p.val;
		}
		return false;
	}

	/*
	 * This must be overidden to allow the object to be cloned
	 */
	public Object clone()
	{
		return new PGmoney(val);
	}

	public String getValue()
	{
		if (val < 0)
		{
			return "-$" + ( -val);
		}
		else
		{
			return "$" + val;
		}
	}
}
