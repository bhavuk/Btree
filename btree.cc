#include <assert.h>
#include <string.h>
#include "btree.h"

KeyValuePair::KeyValuePair()
{}


KeyValuePair::KeyValuePair(const KEY_T &k, const VALUE_T &v) : 
  key(k), value(v)
{}


KeyValuePair::KeyValuePair(const KeyValuePair &rhs) :
  key(rhs.key), value(rhs.value)
{}


KeyValuePair::~KeyValuePair()
{}


KeyValuePair & KeyValuePair::operator=(const KeyValuePair &rhs)
{
  return *( new (this) KeyValuePair(rhs));
}

BTreeIndex::BTreeIndex(SIZE_T keysize, 
                       SIZE_T valuesize,
                       BufferCache *cache,
                       bool unique) 
{
  superblock.info.keysize=keysize;
  superblock.info.valuesize=valuesize;
  buffercache=cache;
  // note: ignoring unique now
}

BTreeIndex::BTreeIndex()
{
  // shouldn't have to do anything
}


//
// Note, will not attach!
//
BTreeIndex::BTreeIndex(const BTreeIndex &rhs)
{
  buffercache=rhs.buffercache;
  superblock_index=rhs.superblock_index;
  superblock=rhs.superblock;
}

BTreeIndex::~BTreeIndex()
{
  // shouldn't have to do anything
}


BTreeIndex & BTreeIndex::operator=(const BTreeIndex &rhs)
{
  return *(new(this)BTreeIndex(rhs));
}


ERROR_T BTreeIndex::AllocateNode(SIZE_T &n)
{
  n=superblock.info.freelist;

  if (n==0) { 
    return ERROR_NOSPACE;
  }

  BTreeNode node;

  node.Unserialize(buffercache,n);

  assert(node.info.nodetype==BTREE_UNALLOCATED_BLOCK);

  superblock.info.freelist=node.info.freelist;

  superblock.Serialize(buffercache,superblock_index);

  buffercache->NotifyAllocateBlock(n);

  return ERROR_NOERROR;
}


ERROR_T BTreeIndex::DeallocateNode(const SIZE_T &n)
{
  BTreeNode node;

  node.Unserialize(buffercache,n);

  assert(node.info.nodetype!=BTREE_UNALLOCATED_BLOCK);

  node.info.nodetype=BTREE_UNALLOCATED_BLOCK;

  node.info.freelist=superblock.info.freelist;

  node.Serialize(buffercache,n);

  superblock.info.freelist=n;

  superblock.Serialize(buffercache,superblock_index);

  buffercache->NotifyDeallocateBlock(n);

  return ERROR_NOERROR;

}

ERROR_T BTreeIndex::Attach(const SIZE_T initblock, const bool create)
{
  ERROR_T rc;

  superblock_index=initblock;
  assert(superblock_index==0);

  if (create) {
    // build a super block, root node, and a free space list
    //
    // Superblock at superblock_index
    // root node at superblock_index+1
    // free space list for rest
    BTreeNode newsuperblock(BTREE_SUPERBLOCK,
                            superblock.info.keysize,
                            superblock.info.valuesize,
                            buffercache->GetBlockSize());
    newsuperblock.info.rootnode=superblock_index+1;
    newsuperblock.info.freelist=superblock_index+2;
    newsuperblock.info.numkeys=0;

    buffercache->NotifyAllocateBlock(superblock_index);

    rc=newsuperblock.Serialize(buffercache,superblock_index);

    if (rc) { 
      return rc;
    }
    
    BTreeNode newrootnode(BTREE_ROOT_NODE,
                          superblock.info.keysize,
                          superblock.info.valuesize,
                          buffercache->GetBlockSize());
    newrootnode.info.rootnode=superblock_index+1;
    newrootnode.info.freelist=superblock_index+2;
    newrootnode.info.numkeys=0;

    buffercache->NotifyAllocateBlock(superblock_index+1);

    rc=newrootnode.Serialize(buffercache,superblock_index+1);

    if (rc) { 
      return rc;
    }

    for (SIZE_T i=superblock_index+2; i<buffercache->GetNumBlocks();i++) { 
      BTreeNode newfreenode(BTREE_UNALLOCATED_BLOCK,
                            superblock.info.keysize,
                            superblock.info.valuesize,
                            buffercache->GetBlockSize());
      newfreenode.info.rootnode=superblock_index+1;
      newfreenode.info.freelist= ((i+1)==buffercache->GetNumBlocks()) ? 0: i+1;
      
      rc = newfreenode.Serialize(buffercache,i);

      if (rc) {
        return rc;
      }

    }
  }

  // OK, now, mounting the btree is simply a matter of reading the superblock 

  return superblock.Unserialize(buffercache,initblock);
}
    

