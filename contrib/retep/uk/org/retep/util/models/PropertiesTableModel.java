package uk.org.retep.util.models;

import uk.org.retep.util.Logger;
import uk.org.retep.util.misc.PropertiesIO;

import java.io.*;
import java.util.*;
import javax.swing.table.*;

import java.text.*;

/**
 * A TableModel that shows a view of a PropertyFile object
 *
 * $Id: PropertiesTableModel.java,v 1.1 2001/03/05 09:15:37 peter Exp $
 *
 * @author
 * @version 1.0
 */
public class PropertiesTableModel extends AbstractTableModel
{
  // The properties
  protected TreeMap properties;
  protected Properties origProperties;
  protected Object keys[];

  public PropertiesTableModel()
  {
    this(new Properties());
  }

  public PropertiesTableModel(Properties aProperties)
  {
    setProperties(aProperties);
  }

  public synchronized int getKeyRow(Object k)
  {
    for(int i=0;i<keys.length;i++) {
      if(keys[i].equals(k)) {
        return i;
      }
    }
    return -1;
  }

  /**
   * Best not use this one to update, use the put method in this class!
   */
  public Properties getProperties()
  {
    return origProperties;
  }

  public synchronized void put(Object k,Object v)
  {
    properties.put(k,v);
    origProperties.put(k,v);
    refresh();
  }

  public Object get(Object k)
  {
    return origProperties.get(k);
  }

  public synchronized void remove(Object k)
  {
    properties.remove(k);
    origProperties.remove(k);
    refresh();
  }

  public boolean contains(Object o)
  {
    return origProperties.contains(o);
  }

  public boolean containsKey(Object o)
  {
    return origProperties.containsKey(o);
  }

  public boolean containsValue(Object o)
  {
    return origProperties.containsValue(o);
  }

  public void setProperties(Properties aProperties)
  {
    origProperties=aProperties;
    properties = PropertiesIO.getTreeMap(aProperties);
    refresh();
  }

  public void refresh()
  {
    keys = properties.keySet().toArray();
    fireTableDataChanged();
  }

  private static final String cols[] = {
    "Property","Value"
  };

  public int getColumnCount()
  {
    return cols.length;
  }

  public Object getValueAt(int aRow, int aColumn)
  {
    if(aRow<0 || aRow>=keys.length || aColumn<0 || aColumn>=cols.length)
      return null;

    Object key = keys[aRow];

    switch(aColumn)
    {
      case 0:
        return key;

      case 1:
        return properties.get(key);

      default:
        return null;
    }
  }

  public int getRowCount()
  {
    return keys.length;
  }

  public String getColumnName(int aColumn)
  {
    return cols[aColumn];
  }

  public void setValueAt(Object aValue, int aRow, int aColumn)
  {
    if(aRow<0 || aRow>=keys.length || aColumn<0 || aColumn>=cols.length)
      return;

    switch(aColumn)
      {
        // Rename the key (only if not already present). If already present
        // the refresh() will replace with the old one anyhow...
        case 0:
          if(!properties.containsKey(aValue)) {
            Object oldValue = get(keys[aRow]);
            remove(keys[aRow]);
            put(aValue,oldValue);
          }
          refresh();
          break;

        // Update the value...
        case 1:
          put(keys[aRow],aValue);
          //refresh();
          break;

        default:
          // Should never be called
          Logger.log(Logger.ERROR,"PropertiesTableModel: Column range",aColumn);
      }
  }

  public boolean isCellEditable(int aRow, int aColumn)
  {
    return true;
  }

}
