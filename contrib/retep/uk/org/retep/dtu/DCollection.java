package uk.org.retep.dtu;

import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;

import java.io.IOException;
import java.util.Collection;
import java.util.Iterator;

public class DCollection implements Collection
{
  protected int num,max,inc;

  protected DElement elements[];

  public DCollection()
  {
    this(10);
  }

  public DCollection(int aIncrement)
  {
    num=0;
    max=0;
    inc=aIncrement;
    elements=null;
  }

  protected void resize()
  {
    if(num>=max) {
      max+=inc;
      DElement n[] = new DElement[max];
      if(elements!=null) {
        System.arraycopy(elements,0,n,0,elements.length);
      }
      elements=n;
    }
  }

  public int size()
  {
    return num;
  }

  public boolean isEmpty()
  {
    return (num==0);
  }

  /**
   * Checks the list using it's XML id.
   */
  public synchronized boolean contains(Object parm1)
  {
    if(parm1 instanceof DElement) {
      DElement e = (DElement) parm1;
      int ei = e.getID();

      // out of range?
      if(ei<0 || ei>=num)
        return false;

      return elements[ei].equals(e);
    }

    return false;
  }

  public Iterator iterator()
  {
    return new iterator(this);
  }

  /**
   * Inner class to implement an Iterator
   */
  protected class iterator implements Iterator
  {
    protected DCollection c;
    protected int i;

    public iterator(DCollection aCollection)
    {
      c=aCollection;
      i=0;
    }

    public boolean hasNext()
    {
      return i<c.size();
    }

    public Object next() {
      return c.getElement(i++);
    }

    public void remove() {
    }
  }

  public synchronized Object[] toArray()
  {
    Object o[] = new Object[num];
    System.arraycopy(elements,0,o,0,num);
    return o;
  }

  public Object[] toArray(Object[] parm1)
  {
    /**@todo: Implement this java.util.Collection method*/
    throw new java.lang.UnsupportedOperationException("Method toArray() not yet implemented.");
  }

  /**
   * Adds a node to the Collection, and sets it's ID to its position in the Collection
   */
  public synchronized boolean add(Object parm1)
  {
    if(parm1 instanceof DElement) {
      DElement e = (DElement) parm1;

      // Do nothing if it's already in a Collection
      if(e.getID()>-1) {
        return false;
      }

      // Add to the Collection
      resize();
      e.setID(num);
      elements[num++] = e;
      return true;
    }
    return false;
  }

  public synchronized boolean remove(Object parm1)
  {
    if(parm1 instanceof DElement) {
      DElement e = (DElement) parm1;
      int ei = e.getID();
      if(ei<0 || ei>=num)
        return false;

      // Mark the node as parentless
      e.setID(-1);

      // Now remove from the array by moving latter nodes, fixing their ids
      // in the process
      for(int j=ei,k=ei+1;k<num;j++,k++) {
        elements[j]=elements[k];
        elements[j].setID(j);
      }
      num--;
      return true;
    }

    return false;
  }

  public boolean containsAll(Collection parm1)
  {
    /**@todo: Implement this java.util.Collection method*/
    throw new java.lang.UnsupportedOperationException("Method containsAll() not yet implemented.");
  }

  public boolean addAll(Collection parm1)
  {
    /**@todo: Implement this java.util.Collection method*/
    throw new java.lang.UnsupportedOperationException("Method addAll() not yet implemented.");
  }

  public boolean removeAll(Collection parm1)
  {
    /**@todo: Implement this java.util.Collection method*/
    throw new java.lang.UnsupportedOperationException("Method removeAll() not yet implemented.");
  }

  public boolean retainAll(Collection parm1)
  {
    /**@todo: Implement this java.util.Collection method*/
    throw new java.lang.UnsupportedOperationException("Method retainAll() not yet implemented.");
  }

  public synchronized void clear()
  {
    // Mark each node as parentless
    for(int i=0;i<num;i++) {
      elements[i].setID(-1);
    }

    // dispose the array
    num=0;
    max=0;
    elements=null;
  }

  /**
   * Returns the element with supplied id.
   * @return element or null
   */
  public synchronized DElement getElement(int id)
  {
    if(id<0 || id>=num)
      return null;

    return elements[id];
  }

  /**
   * Repairs the collection, ensuring all id's are correct
   */
  public synchronized void repair()
  {
    for(int i=0;i<num;i++) {
      elements[i].setID(i);
    }
  }

  public synchronized void saveXML(XMLFactory aFactory)
  throws IOException, XMLFactoryException
  {
    for(int i=0;i<num;i++) {
      elements[i].saveXML(aFactory);
    }
  }

}