ERROR_T BTreeIndex::Detach(SIZE_T &initblock)
{
  return superblock.Serialize(buffercache,superblock_index);
}
 

ERROR_T BTreeIndex::LookupOrUpdateInternal(const SIZE_T &node,
                                           const BTreeOp op,
                                           const KEY_T &key,
                                           VALUE_T &value)
{
  BTreeNode b;
  ERROR_T rc;
  SIZE_T offset;
  KEY_T testkey;
  SIZE_T ptr;

  rc= b.Unserialize(buffercache,node);

  if (rc!=ERROR_NOERROR) { 
    return rc;
  }

  switch (b.info.nodetype) { 
  case BTREE_ROOT_NODE:
  case BTREE_INTERIOR_NODE:
    // Scan through key/ptr pairs
    //and recurse if possible
    for (offset=0;offset<b.info.numkeys;offset++) { 
      rc=b.GetKey(offset,testkey);
      if (rc) {  return rc; }
      if (key<testkey) {
        // OK, so we now have the first key that's larger
        // so we ned to recurse on the ptr immediately previous to 
        // this one, if it exists
        rc=b.GetPtr(offset,ptr);
        if (rc) { return rc; }
        return LookupOrUpdateInternal(ptr,op,key,value);
      }
    }
    // if we got here, we need to go to the next pointer, if it exists
    if (b.info.numkeys>0) { 
      rc=b.GetPtr(b.info.numkeys,ptr);
      if (rc) { return rc; }
      return LookupOrUpdateInternal(ptr,op,key,value);
    } else {
      // There are no keys at all on this node, so nowhere to go
      return ERROR_NONEXISTENT;
    }
    break;
  case BTREE_LEAF_NODE:
    // Scan through keys looking for matching value
    for (offset=0;offset<b.info.numkeys;offset++) { 
      rc=b.GetKey(offset,testkey);
      if (rc) {  return rc; }
      if (testkey==key) { 
	if (op==BTREE_OP_LOOKUP) { 
	  return b.GetVal(offset,value);
	} else { 
	  // BTREE_OP_UPDATE
	  rc =  b.SetVal(offset,value);
	  if (rc) { return rc; }
	  return b.Serialize(buffercache,node);
	}
      }
    }
    return ERROR_NONEXISTENT;
    break;
  default:
    // We can't be looking at anything other than a root, internal, or leaf
    return ERROR_INSANE;
    break;
  }  

  return ERROR_INSANE;
}


static ERROR_T PrintNode(ostream &os, SIZE_T nodenum, BTreeNode &b, BTreeDisplayType dt)
{
  KEY_T key;
  VALUE_T value;
  SIZE_T ptr;
  SIZE_T offset;
  ERROR_T rc;
  unsigned i;

  if (dt==BTREE_DEPTH_DOT) { 
    os << nodenum << " [ label=\""<<nodenum<<": ";
  } else if (dt==BTREE_DEPTH) {
    os << nodenum << ": ";
  } else {
  }

  switch (b.info.nodetype) { 
  case BTREE_ROOT_NODE:
  case BTREE_INTERIOR_NODE:
    if (dt==BTREE_SORTED_KEYVAL) {
    } else {
      if (dt==BTREE_DEPTH_DOT) { 
      } else { 
        os << "Interior: ";
      }
      for (offset=0;offset<=b.info.numkeys;offset++) { 
        rc=b.GetPtr(offset,ptr);
        if (rc) { return rc; }
        os << "*" << ptr << " ";
        // Last pointer
        if (offset==b.info.numkeys) break;
        rc=b.GetKey(offset,key);
        if (rc) {  return rc; }
        for (i=0;i<b.info.keysize;i++) { 
          os << key.data[i];
        }
        os << " ";
      }
    }
    break;
  case BTREE_LEAF_NODE:
    if (dt==BTREE_DEPTH_DOT || dt==BTREE_SORTED_KEYVAL) { 
    } else {
      os << "Leaf: ";
    }
    for (offset=0;offset<b.info.numkeys;offset++) { 
      if (offset==0) { 
	// special case for first pointer
	rc=b.GetPtr(offset,ptr);
	if (rc) { return rc; }
	if (dt!=BTREE_SORTED_KEYVAL) { 
	  os << "*" << ptr << " ";
	}
      }
      if (dt==BTREE_SORTED_KEYVAL) { 
	os << "(";
      }
      rc=b.GetKey(offset,key);
      if (rc) {  return rc; }
      for (i=0;i<b.info.keysize;i++) { 
	os << key.data[i];
      }
      if (dt==BTREE_SORTED_KEYVAL) { 
	os << ",";
      } else {
	os << " ";
      }
      rc=b.GetVal(offset,value);
      if (rc) {  return rc; }
      for (i=0;i<b.info.valuesize;i++) { 
	os << value.data[i];
      }
      if (dt==BTREE_SORTED_KEYVAL) { 
	os << ")\n";
      } else {
	os << " ";
      }
    }
    break;
  default:
    if (dt==BTREE_DEPTH_DOT) { 
      os << "Unknown("<<b.info.nodetype<<")";
    } else {
      os << "PrintNode: Unsupported Node Type " << b.info.nodetype ;
    }
  }
  if (dt==BTREE_DEPTH_DOT) { 
    os << "\" ]";
  }
  return ERROR_NOERROR;
}
  
