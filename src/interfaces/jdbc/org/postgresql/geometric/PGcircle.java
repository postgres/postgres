/*-------------------------------------------------------------------------
 *
 * PGcircle.java
 *     This represents org.postgresql's circle datatype, consisting of a point
 *     and a radius
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/geometric/Attic/PGcircle.java,v 1.6.2.1 2004/05/07 05:28:22 jurka Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.geometric;

import org.postgresql.util.PGobject;
import org.postgresql.util.PGtokenizer;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

import java.io.Serializable;
import java.sql.SQLException;

public class PGcircle extends PGobject implements Serializable, Cloneable
{
	/*
	 * This is the centre point
	 */
	public PGpoint center;

	/*
	 * This is the radius
	 */
	public double radius;

	/*
	 * @param x coordinate of centre
	 * @param y coordinate of centre
	 * @param r radius of circle
	 */
	public PGcircle(double x, double y, double r)
	{
		this(new PGpoint(x, y), r);
	}

	/*
	 * @param c PGpoint describing the circle's centre
	 * @param r radius of circle
	 */
	public PGcircle(PGpoint c, double r)
	{
		this();
		this.center = c;
		this.radius = r;
	}

	/*
	 * @param s definition of the circle in PostgreSQL's syntax.
	 * @exception SQLException on conversion failure
	 */
	public PGcircle(String s) throws SQLException
	{
		this();
		setValue(s);
	}

	/*
	 * This constructor is used by the driver.
	 */
	public PGcircle()
	{
		setType("circle");
	}

	/*
	 * @param s definition of the circle in PostgreSQL's syntax.
	 * @exception SQLException on conversion failure
	 */
	public void setValue(String s) throws SQLException
	{
		PGtokenizer t = new PGtokenizer(PGtokenizer.removeAngle(s), ',');
		if (t.getSize() != 2)
			throw new PSQLException("postgresql.geo.circle", PSQLState.DATA_TYPE_MISMATCH, s);

		try
		{
			center = new PGpoint(t.getToken(0));
			radius = Double.valueOf(t.getToken(1)).doubleValue();
		}
		catch (NumberFormatException e)
		{
			throw new PSQLException("postgresql.geo.circle", PSQLState.DATA_TYPE_MISMATCH, e);
		}
	}

	/*
	 * @param obj Object to compare with
	 * @return true if the two boxes are identical
	 */
	public boolean equals(Object obj)
	{
		if (obj instanceof PGcircle)
		{
			PGcircle p = (PGcircle)obj;
			return p.center.equals(center) && p.radius == radius;
		}
		return false;
	}

	/*
	 * This must be overidden to allow the object to be cloned
	 */
	public Object clone()
	{
		return new PGcircle((PGpoint)center.clone(), radius);
	}

	/*
	 * @return the PGcircle in the syntax expected by org.postgresql
	 */
	public String getValue()
	{
		return "<" + center + "," + radius + ">";
	}
}
