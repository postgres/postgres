/*-------------------------------------------------------------------------
 *
 * PGline.java
 *     It maps to the point datatype in org.postgresql.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/geometric/Attic/PGpoint.java,v 1.6 2003/09/13 04:02:15 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.geometric;

import org.postgresql.util.PGobject;
import org.postgresql.util.PGtokenizer;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

import java.awt.Point;
import java.io.Serializable;
import java.sql.SQLException;

/*
 * This implements a version of java.awt.Point, except it uses double
 * to represent the coordinates.
 */
public class PGpoint extends PGobject implements Serializable, Cloneable
{
	/*
	 * The X coordinate of the point
	 */
	public double x;

	/*
	 * The Y coordinate of the point
	 */
	public double y;

	/*
	 * @param x coordinate
	 * @param y coordinate
	 */
	public PGpoint(double x, double y)
	{
		this();
		this.x = x;
		this.y = y;
	}

	/*
	 * This is called mainly from the other geometric types, when a
	 * point is imbeded within their definition.
	 *
	 * @param value Definition of this point in PostgreSQL's syntax
	 */
	public PGpoint(String value) throws SQLException
	{
		this();
		setValue(value);
	}

	/*
	 * Required by the driver
	 */
	public PGpoint()
	{
		setType("point");
	}

	/*
	 * @param s Definition of this point in PostgreSQL's syntax
	 * @exception SQLException on conversion failure
	 */
	public void setValue(String s) throws SQLException
	{
		PGtokenizer t = new PGtokenizer(PGtokenizer.removePara(s), ',');
		try
		{
			x = Double.valueOf(t.getToken(0)).doubleValue();
			y = Double.valueOf(t.getToken(1)).doubleValue();
		}
		catch (NumberFormatException e)
		{
			throw new PSQLException("postgresql.geo.point", PSQLState.DATA_TYPE_MISMATCH, e.toString());
		}
	}

	/*
	 * @param obj Object to compare with
	 * @return true if the two boxes are identical
	 */
	public boolean equals(Object obj)
	{
		if (obj instanceof PGpoint)
		{
			PGpoint p = (PGpoint)obj;
			return x == p.x && y == p.y;
		}
		return false;
	}

	/*
	 * This must be overidden to allow the object to be cloned
	 */
	public Object clone()
	{
		return new PGpoint(x, y);
	}

	/*
	 * @return the PGpoint in the syntax expected by org.postgresql
	 */
	public String getValue()
	{
		return "(" + x + "," + y + ")";
	}

	/*
	 * Translate the point with the supplied amount.
	 * @param x integer amount to add on the x axis
	 * @param y integer amount to add on the y axis
	 */
	public void translate(int x, int y)
	{
		translate((double)x, (double)y);
	}

	/*
	 * Translate the point with the supplied amount.
	 * @param x double amount to add on the x axis
	 * @param y double amount to add on the y axis
	 */
	public void translate(double x, double y)
	{
		this.x += x;
		this.y += y;
	}

	/*
	 * Moves the point to the supplied coordinates.
	 * @param x integer coordinate
	 * @param y integer coordinate
	 */
	public void move(int x, int y)
	{
		setLocation(x, y);
	}

	/*
	 * Moves the point to the supplied coordinates.
	 * @param x double coordinate
	 * @param y double coordinate
	 */
	public void move(double x, double y)
	{
		this.x = x;
		this.y = y;
	}

	/*
	 * Moves the point to the supplied coordinates.
	 * refer to java.awt.Point for description of this
	 * @param x integer coordinate
	 * @param y integer coordinate
	 * @see java.awt.Point
	 */
	public void setLocation(int x, int y)
	{
		move((double)x, (double)y);
	}

	/*
	 * Moves the point to the supplied java.awt.Point
	 * refer to java.awt.Point for description of this
	 * @param p Point to move to
	 * @see java.awt.Point
	 */
	public void setLocation(Point p)
	{
		setLocation(p.x, p.y);
	}

}