ERROR_T BTreeIndex::Lookup(const KEY_T &key, VALUE_T &value)
{
  return LookupOrUpdateInternal(superblock.info.rootnode, BTREE_OP_LOOKUP, key, value);
}

ERROR_T BTreeIndex::Inserter(list<SIZE_T> crumbs, const SIZE_T &node, const KEY_T &key, const VALUE_T &value)
{
  BTreeNode b;
  BTreeNode& b_ref = b;
  ERROR_T rc;
  SIZE_T offset;
  KEY_T testkey;
  SIZE_T ptr;

  // Push current node 
  crumbs.push_front(node);

  rc = b.Unserialize(buffercache,node);
  if (rc) { return rc; }

  switch (b.info.nodetype) {
    case BTREE_ROOT_NODE:
      if (b.info.numkeys==0) {
        //
        // Special case where rootnode is empty

        SIZE_T left_block_loc;
        SIZE_T& left_block_ref = left_block_loc;
        SIZE_T right_block_loc;
        SIZE_T& right_block_ref = right_block_loc;

        // Left node
        //
        // Get block offset from AllocateNode
        rc = AllocateNode(left_block_ref);
        if (rc) { cout<<rc<<endl; return rc; }

        // Unserialize from block offset into left_node
        BTreeNode left_node;
        rc = left_node.Unserialize(buffercache,left_block_loc);	
        if (rc) { return rc; }

        // left_node is a leaf node
        left_node.info.nodetype = BTREE_LEAF_NODE;
        left_node.data = new char [left_node.info.GetNumDataBytes()];
        memset(left_node.data,0,left_node.info.GetNumDataBytes());

        // Set number of keys in left_node to 0
        left_node.info.numkeys = 0;

        // Serialize left_node back into buffer
        rc = left_node.Serialize(buffercache,left_block_loc);
        if (rc) { return rc; }


        // Right node
        //
        // Get block offset from AllocateNode
        rc = AllocateNode(right_block_ref);
        if (rc) { cout<<rc<<endl; return rc; }

        // Unserialize from block offset into right_node
        BTreeNode right_node;
        rc = right_node.Unserialize(buffercache,right_block_loc);	
        if (rc) { return rc; }

        // right_node is a leaf node
        right_node.info.nodetype = BTREE_LEAF_NODE;
        right_node.data = new char [right_node.info.GetNumDataBytes()];
        memset(right_node.data,0,right_node.info.GetNumDataBytes());

        // Set number of keys in right_node to 1
        right_node.info.numkeys = 1;

        // Set key of right_node
        rc = right_node.SetKey(0,key);
        if (rc) { return rc; }

        // Set value in right_node
        rc = right_node.SetVal(0,value);
        if (rc) { return rc; }

        // Serialize right_node back into buffer
        rc = right_node.Serialize(buffercache,right_block_loc);
        if (rc) { return rc; }


        // Root node
        //
        // Set number of keys in root to 1
        b.info.numkeys = 1;

        // Set key in root
        rc = b.SetKey(0,key);
        if (rc) { return rc; }

        // Set left pointer of root to point at left_node
        rc = b.SetPtr(0,left_block_ref);
        if (rc) { return rc; }

        // Set right pointer of root to point at right_node
        rc = b.SetPtr(1,right_block_ref);
        if (rc) { return rc; }

        // Serialize root node
        rc = b.Serialize(buffercache,node);
        if (rc) { return rc; }

        return ERROR_NOERROR;
        break;
      } 

    case BTREE_INTERIOR_NODE:
      // Scan through key/ptr pairs
      //and recurse if possible
      for (offset=0;offset<b.info.numkeys;offset++) { 
        rc=b.GetKey(offset,testkey);
        if (rc) {  return rc; }
        if (key<testkey) {
          // OK, so we now have the first key that's larger
          // so we ned to recurse on the ptr immediately previous to 
          // this one, if it exists
          rc=b.GetPtr(offset,ptr);
          if (rc) { return rc; }
          return Inserter(crumbs,ptr,key,value);
        }
      }
      // if we got here, we need to go to the next pointer, if it exists
      if (b.info.numkeys>0) { 
        rc=b.GetPtr(b.info.numkeys,ptr);
        if (rc) { return rc; }
        return Inserter(crumbs,ptr,key,value);
      } else {
        // There are no keys at all on this node, so nowhere to go
        return ERROR_NONEXISTENT;
      }
      break;

    case BTREE_LEAF_NODE:
      return LeafNodeInsert(crumbs, node ,b_ref, key, value);

    default:
      return ERROR_INSANE;
      break;
   }

   return ERROR_INSANE;
}

