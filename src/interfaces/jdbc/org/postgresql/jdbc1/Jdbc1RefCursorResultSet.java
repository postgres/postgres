package org.postgresql.jdbc1;

import java.sql.SQLException;
import org.postgresql.core.QueryExecutor;
import org.postgresql.core.BaseStatement;
import org.postgresql.PGRefCursorResultSet;

/** A real result set based on a ref cursor.
 *
 * @author Nic Ferrier <nferrier@tapsellferrier.co.uk>
 */
public class Jdbc1RefCursorResultSet extends Jdbc1ResultSet
        implements PGRefCursorResultSet
{

        // The name of the cursor being used.
        String refCursorHandle;

        // Indicates when the result set has activaly bound to the cursor.
        boolean isInitialized = false;

        
        Jdbc1RefCursorResultSet(BaseStatement statement, String refCursorName)
        {
                super(statement, null, null, null, -1, 0L);
                this.refCursorHandle = refCursorName;
        }

        public String getRefCursor ()
        {
                return refCursorHandle;
        }

        public boolean next () throws SQLException
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
