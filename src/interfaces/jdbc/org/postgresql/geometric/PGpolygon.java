/*-------------------------------------------------------------------------
 *
 * PGline.java
 *     This implements the polygon datatype within PostgreSQL.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/geometric/Attic/PGpolygon.java,v 1.5 2003/05/29 04:39:48 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.geometric;

import org.postgresql.util.PGobject;
import org.postgresql.util.PGtokenizer;
import java.io.Serializable;
import java.sql.SQLException;

public class PGpolygon extends PGobject implements Serializable, Cloneable
{
	/*
	 * The points defining the polygon
	 */
	public PGpoint points[];

	/*
	 * Creates a polygon using an array of PGpoints
	 *
	 * @param points the points defining the polygon
	 */
	public PGpolygon(PGpoint[] points)
	{
		this();
		this.points = points;
	}

	/*
	 * @param s definition of the circle in PostgreSQL's syntax.
	 * @exception SQLException on conversion failure
	 */
	public PGpolygon(String s) throws SQLException
	{
		this();
		setValue(s);
	}

	/*
	 * Required by the driver
	 */
	public PGpolygon()
	{
		setType("polygon");
	}

	/*
	 * @param s Definition of the polygon in PostgreSQL's syntax
	 * @exception SQLException on conversion failure
	 */
	public void setValue(String s) throws SQLException
	{
		PGtokenizer t = new PGtokenizer(PGtokenizer.removePara(s), ',');
		int npoints = t.getSize();
		points = new PGpoint[npoints];
		for (int p = 0;p < npoints;p++)
			points[p] = new PGpoint(t.getToken(p));
	}

	/*
	 * @param obj Object to compare with
	 * @return true if the two boxes are identical
	 */
	public boolean equals(Object obj)
	{
		if (obj instanceof PGpolygon)
		{
			PGpolygon p = (PGpolygon)obj;

			if (p.points.length != points.length)
				return false;

			for (int i = 0;i < points.length;i++)
				if (!points[i].equals(p.points[i]))
					return false;

			return true;
		}
		return false;
	}

	/*
	 * This must be overidden to allow the object to be cloned
	 */
	public Object clone()
	{
		PGpoint ary[] = new PGpoint[points.length];
		for (int i = 0;i < points.length;i++)
			ary[i] = (PGpoint)points[i].clone();
		return new PGpolygon(ary);
	}

	/*
	 * @return the PGpolygon in the syntax expected by org.postgresql
	 */
	public String getValue()
	{
		StringBuffer b = new StringBuffer();
		b.append("(");
		for (int p = 0;p < points.length;p++)
		{
			if (p > 0)
				b.append(",");
			b.append(points[p].toString());
		}
		b.append(")");
		return b.toString();
	}
}