ERROR_T BTreeIndex::LeafNodeInsert(list<SIZE_T> crumbs, const SIZE_T &node, BTreeNode &b, const KEY_T &key, const VALUE_T &value)
{
  ERROR_T rc;
  SIZE_T offset;
  KEY_T testkey;

  // Iterator and temporary keys and values, used to copy data
  SIZE_T iter;
  KEY_T temp_key;
  KEY_T& temp_key_ref = temp_key;
  VALUE_T temp_val;
  VALUE_T& temp_val_ref = temp_val;

  //---------------//

  if (b.info.nodetype!=BTREE_LEAF_NODE) {
    // If we aren't in a leaf node, something bad has happened.
    return ERROR_BADNODETYPE;
  }

  if (b.info.numkeys==0) {
    // Special case where leaf node is empty. Insert at beginning.

    //Increment numkeys
    b.info.numkeys++;

    rc = b.SetKey(0,key);
    if (rc) { return rc; }

    rc = b.SetVal(0,value);
    if (rc) { return rc; }

    rc = b.Serialize(buffercache,node);
    if (rc) { return rc; }

    return ERROR_NOERROR;
  }

  //
  // Node has keys and values in it

  for (offset=0;offset<b.info.numkeys;offset++) {
    // move through keys until we find one larger than input key
    rc = b.GetKey(offset,testkey);
    if (rc) {  return rc; }
    //
    // If key exists, can't insert. Return conflict error.
    if (key == testkey) { return ERROR_CONFLICT; }
    // Otherwise, break loop and continue
    if (key<testkey) { break; }
  }

  // Increment numkeys
  b.info.numkeys++;

  // Move back from largest key to offset key, shifting keys and values one to
  // the right
  for(iter=(b.info.numkeys-2);iter>=offset;iter--) {

    // Shift key
    rc = b.GetKey(iter,temp_key_ref);
    if (rc) { return rc; }
    rc = b.SetKey(iter+1,temp_key_ref);
    if (rc) { return rc; }

    // Shift value
    rc = b.GetVal(iter,temp_val_ref);
    if (rc) { return rc; }
    rc = b.SetVal(iter+1,temp_val_ref);
    if (rc) { return rc; }

  // Test if pointers are equal, since >= doesn't work with pointers. Break loop if they are
    if (iter == offset) { break; }
  }

  // Set input key
  rc = b.SetKey(offset,key);
  if (rc) { return rc; }

  // Set input value
  rc = b.SetVal(offset,value);
  if (rc) { return rc; }

  // Serialize block
  rc = b.Serialize(buffercache,node);
  if (rc) { return rc; }

  if (b.info.numkeys >= b.info.GetNumSlotsAsLeaf()) {
    // We're at or over the slot upper bound
    rc = Split(crumbs);
    if (rc) { return rc; }
  }

  return ERROR_NOERROR;
}

