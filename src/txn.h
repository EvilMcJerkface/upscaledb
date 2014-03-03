/*
 * Copyright (C) 2005-2014 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 */

#ifndef HAM_TXN_H__
#define HAM_TXN_H__

/*
 * The hamsterdb Transaction implementation
 *
 * hamsterdb stores Transactions in volatile RAM (with an append-only journal
 * in case the RAM is lost). Each Transaction and each modification *in* a 
 * Transaction is stored in a complex data structure.
 *
 * When a Database is created, it contains a BtreeIndex for persistent
 * (committed and flushed) data, and a TransactionIndex for active Transactions
 * and those Transactions which were committed but not yet flushed to disk.
 * This TransactionTree is implemented as a binary search tree (see rb.h).
 *
 * Each node in the TransactionTree is implemented by TransactionNode. Each
 * node is identified by its database key, and groups all modifications of this
 * key (of all Transactions!).
 *
 * Each modification in the node is implemented by TransactionOperation. There
 * is one such TransactionOperation for 'insert', 'erase' etc. The
 * TransactionOperations form two linked lists - one stored in the Transaction
 * ("all operations from this Transaction") and another one stored in the
 * TransactionNode ("all operations on the same key").
 *
 * All Transactions in an Environment for a linked list, where the tail is
 * the chronologically newest Transaction and the head is the oldest
 * (see Transaction::get_newer and Transaction::get_older).
 */

#include <string>

#include "rb.h"
#include "util.h"
#include "error.h"

//
// A helper structure; ham_txn_t is declared in ham/hamsterdb.h as an
// opaque C structure, but internally we use a C++ class. The ham_txn_t
// struct satisfies the C compiler, and internally we just cast the pointers.
//
struct ham_txn_t
{
  int dummy;
};

namespace hamsterdb {

class TransactionNode;
class TransactionIndex;
class TransactionCursor;
class LocalTransaction;
class LocalDatabase;
class Environment;


//
// The TransactionOperation class describes a single operation (i.e.
// insert or erase) in a Transaction.
//
class TransactionOperation
{
  public:
    enum {
      // a NOP operation (empty)
      kNop              = 0x000000u,

      // txn operation is an insert
      kInsert           = 0x010000u,

      // txn operation is an insert w/ overwrite
      kInsertOverwrite  = 0x020000u,

      // txn operation is an insert w/ duplicate
      kInsertDuplicate  = 0x040000u,

      // txn operation erases the key
      kErase            = 0x080000u,

      // txn operation was already flushed
      kIsFlushed        = 0x100000u
    };

    // Returns the flags
    ham_u32_t get_flags() const {
      return (m_flags);
    }

    // This Operation was flushed to disk
    void set_flushed() {
      m_flags |= kIsFlushed;
    }

    // Returns the original flags of ham_insert/ham_cursor_insert/ham_erase...
    ham_u32_t get_orig_flags() const {
      return (m_orig_flags);
    }

    // Returns the referenced duplicate id
    ham_u32_t get_referenced_dupe() const {
      return (m_referenced_dupe);
    }

    // Sets the referenced duplicate id
    void set_referenced_dupe(ham_u32_t id) {
      m_referenced_dupe = id;
    }

    // Returns a pointer to the Transaction of this update
    LocalTransaction *get_txn() {
      return (m_txn);
    }

    // Returns a pointer to the parent node of this update */
    TransactionNode *get_node() {
      return (m_node);
    }

    // Returns the lsn of this operation
    ham_u64_t get_lsn() const {
      return (m_lsn);
    }

    // Returns the record of this operation
    ham_record_t *get_record() {
      return (&m_record);
    }

    // Returns the list of Cursors coupled to this operation
    TransactionCursor *get_cursor_list() {
      return (m_cursor_list);
    }

    // Sets the list of Cursors coupled to this operation
    void set_cursor_list(TransactionCursor *cursors) {
      m_cursor_list = cursors;
    }

    // Returns the next TransactionOperation which modifies the
    // same TransactionNode
    TransactionOperation *get_next_in_node() {
      return (m_node_next);
    }

    // Returns the previous TransactionOperation which modifies the
    // same TransactionNode
    TransactionOperation *get_previous_in_node() {
      return (m_node_prev);
    }

    // Returns the next TransactionOperation in the same Transaction
    TransactionOperation *get_next_in_txn() {
      return (m_txn_next);
    }

    // Returns the previous TransactionOperation in the same Transaction
    TransactionOperation *get_previous_in_txn() {
      return (m_txn_prev);
    }

  private:
    friend class TransactionNode;
    friend struct TransactionFactory;

    // Initialization
    void initialize(LocalTransaction *txn, TransactionNode *node,
                    ham_u32_t flags, ham_u32_t orig_flags, ham_u64_t lsn,
                    ham_record_t *record);

