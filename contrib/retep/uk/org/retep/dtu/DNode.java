package uk.org.retep.dtu;

import uk.org.retep.util.Logger;
import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;

import java.io.IOException;
import java.io.Serializable;
import java.util.Iterator;

/**
 * This is the base class for all nodes.
 */
public class DNode implements DElement, Serializable
{
  // The id of this node
  protected int id;

  // The type of this node
  protected int type;

  protected int x,y;

  public static final int OK      = 0;  // Node last ran fine
  public static final int ERROR   = 1;  // Node failed on last run

  /**
   * This type of node does nothing
   */
  public static int NOP   = 0; // No action

  public DNode()
  {
    this(NOP);
  }

  public DNode(int aType)
  {
    id=-1;
    type=aType;

    // Init the transform linkage
    mf=mt=5;
    nf=nt=0;
    fn = new DTransform[mf];
    tn = new DTransform[mt];

    Logger.log(Logger.DEBUG,"new DNode");
  }

  public int getID()
  {
    return id;
  }

  public void setID(int aID)
  {
    id=aID;
    Logger.log(Logger.DEBUG,"DNode.setID",aID);
  }

  public int getType()
  {
    return type;
  }

  public void setType(int aType)
  {
    type=aType;
    Logger.log(Logger.DEBUG,"DNode.setType",aType);
  }

  /**
   */
  public void saveXML(XMLFactory aFactory)
  throws IOException, XMLFactoryException
  {
    Logger.log(Logger.DEBUG,"DNode.saveXML start",this);
    Iterator it;

    aFactory.startTag(DConstants.XML_NODE);
    aFactory.addAttribute(DConstants.XML_ID,new Integer(getID()));
    aFactory.addAttribute(DConstants.XML_TYPE,new Integer(getType()));

    // used for display only
    aFactory.addAttribute(DConstants.XML_X,new Integer(getX()));
    aFactory.addAttribute(DConstants.XML_Y,new Integer(getY()));

    // Save the transforms here (only the from list required)
    for(int i=0;i<nf;i++) {
      fn[i].saveXML(aFactory);
    }

    aFactory.endTag(); // NODE
    Logger.log(Logger.DEBUG,"DNode.saveXML finish",this);
  }

  public void setPosition(int aX,int aY)
  {
    x=aX;
    y=aY;
  }

  public int getX()
  {
    return x;
  }

  public int getY()
  {
    return y;
  }

  public void setX(int aX)
  {
    x=aX;
  }

  public void setY(int aY)
  {
    y=aY;
  }

  /**
   * This must be overidden to do something
   * @return Return status
   */
  public int run(DEnvironment env)
  {
    return OK;
  }

  /**
   * Node Transforms...
   */
  protected int nf,mf,nt,mt;
  protected DTransform fn[],tn[];

  /**
   * Executes the transform
   */
  public DTransform getTransform(int aID)
  {
    return tn[aID];
  }

  /**
   * @return number of transforms
   */
  public int getFromTransforms()
  {
    return nf;
  }

  /**
   * @return number of transforms
   */
  public int getToTransforms()
  {
    return nt;
  }

  /**
   * Adds a transform to this node (called by DTransform)
   */
  protected synchronized void setFrom(DTransform aTransform)
  {
    for(int i=0;i<nf;i++) {
      if(fn[i].equals(aTransform)) {
        return;
      }
    }
    if(nf>=mf) {
      mf+=5;
      DTransform nn[] = new DTransform[mf];
      System.arraycopy(fn,0,nn,0,nf);
      fn=nn;
    }
    fn[nf++]=aTransform;
  }

  /**
   * Adds a transform to this node (called by DTransform)
   */
  protected synchronized void setTo(DTransform aTransform)
  {
    for(int i=0;i<nt;i++) {
      if(tn[i].equals(aTransform)) {
        return;
      }
    }
    if(nt>=mt) {
      mt+=5;
      DTransform nn[] = new DTransform[mt];
      System.arraycopy(tn,0,nn,0,nt);
      tn=nn;
    }
    tn[nt++]=aTransform;
  }

  /**
   * Removes a transform (called by DTransform)
   */
  protected synchronized void removeFrom(DTransform aTransform)
  {
    for(int i=0;i<nf;i++) {
      if(tn[i].equals(aTransform)) {
        for(int j=i+1;j<nf;j++,i++) {
          fn[i]=fn[j];
        }
        nf--;
        return;
      }
    }
  }

  /**
   * Removes a transform (called by DTransform)
   */
  protected synchronized void removeTo(DTransform aTransform)
  {
    for(int i=0;i<nt;i++) {
      if(tn[i].equals(aTransform)) {
        for(int j=i+1;j<nt;j++,i++) {
          tn[i]=tn[j];
        }
        nt--;
        return;
      }
    }
  }

}