ERROR_T BTreeIndex::Split(list<SIZE_T> crumbs)
{
  SIZE_T orig_block_loc;
  ERROR_T rc;

  // First node offset on list is current node. Pop it for when we recurse.
  if (crumbs.empty()) { return ERROR_INSANE; }
  orig_block_loc = crumbs.front();
  SIZE_T& orig_block_ref = orig_block_loc;
  crumbs.pop_front();

  // Unserialize node
  BTreeNode orig_node;
  rc = orig_node.Unserialize(buffercache,orig_block_ref);
  if (rc) { return rc; }

  // To hold the key sizes for the split blocks
  SIZE_T k2;
  SIZE_T k1;

  // Pointer to new block location and reference to it and new node
  SIZE_T new_block_loc;
  SIZE_T& new_block_ref = new_block_loc;
  BTreeNode new_node;
  

  // Iterator and temporary keys and values, used to copy data
  SIZE_T iter;
  KEY_T temp_key;
  KEY_T& temp_key_ref = temp_key;
  SIZE_T temp_ptr;
  SIZE_T& temp_ptr_ref = temp_ptr;
  VALUE_T temp_val;
  VALUE_T& temp_val_ref = temp_val;

  unsigned int i;
  string null_key_str;
  for (i = 0; i < superblock.info.keysize; i++) {
    null_key_str = "0" + null_key_str;
  }

  string null_val_str;

  // Null pointer used to set copied values to NULL in old_node
  SIZE_T null_ptr = 0;
  SIZE_T& null_ptr_ref = null_ptr;


  switch (orig_node.info.nodetype) { 
    case BTREE_ROOT_NODE:
      // Falls through into interior node
    case BTREE_INTERIOR_NODE:


      if (orig_node.info.numkeys < orig_node.info.GetNumSlotsAsInterior()) { return ERROR_INSANE; }
      
      // Numbers of keys for blocks that result from split
      k1 = orig_node.info.numkeys/2;
      k2 = orig_node.info.numkeys-k1-1;

      // Get block from AllocateNode and unserialize into new_node
      rc = AllocateNode(new_block_ref);
      if (rc) { cout<<rc<<endl; return rc; }
      rc = new_node.Unserialize(buffercache, new_block_ref);
      if (rc) { return rc; }

      // Set new_node type and numkeys
      new_node.info.nodetype=BTREE_INTERIOR_NODE;
      new_node.info.numkeys=k2;
      // Initialize new_node's data to all 0's
      new_node.data = new char [new_node.info.GetNumDataBytes()];
      memset(new_node.data,0,new_node.info.GetNumDataBytes());

      // Loop through orig_node, copying into new_node
      for (iter=k1+1; iter<orig_node.info.numkeys; iter++) {
        
        // Copy key
        rc = orig_node.GetKey(iter,temp_key_ref);
        if (rc) { return rc; }
        rc = new_node.SetKey(iter-(k1+1),temp_key_ref);
        if (rc) { return rc; }
        // Set copied key to 0
        rc = orig_node.SetKey(iter,KEY_T(null_key_str.c_str()));
        if (rc) { return rc; }

        // Copy pointer
        rc = orig_node.GetPtr(iter,temp_ptr_ref);
        if (rc) { return rc; }
        rc = new_node.SetPtr(iter-(k1+1),temp_ptr_ref);
        if (rc) { return rc; }
        // Set copied pointer to 0
        rc = orig_node.SetPtr(iter,SIZE_T(null_ptr_ref));
        if (rc) { return rc; }
      }

      // Set key at index k1 of orig_node to null. It is unneeded after the split.
      rc = orig_node.SetKey(k1,KEY_T(null_key_str.c_str()));
      if (rc) { return rc; }

      // Copy last pointer in orig_node to new_node
      rc = orig_node.GetPtr(orig_node.info.numkeys,temp_ptr_ref);
      if (rc) { return rc; }
      rc = new_node.SetPtr(k1,temp_ptr_ref);
      // Set pointer to 0
      rc = orig_node.SetPtr(orig_node.info.numkeys,null_ptr_ref);
      if (rc) { return rc; }

      // Set numkeys of orig_node to k1
      orig_node.info.numkeys=k1;

      //
      // Different ending depending on ROOT_NODE vs INTERIOR NODE
      if (orig_node.info.nodetype == BTREE_INTERIOR_NODE) {
        // Serialize orig_node and new_node
        rc = orig_node.Serialize(buffercache, orig_block_ref);
        if (rc) { return rc; }
        rc = new_node.Serialize(buffercache, new_block_ref);
        if (rc) { return rc; }

        // Get the first key in the new_node. This is the key we'll insert into the parent.
        rc = new_node.GetKey(0,temp_key_ref);
        if (rc) { return rc; }

        // Insert a pointer to it into the parent node of orig_node, using InternalPointerInsert
        rc = InteriorPointerInsert(crumbs, temp_key, new_block_ref);
        if (rc) { return rc; }

        return ERROR_NOERROR;
      } else {
        // orig_node is now an interior node, so set it as such
        orig_node.info.nodetype=BTREE_INTERIOR_NODE;

        //
        // We need to create a new root node, set the superblock to point at it, and insert both
        // orig_node and new_node into it.
        SIZE_T new_root_loc;
        SIZE_T& new_root_ref = new_root_loc;
        BTreeNode new_root;

        // Allocate new root node
        rc = AllocateNode(new_root_ref);
        if (rc) { cout<<rc<<endl; return rc; }
        rc = new_root.Unserialize(buffercache, new_root_ref);
        if (rc) { return rc; }

        // Set new_root type, numkeys, and data
        new_root.info.nodetype=BTREE_ROOT_NODE;
        new_root.info.numkeys=1;
        new_root.data = new char [new_root.info.GetNumDataBytes()];
        memset(new_root.data,0,new_root.info.GetNumDataBytes());

        // Set superblock to point to new_root
        superblock.info.rootnode = new_root_loc;

        // Serialize orig_node, and new_node
        rc = orig_node.Serialize(buffercache,orig_block_ref);
        if (rc) { return rc; }
        rc = new_node.Serialize(buffercache,new_block_ref);
        if (rc) { return rc; }

        // Must insert manually into new_root
        //
        // Use first key in new_node as the first key in new_root
        rc = new_node.GetKey(0,temp_key_ref);
        if (rc) { return rc; }
        // Set it in new_root
        rc = new_root.SetKey(0,temp_key_ref);
        if (rc) { return rc; }
        // Insert pointers to orig_node and new_node
        rc = new_root.SetPtr(0,orig_block_ref);
        if (rc) { return rc; }
        rc = new_root.SetPtr(1,new_block_ref);
        if (rc) { return rc; }
        // Serialize new_root
        rc = new_root.Serialize(buffercache,new_root_ref);
        if (rc) { return rc; }

        return ERROR_NOERROR;
      }

      // Control shouldn't reach here
      return ERROR_INSANE;
      break;

    case BTREE_LEAF_NODE:
      
      // Build up null_val_str of 0's.
      for (i = 0; i < superblock.info.valuesize; i++) {
        null_val_str = "0" + null_val_str;
      }


      if (orig_node.info.numkeys < orig_node.info.GetNumSlotsAsLeaf()) { return ERROR_INSANE; }
      
      // Numbers of keys for blocks that result from split
      k2 = orig_node.info.numkeys/2;
      k1 = orig_node.info.numkeys-k2;
        
      // Get block from AllocateNode and unserialize into new_node
      rc = AllocateNode(new_block_ref);
      if (rc) { cout<<rc<<endl; return rc; }
      rc = new_node.Unserialize(buffercache, new_block_ref);
      if (rc) { return rc; }
      
      // Set new_node type and numkeys
      new_node.info.nodetype=BTREE_LEAF_NODE;
      new_node.info.numkeys=k2;
      // Initialize new_node's data to all 0's
      new_node.data = new char [new_node.info.GetNumDataBytes()];
      memset(new_node.data,0,new_node.info.GetNumDataBytes());

      // Loop through orig_node, copying into new_node
      for(iter=k1;iter<orig_node.info.numkeys;iter++) {
        
        // Copy key
        rc = orig_node.GetKey(iter,temp_key_ref);
        if (rc) { return rc; }
        rc = new_node.SetKey(iter-k1,temp_key_ref);
        if (rc) { return rc; }
        // Set copied key to 0
        rc = orig_node.SetKey(iter,KEY_T(null_key_str.c_str()));
        if (rc) { return rc; }

        // Copy value
        rc = orig_node.GetVal(iter,temp_val_ref);
        if (rc) { return rc; }
        rc = new_node.SetVal(iter-k1,temp_val_ref);
        if (rc) { return rc; }
        // Set copied value to 0
        rc = orig_node.SetVal(iter,VALUE_T(null_val_str.c_str()));
        if (rc) { return rc; }
      }

      //Set the original node's number of keys to k1
      orig_node.info.numkeys=k1;

      // Serialize orig_node and new_node
      rc = orig_node.Serialize(buffercache,orig_block_ref);
      if (rc) { return rc; }
      rc = new_node.Serialize(buffercache,new_block_ref);
      if (rc) { return rc; }

      // Get the first key in the new_node. This is the key we'll insert into the parent.
      rc = new_node.GetKey(0,temp_key_ref);
      if (rc) { return rc; }

      // Insert a pointer to it into the parent node of orig_node, using InternalPointerInsert
      rc = InteriorPointerInsert(crumbs, temp_key, new_block_ref);
      if (rc) { return rc; }

      return ERROR_NOERROR;
      break;

 
    default:
      return ERROR_INSANE;
      break;
  }

  // We shouldn't reach here
  return ERROR_INSANE;
}