    // Destructor
    void destroy();

    // Sets the next TransactionOperation which modifies the
    // same TransactionNode
    void set_next_in_node(TransactionOperation *next) {
      m_node_next = next;
    }

    // Sets the previous TransactionOperation which modifies the
    // same TransactionNode
    void set_previous_in_node(TransactionOperation *prev) {
      m_node_prev = prev;
    }

    // Sets the next TransactionOperation in the same Transaction
    void set_next_in_txn(TransactionOperation *next) {
      m_txn_next = next;
    }

    // Sets the previous TransactionOperation in the same Transaction
    void set_previous_in_txn(TransactionOperation *prev) {
      m_txn_prev = prev;
    }

    // the Transaction of this operation
    LocalTransaction *m_txn;

    // the parent node
    TransactionNode *m_node;

    // flags and type of this operation; defined in this file
    ham_u32_t m_flags;

    // the original flags of this operation, used when calling
    // ham_cursor_insert, ham_insert, ham_erase etc
    ham_u32_t m_orig_flags;

    // the referenced duplicate id (if neccessary) - used if this is
    // i.e. a ham_cursor_erase, ham_cursor_overwrite or ham_cursor_insert
    // with a DUPLICATE_AFTER/BEFORE flag
    // this is 1-based (like dupecache-index, which is also 1-based)
    ham_u32_t m_referenced_dupe;

    // the log serial number (lsn) of this operation
    ham_u64_t m_lsn;

    // a linked list of cursors which are attached to this operation
    TransactionCursor *m_cursor_list;

    // next in linked list (managed in TransactionNode)
    TransactionOperation *m_node_next;

    // previous in linked list (managed in TransactionNode)
    TransactionOperation *m_node_prev;

    // next in linked list (managed in Transaction)
    TransactionOperation *m_txn_next;

    // previous in linked list (managed in Transaction)
    TransactionOperation *m_txn_prev;

    // the record which is inserted or overwritten
    ham_record_t m_record;

    // Storage for record->data. This saves us one memory allocation.
    ham_u8_t m_data[1];
};


//
// A node in the Transaction Index, used as the node structure in rb.h.
// Manages a group of TransactionOperation objects which all modify the
// same key.
//
class TransactionNode
{
  public:
    // Constructor; creates a deep copy of |key|. Inserts itself into the
    // TransactionIndex of |db| (unless |dont_insert| is true).
    // The default parameters are required for the compilation of rb.h.
    TransactionNode(LocalDatabase *db = 0, ham_key_t *key = 0);

    // Destructor; removes this node from the tree, unless |dont_insert|
    // was set to true
    ~TransactionNode();

    // Returns the database
    LocalDatabase *get_db() {
      return (m_db);
    }

    // Returns the modified key
    ham_key_t *get_key() {
      return (&m_key);
    }

    // Appends an actual operation to this node
    TransactionOperation *append(LocalTransaction *txn, ham_u32_t orig_flags,
              ham_u32_t flags, ham_u64_t lsn, ham_record_t *record);

    // Retrieves the next larger sibling of a given node, or NULL if there
    // is no sibling
    TransactionNode *get_next_sibling();

    // Retrieves the previous larger sibling of a given node, or NULL if there
    // is no sibling
    TransactionNode *get_previous_sibling();

    // Returns the first (oldest) TransactionOperation in this node
    // TODO is this required?
    TransactionOperation *get_oldest_op() {
      return (m_oldest_op);
    };

    // Sets the first (oldest) TransactionOperation in this node
    // TODO is this required?
    void set_oldest_op(TransactionOperation *oldest) {
      m_oldest_op = oldest;
    }

    // Returns the last (newest) TransactionOperation in this node
    // TODO is this required?
    TransactionOperation *get_newest_op() {
      return (m_newest_op);
    };

    // Sets the last (newest) TransactionOperation in this node
    // TODO is this required?
    void set_newest_op(TransactionOperation *newest) {
      m_newest_op = newest;
    }

    // red-black tree stub, required for rb.h
    rb_node(TransactionNode) node;

  private:
    // the database - need this to get the compare function
    LocalDatabase *m_db;

    // the linked list of operations - head is oldest operation
    TransactionOperation *m_oldest_op;

    // the linked list of operations - tail is newest operation
    TransactionOperation *m_newest_op;

    // this is the key which is modified in this node
    ham_key_t m_key;

    // Storage for key->data. This saves us one memory allocation.
    ham_u8_t m_data[1];
};


//
// Each Database has a binary tree which stores the current Transaction
// operations; this tree is implemented in TransactionIndex
//
class TransactionIndex
{
  public:
    // Traverses a TransactionIndex; for each node, a callback is executed
    struct Visitor {
      virtual void visit(TransactionNode *node) = 0;
    };

