package uk.org.retep.dtu;

import java.util.Collection;
import java.util.HashMap;
import java.util.Iterator;

public class DEnvironment
{
  protected HashMap dsrc;

  public DEnvironment()
  {
    dsrc=new HashMap();
  }

  public void addDataSource(String aKey,Object aObject)
  {
    dsrc.put(aKey,aObject);
  }

  public Object getDataSource(String aKey)
  {
    return dsrc.get(aKey);
  }

  public Iterator getDataSources()
  {
    return dsrc.values().iterator();
  }
}