ERROR_T BTreeIndex::InteriorPointerInsert(list<SIZE_T> crumbs, const KEY_T &key, const SIZE_T &ptr)
{
  BTreeNode b;
  ERROR_T rc;
  SIZE_T offset;
  KEY_T testkey;

  // Iterator and temporary keys and pointers, used to copy data
  SIZE_T iter;
  KEY_T temp_key;
  KEY_T& temp_key_ref = temp_key;
  SIZE_T temp_ptr;
  SIZE_T& temp_ptr_ref = temp_ptr;

  //---------------//

  // Crumbs should never be empty
  if (crumbs.empty()) { return ERROR_INSANE; }

  // node is first node in crumbs list. Don't pop so that Split will look at this node.
  const SIZE_T& node = crumbs.front();

  rc = b.Unserialize(buffercache,node);
  if (rc) { return rc; }

  // Check nodetype. If it isn't an interior node or the root node, error.
  if (b.info.nodetype != BTREE_INTERIOR_NODE && b.info.nodetype != BTREE_ROOT_NODE)
  {
    return ERROR_BADNODETYPE;
  }

  //
  // Node shouldn't be empty.
  if (b.info.numkeys == 0) {
    return ERROR_INSANE;
  }

  for (offset=0; offset<b.info.numkeys; offset++) {
    // Move through keys until we find one larger than input key
    rc = b.GetKey(offset,testkey);
    if (rc) { return rc; }
    // If key exists, conflict error.
    if (key == testkey) { return ERROR_CONFLICT; }
    // Otherwise, break loop
    if (key<testkey) { break; }
  }

  // Increment numkeys
  b.info.numkeys++;

  // Move back from largest key to offset key, shifting keys and pointers
  // one slot to the right
  for (iter=b.info.numkeys-2; iter>=offset; iter--) {

    // Shift key
    rc = b.GetKey(iter,temp_key_ref);
    if (rc) { return rc; }
    rc = b.SetKey(iter+1,temp_key_ref);
    if (rc) { return rc; }

    //Shift pointer
    rc = b.GetPtr(iter+1,temp_ptr_ref);
    if (rc) { return rc; }
    rc = b.SetPtr(iter+2,temp_ptr_ref);
    if (rc) { return rc; }

    // Test if pointers are equal, since >= doesn't work with pointers. Break loop if they are.
    if (iter == offset) { break; }
  }

  // Set input key
  rc = b.SetKey(offset,key);
  if (rc) { return rc; }

  // Set input ptr
  rc = b.SetPtr(offset+1,ptr);
  if (rc) { return rc; }

  // Serialize block
  rc = b.Serialize(buffercache,node);
  if (rc) { return rc; }

  if (b.info.numkeys >= b.info.GetNumSlotsAsInterior()) {
    // Check if we're at or over the slot upper bound, split if we are.
    rc = Split(crumbs);
    if (rc) { return rc; }
  }

  return ERROR_NOERROR;  
  
}
  