    // Constructor
    TransactionIndex(LocalDatabase *db);

    // Destructor; frees all nodes and their operations
    ~TransactionIndex();

    // Stores a new TransactionNode in the index
    void store(TransactionNode *node);

    // Removes a TransactionNode from the index
    void remove(TransactionNode *node);

    // Visits every node in the TransactionTree
    void enumerate(Visitor *visitor);

    // Returns an opnode for an optree; if a node with this
    // key already exists then the existing node is returned, otherwise NULL.
    // |flags| can be HAM_FIND_GEQ_MATCH, HAM_FIND_LEQ_MATCH etc
    TransactionNode *get(ham_key_t *key, ham_u32_t flags);

    // Returns the first (= "smallest") node of the tree, or NULL if the
    // tree is empty
    TransactionNode *get_first();

    // Returns the last (= "greatest") node of the tree, or NULL if the
    // tree is empty
    TransactionNode *get_last();

    // Returns the key count of this index
    ham_u64_t get_key_count(LocalTransaction *txn, ham_u32_t flags);

 // private: //TODO re-enable this; currently disabled because rb.h needs it
    // the Database for all operations in this tree
    LocalDatabase *m_db;

    // stuff for rb.h
    TransactionNode *rbt_root;
    TransactionNode rbt_nil;
};


//
// An abstract base class for a Transaction. Overwritten for local and
// remote implementations
//
class Transaction
{
  protected:
    enum {
      // Transaction was aborted
      kStateAborted   = 0x10000,

      // Transaction was committed
      kStateCommitted = 0x20000
    };

  public:
    // Constructor; "begins" the Transaction
    // supported flags: HAM_TXN_READ_ONLY, HAM_TXN_TEMPORARY
    Transaction(Environment *env, const char *name, ham_u32_t flags);

    // Destructor
    virtual ~Transaction() { }

    // Commits the Transaction
    virtual void commit(ham_u32_t flags = 0) = 0;

    // Aborts the Transaction
    virtual void abort(ham_u32_t flags = 0) = 0;

    // Returns true if the Transaction was aborted
    bool is_aborted() const {
      return (m_flags & kStateAborted) != 0;
    }

    // Returns true if the Transaction was committed
    bool is_committed() const {
      return (m_flags & kStateCommitted) != 0;
    }

    // Returns the unique id of this Transaction
    ham_u64_t get_id() const {
      return (m_id);
    }

    // Returns the environment pointer
    Environment *get_env() const {
      return (m_env);
    }

    // Returns the txn name
    const std::string &get_name() const {
      return (m_name);
    }

    // Returns the flags
    ham_u32_t get_flags() const {
      return (m_flags);
    }

    // Returns the cursor refcount (numbers of Cursors using this Transaction)
    ham_u32_t get_cursor_refcount() const {
      return (m_cursor_refcount);
    }

    // Increases the cursor refcount (numbers of Cursors using this Transaction)
    void increase_cursor_refcount() {
      m_cursor_refcount++;
    }

    // Decreases the cursor refcount (numbers of Cursors using this Transaction)
    void decrease_cursor_refcount() {
      ham_assert(m_cursor_refcount > 0);
      m_cursor_refcount--;
    }

    // Returns the memory buffer for the key data.
    // Used to allocate array in ham_find, ham_cursor_move etc. which is
    // then returned to the user.
    ByteArray &get_key_arena() {
      return (m_key_arena);
    }

    // Returns the memory buffer for the record data.
    // Used to allocate array in ham_find, ham_cursor_move etc. which is
    // then returned to the user.
    ByteArray &get_record_arena() {
      return (m_record_arena);
    }

    // Returns the next Transaction in the linked list */
    Transaction *get_next() const {
      return (m_next);
    }

    // Sets the next Transaction in the linked list */
    void set_next(Transaction *n) {
      m_next = n;
    }

  protected:
    // the id of this Transaction
    ham_u64_t m_id;

    // the Environment pointer
    Environment *m_env;

    // flags for this Transaction
    ham_u32_t m_flags;

    // the Transaction name
    std::string m_name;

    // the linked list of all transactions
    Transaction *m_next;

    // reference counter for cursors (number of cursors attached to this txn)
    ham_u32_t m_cursor_refcount;

    // this is where key->data points to when returning a key to the user
    ByteArray m_key_arena;

    // this is where record->data points to when returning a record to the user
    ByteArray m_record_arena;

  private:
    friend class Journal;

    // Sets the unique id of this Transaction; the journal needs this to patch
    // in the id when recovering a Transaction 
    void set_id(ham_u64_t id) {
      m_id = id;
    }
};


} // namespace hamsterdb

#endif /* HAM_TXN_H__ */
