/*-------------------------------------------------------------------------
 *
 * PGRefCursorResultSet.java
 *	  Describes a PLPGSQL refcursor type.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/interfaces/jdbc/org/postgresql/PGRefCursorResultSet.java,v 1.2 2003/11/29 19:52:09 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql;


/** A ref cursor based result set.
 */
public interface PGRefCursorResultSet
{

        /** return the name of the cursor.
         */
	public String getRefCursor ();

}
