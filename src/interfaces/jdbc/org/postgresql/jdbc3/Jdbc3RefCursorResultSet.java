package org.postgresql.jdbc3;

import org.postgresql.PGRefCursorResultSet;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;
import org.postgresql.core.QueryExecutor;
import java.util.Vector;

/** A real result set based on a ref cursor.
 *
 * @author Nic Ferrier <nferrier@tapsellferrier.co.uk>
 */
public class Jdbc3RefCursorResultSet extends Jdbc3ResultSet implements PGRefCursorResultSet
{

	String refCursorHandle;

	// Indicates when the result set has activaly bound to the cursor.
	boolean isInitialized = false;

	Jdbc3RefCursorResultSet(java.sql.Statement statement, String refCursorName) throws java.sql.SQLException
	{
                // This casting is a GCJ requirement.
                super((BaseStatement)statement,
                      (Field[])null,
                      (Vector)null,
                      (String)null, -1, 0L);
                this.refCursorHandle = refCursorName;
	}

	public String getRefCursor ()
	{
		return refCursorHandle;
	}

	public boolean next () throws java.sql.SQLException
	{
		if (isInitialized)
			return super.next();
		// Initialize this res set with the rows from the cursor.
		String[] toExec = { "FETCH ALL IN \"" + refCursorHandle + "\";" };
                QueryExecutor.execute(toExec, new String[0], this);
		isInitialized = true;
		return super.next();
	}
}
