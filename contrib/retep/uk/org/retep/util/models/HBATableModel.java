package uk.org.retep.util.models;

import uk.org.retep.util.hba.Record;

import java.util.ArrayList;
import java.util.Iterator;
import javax.swing.table.*;

/**
 * A TableModel to display the contents of a pg_hba.conf file
 * @author
 * @version 1.0
 */

public class HBATableModel extends AbstractTableModel
{
  ArrayList list = new ArrayList();

  private static final String cols[] = {
    "Type","Database","IP Address","IP Mask","Authentication","Arguments"
  };


  public HBATableModel()
  {
  }

  public ArrayList getArray()
  {
    return list;
  }

  public int getColumnCount()
  {
    return cols.length;
  }

  public Object getValueAt(int aRow, int aCol)
  {
    Record rec = (Record) list.get(aRow);
    int t;

    switch(aCol)
    {
      case 0:
        t = rec.getType();
        return t<0 ? "ERR" : Record.types[t] ;

      case 1:
        return rec.getDatabase();

      case 2:
        return rec.getIP();

      case 3:
        return rec.getMask();

      case 4:
        t=rec.getAuthType();
        return t<0 ? "ERR" : Record.auths[t] ;

      case 5:
        return rec.getAuthArgs();

      default:
        return "";
    }
  }

  public int getRowCount()
  {
    return list.size();
  }

  public boolean isCellEditable(int rowIndex, int columnIndex)
  {
    /**@todo: Override this javax.swing.table.AbstractTableModel method*/
    return super.isCellEditable( rowIndex,  columnIndex);
  }

  public String getColumnName(int aColumn)
  {
    return cols[aColumn];
  }

  public void setValueAt(Object aValue, int rowIndex, int columnIndex)
  {
    /**@todo: Override this javax.swing.table.AbstractTableModel method*/
    super.setValueAt( aValue,  rowIndex,  columnIndex);
  }
}