ERROR_T BTreeIndex::Insert(const KEY_T &key, const VALUE_T &value)
{
  list<SIZE_T> crumbs;
  return Inserter(crumbs, superblock.info.rootnode, key, value);
}

ERROR_T BTreeIndex::Update(const KEY_T &key, const VALUE_T &value)
{
  VALUE_T val = value;
  return LookupOrUpdateInternal(superblock.info.rootnode, BTREE_OP_UPDATE, key, val);
}

  
ERROR_T BTreeIndex::Delete(const KEY_T &key)
{
  // This is optional extra credit for F12
  //
  // 
  return ERROR_UNIMPL;
}

  
//
//
// DEPTH first traversal
// DOT is Depth + DOT format
//

ERROR_T BTreeIndex::DisplayInternal(const SIZE_T &node,
				    ostream &o,
				    BTreeDisplayType display_type) const
{
  KEY_T testkey;
  SIZE_T ptr;
  BTreeNode b;
  ERROR_T rc;
  SIZE_T offset;

  rc= b.Unserialize(buffercache,node);

  if (rc!=ERROR_NOERROR) { 
    return rc;
  }

  rc = PrintNode(o,node,b,display_type);
  
  if (rc) { return rc; }

  if (display_type==BTREE_DEPTH_DOT) { 
    o << ";";
  }

  if (display_type!=BTREE_SORTED_KEYVAL) {
    o << endl;
  }

  switch (b.info.nodetype) { 
    case BTREE_ROOT_NODE:
    case BTREE_INTERIOR_NODE:
      if (b.info.numkeys>0) { 
        for (offset=0;offset<=b.info.numkeys;offset++) { 
    rc=b.GetPtr(offset,ptr);
    if (rc) { return rc; }
    if (display_type==BTREE_DEPTH_DOT) { 
      o << node << " -> "<<ptr<<";\n";
    }
    rc=DisplayInternal(ptr,o,display_type);
    if (rc) { return rc; }
        }
      }
      return ERROR_NOERROR;
      break;
    case BTREE_LEAF_NODE:
      return ERROR_NOERROR;
      break;
    default:
      if (display_type==BTREE_DEPTH_DOT) { 
      } else {
        o << "DisplayInternal: Unsupported Node Type " << b.info.nodetype ;
      }
      return ERROR_INSANE;
  }

  return ERROR_NOERROR;
}


