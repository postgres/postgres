package org.postgresql.jdbc2;


import org.postgresql.core.QueryExecutor;
import org.postgresql.core.BaseStatement;
import org.postgresql.PGRefCursorResultSet;


/** A real result set based on a ref cursor.
 *
 * @author Nic Ferrier <nferrier@tapsellferrier.co.uk>
 */
public class Jdbc2RefCursorResultSet extends Jdbc2ResultSet
        implements PGRefCursorResultSet
{
        
        String refCursorHandle;

        // Indicates when the result set has activaly bound to the cursor.
        boolean isInitialized = false;

        Jdbc2RefCursorResultSet(BaseStatement statement, String refCursorName) throws java.sql.SQLException
        {
                super(statement, null, null, null, -1, 0L);
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