ERROR_T BTreeIndex::Display(ostream &o, BTreeDisplayType display_type) const
{
  ERROR_T rc;
  if (display_type==BTREE_DEPTH_DOT) { 
    o << "digraph tree { \n";
  }
  rc=DisplayInternal(superblock.info.rootnode,o,display_type);
  if (rc) { return rc; }
  if (display_type==BTREE_DEPTH_DOT) { 
    o << "}\n";
  }
  return ERROR_NOERROR;
}


  
ERROR_T BTreeIndex::ISA_Tree(set<SIZE_T> visited, const SIZE_T &node) const
{
  BTreeNode b;
  ERROR_T rc;
  SIZE_T ptr;
  SIZE_T& ptr_ref = ptr;
  SIZE_T offset;

  //
  //Check here to see if node has already been visited(by scanning the visited list)
  //
  if (visited.count(node)) {
    // We've been here before! Error
    return ERROR_INSANE;
  } else {
    // Haven't seen this. Insert it.
    visited.insert(node);
  }
  

  rc = b.Unserialize(buffercache, node);
  if(rc) {return rc;}

  switch(b.info.nodetype){
  case BTREE_ROOT_NODE:
  case BTREE_INTERIOR_NODE:

    if (b.info.numkeys >= b.info.GetNumSlotsAsInterior()) {
      return ERROR_INSANE;
    }

    for(offset=0; offset<=b.info.numkeys; offset++){
      rc = b.GetPtr(offset, ptr_ref);
      if(rc) {return rc;}
      rc = ISA_Tree(visited, ptr_ref);
      if (rc) { return rc; }
    }
    return ERROR_NOERROR;
    break;
  case BTREE_LEAF_NODE:
    if (b.info.numkeys >= b.info.GetNumSlotsAsLeaf()) {
      return ERROR_INSANE;
    }
    return ERROR_NOERROR;
    break;
  default:
    return ERROR_INSANE;
    break;
  }
  return ERROR_INSANE;
}

ERROR_T BTreeIndex::SanityCheck() const
{
  set<SIZE_T> visited;
  SIZE_T root = superblock.info.rootnode;
  return ISA_Tree(visited, root);

}


ostream & BTreeIndex::Print(ostream &os) const
{
  // WRITE ME
  return os;
}




