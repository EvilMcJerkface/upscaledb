/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * NOTICE: All information contained herein is, and remains the property
 * of Christoph Rupp and his suppliers, if any. The intellectual and
 * technical concepts contained herein are proprietary to Christoph Rupp
 * and his suppliers and may be covered by Patents, patents in process,
 * and are protected by trade secret or copyright law. Dissemination of
 * this information or reproduction of this material is strictly forbidden
 * unless prior written permission is obtained from Christoph Rupp.
 */

/*
 * Compressed 32bit integer keys
 *
 * @exception_safe: strong
 * @thread_safe: no
 */

#ifndef HAM_BTREE_KEYS_SIMDCOMP_H
#define HAM_BTREE_KEYS_SIMDCOMP_H

#include <sstream>
#include <iostream>
#include <algorithm>

#include "0root/root.h"

#include "3rdparty/simdcomp/include/simdcomp.h"

// Always verify that a file of level N does not include headers > N!
#include "3btree/btree_keys_block.h"

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

namespace hamsterdb {

//
// The template classes in this file are wrapped in a separate namespace
// to avoid naming clashes with other KeyLists
//
namespace Zint32 {

// This structure is an "index" entry which describes the location
// of a variable-length block
#include "1base/packstart.h"
HAM_PACK_0 class HAM_PACK_1 SimdCompIndex : public IndexBase {
  public:
    enum {
      // Initial size of a new block (1 bit per key = 16 bytes)
      kInitialBlockSize = 16,

      // Grow blocks by this factor
      kGrowFactor = 16,

      // Maximum keys per block (a compressed block holds up to 128 keys,
      // and one key is stored in the index)
      kMaxKeysPerBlock = 129,

      // Maximum size of an encoded integer; not required
      kMaxSizePerInt = 5,

      // Maximum block size - not relevant for this codec
      kMaxBlockSize = 102400,
    };

    // initialize this block index
    void initialize(uint32_t offset, uint32_t block_size) {
      IndexBase::initialize(offset);
      m_bits = block_size / 16;
      m_key_count = 0;
    }

    // returns the used block of the block
    uint32_t used_size() const {
      return (block_size());
    }

    // sets the used size; not required
    void set_used_size(uint32_t size) {
      // nop
    }

    // returns the total block size
    uint32_t block_size() const {
      return (m_bits * 128 / 8);
    }

    // sets the block size; not required
    void set_block_size(uint32_t new_size) {
      // nop
    }

    // returns the key count
    uint32_t key_count() const {
      return (m_key_count);
    }

    // sets the key count
    void set_key_count(uint32_t key_count) {
      m_key_count = key_count;
    }

    // returns the bits used to encode the block
    uint32_t bits() const {
      return (m_bits);
    }

    // sets the bits used to encode the block
    void set_bits(uint32_t bits) {
      m_bits = bits;
    }

    // copies this block to the |dest| block
    void copy_to(const uint8_t *block_data, SimdCompIndex *dest,
                    uint8_t *dest_data) {
      ham_assert(dest->bits() == bits());
      dest->set_value(value());
      dest->set_key_count(key_count());
      ::memcpy(dest_data, block_data, block_size());
    }

  private:
    // the number of keys in this block; max 129 (kMaxKeysPerBlock)
    unsigned short m_key_count : 8;

    // stored bits per integer; max 32
    unsigned short m_bits : 6;
} HAM_PACK_2;
#include "1base/packstop.h"

struct SimdCompCodecImpl : public BlockCodecBase<SimdCompIndex>
{
  enum {
    kHasCompressApi = 1,
    kHasFindLowerBoundApi = 1,
    kHasSelectApi = 1,
    kHasDelApi = 1,
  };

  static uint32_t compress_block(SimdCompIndex *index, const uint32_t *in,
                  uint32_t *out) {
    ham_assert(index->key_count() > 0);
    simdpackwithoutmaskd1(index->value(), in, (__m128i *)out, index->bits());
    return (index->used_size());
  }

  static uint32_t *uncompress_block(SimdCompIndex *index,
                  const uint32_t *block_data, uint32_t *out) {
    simdunpackd1(index->value(), (__m128i *)block_data, out, index->bits());
    return (out);
  }

  static int find_lower_bound(SimdCompIndex *index, const uint32_t *block_data,
                  uint32_t key, uint32_t *presult) {
    return (simdsearchwithlengthd1(index->value(), (const __m128i *)block_data,
                                    index->bits(), (int)index->key_count() - 1,
                                    key, presult));
  }

  // Returns a decompressed value
  static uint32_t select(SimdCompIndex *index, uint32_t *block_data,
                        int position_in_block) {
    return (simdselectd1(index->value(), (const __m128i *)block_data,
                                    index->bits(), position_in_block));
  }

  template<typename GrowHandler>
  static void del(SimdCompIndex *index, uint32_t *block_data, int slot,
                  GrowHandler *key_list) {
    // The key is now deleted from the block, and afterwards the block
    // is compressed again. The simdcomp algorithm is not delete-stable,
    // which means that after compression it might require more storage
    // than before. If this is the case an Exception is thrown and the
    // caller will provide more space.
    //
    // !!
    // Make sure that this code path is Exception Safe! Do not modify any
    // persistent data until we are 100% sure that no exception will be
    // thrown!

    // uncompress the block and remove the key
    uint32_t data[128];
    uncompress_block(index, block_data, data);

    // delete the first value?
    if (slot == 0) {
      index->set_value(data[0]);
      slot++;
    }

    if (slot < (int)index->key_count() - 1) {
      ::memmove(&data[slot - 1], &data[slot],
              sizeof(uint32_t) * (index->key_count() - slot - 1));
    }

    // grow the block?
    if (index->bits() < 32 && slot < (int)index->key_count() - 1) {
      uint32_t new_bits;
      ham_assert(slot > 0);
      if (slot == 1)
         new_bits = bits(data[0] - index->value());
      else
         new_bits = bits(data[slot - 1] - data[slot - 2]);
      if (new_bits > index->bits()) {
        // yes, try to grow; this will cause a split if it fails
        uint32_t new_size = new_bits * 128 / 8;
        key_list->grow_block(index, new_size - index->block_size());
        index->set_bits(new_bits);
      }
    }

    index->set_key_count(index->key_count() - 1);
    if (index->key_count() > 1)
      compress_block(index, data, block_data);
  }
};

typedef Zint32Codec<SimdCompIndex, SimdCompCodecImpl> SimdCompCodec;

class SimdCompKeyList : public BlockKeyList<SimdCompCodec>
{
  public:
    // Constructor
    SimdCompKeyList(LocalDatabase *db)
      : BlockKeyList<SimdCompCodec>(db) {
    }

    // Copies all keys from this[sstart] to dest[dstart]; this method
    // is used to split and merge btree nodes.
    void copy_to(int sstart, size_t node_count, SimdCompKeyList &dest,
                    size_t other_count, int dstart) {
      ham_assert(check_integrity(0, node_count));

      // if the destination node is empty (often the case when merging nodes)
      // then re-initialize it.
      if (other_count == 0)
        dest.initialize();

      // find the start block
      int src_position_in_block;
      Index *srci = find_block_by_slot(sstart, &src_position_in_block);
      // find the destination block
      int dst_position_in_block;
      Index *dsti = dest.find_block_by_slot(dstart, &dst_position_in_block);

      bool initial_block_used = false;

      // If start offset or destination offset > 0: uncompress both blocks,
      // merge them
      if (src_position_in_block > 0 || dst_position_in_block > 0) {
        uint32_t sdata[128];
        uncompress_block(srci, sdata);
        uint32_t ddata[128];
        dest.uncompress_block(dsti, ddata);

        uint32_t *d = &ddata[srci->key_count()];

        if (src_position_in_block == 0) {
          ham_assert(dst_position_in_block != 0);
          *d = srci->value();
          d++;
        }
        else {
          ham_assert(dst_position_in_block == 0);
          dsti->set_value(sdata[src_position_in_block - 1]);
          src_position_in_block++;
        }
        dsti->set_key_count(dsti->key_count() + 1);

        for (int i = src_position_in_block; i < (int)srci->key_count(); i++) {
          ddata[dsti->key_count() - 1] = sdata[i - 1];
          dsti->set_key_count(dsti->key_count() + 1);
        }

        srci->set_key_count(srci->key_count() - dsti->key_count());

        // grow destination block?
        if (dsti->bits() < 32) {
          uint32_t new_bits = calc_max_bits(dsti->value(), &ddata[0],
                                    dsti->key_count() - 1);
          if (new_bits > dsti->bits()) {
            uint32_t new_size = new_bits * 128 / 8;
            dest.grow_block(dsti, new_size - dsti->block_size());
            dsti->set_bits(new_bits);
          }
        }

        dest.compress_block(dsti, ddata);

        srci++;
        dsti++;
        initial_block_used = true;
      }

      // When merging nodes, check if we actually append to the other node
      if (dst_position_in_block == 0 && dstart > 0)
        initial_block_used = true; // forces loop below to create a new block

      // Now copy the remaining blocks (w/o uncompressing them)
      // TODO this could be sped up by adding multiple blocks in one step
      int copied_blocks = 0;
      for (; srci < get_block_index(get_block_count());
                      srci++, copied_blocks++) {
        if (initial_block_used == true)
          dsti = dest.add_block(dest.get_block_count(), srci->block_size());
        else if (dsti->bits() < srci->bits()) {
          dest.grow_block(dsti, srci->block_size() - dsti->block_size());
          dsti->set_bits(srci->bits());
          initial_block_used = true;
        }

        srci->copy_to(get_block_data(srci), dsti, dest.get_block_data(dsti));
      }

      // remove the copied blocks
      uint8_t *pend = &m_data[get_used_size()];
      uint8_t *pold = (uint8_t *)get_block_index(get_block_count());
      uint8_t *pnew = (uint8_t *)get_block_index(get_block_count()
                                    - copied_blocks);
      ::memmove(pnew, pold, pend - pold);

      set_block_count(get_block_count() - copied_blocks);

      reset_used_size();

      // we need at least ONE empty block, otherwise a few functions will bail
      if (get_block_count() == 0) {
        initialize();
      }

      ham_assert(dest.check_integrity(0, other_count + (node_count - sstart)));
      ham_assert(check_integrity(0, sstart));
    }

  private:
    // Returns the number of bits required to store a block
    uint32_t calc_max_bits(uint32_t initial_value, uint32_t *data,
                    uint32_t length) const {
      if (length == 0)
        return (1);
      return (simdmaxbitsd1_length(initial_value, data, length));
    }

    // Implementation for insert()
    virtual PBtreeNode::InsertResult insert_impl(size_t node_count,
                    uint32_t key, uint32_t flags) {
      int slot = 0;

      // perform a linear search through the index and get the block
      // which will receive the new key
      Index *index = find_index(key, &slot);

      // first key in an empty block? then don't store a delta
      if (index->key_count() == 0) {
        index->set_key_count(1);
        index->set_value(key);
        return (PBtreeNode::InsertResult(0, slot));
      }

      // fail if the key already exists
      if (index->value() == key)
        throw Exception(HAM_DUPLICATE_KEY);

      uint32_t old_data[128];
      uint32_t new_data[128];
      uint32_t *data = &old_data[0];
      uncompress_block(index, data);

      bool needs_compress = false;

      // if the block is empty then just write the new key
      if (index->key_count() == 1) {
        // grow the block if required
        uint32_t required_bits = bits(key > index->value()
                                        ? key - index->value()
                                        : index->value() - key);
        if (required_bits > index->bits()) {
          uint32_t new_size = required_bits * 128 / 8;
          grow_block(index, new_size - index->block_size());
          index->set_bits(required_bits);
        }

        // swap |key| and |index->value| (cannot use std::swap because
        // g++ refuses swapping a packed value)
        if (key < index->value()) {
          uint32_t tmp = index->value();
          index->set_value(key);
          key = tmp;
        }

        // overwrite the first entry
        data[0] = key;
        slot += 1;

        index->set_key_count(index->key_count() + 1);

        // then compress and store the block
        compress_block(index, data);

        ham_assert(check_integrity(0, node_count + 1));
        return (PBtreeNode::InsertResult(0, slot));
      }

      // if the block is full then split it
      if (index->key_count() == SimdCompIndex::kMaxKeysPerBlock) {
        int block = index - get_block_index(0);

        // prepend the key?
        if (key < index->value()) {
          Index *new_index = add_block(block + 1,
                          SimdCompIndex::kInitialBlockSize);
          new_index->set_key_count(1);
          new_index->set_value(key);

          // swap the indices
          Index tmp = *index;
          *index = *new_index;
          *new_index = tmp;

          // then compress and store the block
          compress_block(index, new_data);
          ham_assert(check_integrity(0, node_count + 1));
          return (PBtreeNode::InsertResult(0, slot < 0 ? 0 : slot));
        }

        // append the key?
        if (key > old_data[index->key_count() - 2]) {
          Index *new_index = add_block(block + 1,
                          SimdCompIndex::kInitialBlockSize);
          new_index->set_key_count(1);
          new_index->set_value(key);

          // then compress and store the block
          compress_block(new_index, new_data);
          ham_assert(check_integrity(0, node_count + 1));
          return (PBtreeNode::InsertResult(0, slot + index->key_count()));
        }

        // otherwise split the block in the middle and move half of the keys
        // to the new block
        uint32_t to_copy = index->key_count() / 2;
        uint32_t new_key_count = index->key_count() - to_copy - 1;
        uint32_t new_value = data[to_copy];

        // once more check if the key already exists
        if (new_value == key)
          throw Exception(HAM_DUPLICATE_KEY);

        to_copy++;
        ::memmove(&new_data[0], &data[to_copy],
                        sizeof(int32_t) * (index->key_count() - to_copy));

        // calculate the required bits for the new block
        uint32_t required_bits = calc_max_bits(new_value, &new_data[0],
                                    new_key_count - 1);

        // Now create a new block. This can throw, but so far we have not
        // modified existing data.
        Index *new_index = add_block(block + 1, required_bits * 128 / 8);
        new_index->set_value(new_value);
        new_index->set_key_count(new_key_count);

        // Adjust the size of the old block
        index->set_key_count(index->key_count() - new_key_count);

        // Now check if the new key will be inserted in the old or the new block
        if (key >= new_index->value()) {
          compress_block(index, data);
          slot += index->key_count();

          // continue with the new block
          index = new_index;
          data = new_data;
        }
        else {
          compress_block(new_index, new_data);
        }

        needs_compress = true;

        // fall through...
      }

      // swap |key| and |index->value| (cannot use std::swap because
      // gcc refuses swapping a packed value)
      if (key < index->value()) {
        uint32_t tmp = index->value();
        index->set_value(key);
        key = tmp;
      }

      // locate the position of the new key
      uint32_t *begin = &data[0];
      uint32_t *end = &data[index->key_count() - 1];
      uint32_t *it = std::lower_bound(begin, end, key);

      // if the new key already exists then throw an exception
      if (it < end && *it == key) {
        if (needs_compress)
          compress_block(index, data);
        throw Exception(HAM_DUPLICATE_KEY);
      }

      // now check if the new key (its delta) requires more bits than the
      // current block uses for storage
      uint32_t required_bits;
      if (it == &data[0]) {
        ham_assert(key > index->value());
        required_bits = bits(key - index->value());
      }
      else if (it == end) {
        ham_assert(key > *(it - 1));
        required_bits = bits(key - *(it - 1));
      }
      else {
        ham_assert(*it > key);
        required_bits = bits(*it - key);
      }

      // if yes then increase the bit width
      if (required_bits > index->bits()) {
        uint32_t additional_size = (required_bits * 128 / 8)
                                    - index->block_size();
        // since grow_block() can throw: flush the modified block, otherwise
        // it's in an inconsistent state 
        if (needs_compress && get_used_size() + additional_size >= m_range_size)
          compress_block(index, data);

        grow_block(index, additional_size);
        index->set_bits(required_bits);
      }

      // insert the new key
      if (it < end)
        ::memmove(it + 1, it, (end - it) * sizeof(uint32_t));
      *it = key;
      slot += it - &data[0] + 1;

      index->set_key_count(index->key_count() + 1);

      // then compress and store the block
      compress_block(index, data);

      ham_assert(check_integrity(0, node_count + 1));
      return (PBtreeNode::InsertResult(0, slot));
    }

    // Implementation of vacuumize()
    void vacuumize_weak() {
      // This is not implemented. Caller will abort the current operation and
      // perform a page split.
      throw Exception(HAM_LIMITS_REACHED);
    }

    // Implementation of vacuumize()
    void vacuumize_full() {
      int capacity = get_block_count() * SimdCompIndex::kMaxKeysPerBlock;

      // iterate over all blocks, uncompress them into a big array
      uint32_t *p = (uint32_t *)::alloca(capacity * sizeof(uint32_t));
      uint32_t *p_end = p;

      Index *index = get_block_index(0);
      Index *end = index + get_block_count();
      for (; index < end; index++) {
        *p_end = index->value();
        p_end++;
        uncompress_block(index, p_end);
        p_end += index->key_count() - 1;
      }

      // now re-build the page
      initialize();

      index = get_block_index(0);

      // how many blocks are required?
      int required_blocks = (p_end - p) / SimdCompIndex::kMaxKeysPerBlock;
      if (required_blocks * SimdCompIndex::kMaxKeysPerBlock < p_end - p)
        required_blocks++;
      set_block_count(required_blocks);

      // Now create and fill all the blocks
      uint32_t offset = 0;
      while (p_end - p >= SimdCompIndex::kMaxKeysPerBlock) {
        uint32_t data[128];
        ::memcpy(&data[0], p + 1, sizeof(data));

        uint32_t required_bits = calc_max_bits(*p, data, 128);
        uint32_t required_size = required_bits * 128 / 8;

        index->set_bits(required_bits);
        index->set_offset(offset);
        index->set_value(*p);
        index->set_key_count(SimdCompIndex::kMaxKeysPerBlock);
        compress_block(index, data);

        offset += required_size;
        p += SimdCompIndex::kMaxKeysPerBlock;
        index++;
      }

      // only one key left? then create an empty block with an initial value
      if (p_end - p == 1) {
        index->set_value(*p);
        index->set_key_count(1);
        index->set_bits(1);
        index->set_offset(offset);
        offset += 16; // minimum block size for 1 bit
      }
      // more keys left? then create a new block and fill it
      else if (p_end - p > 1) {
        uint32_t value = *p;
        p++;
        uint32_t data[128];
        ::memcpy(&data[0], p, (p_end - p) * sizeof(uint32_t));

        uint32_t required_bits = calc_max_bits(value, data, p_end - p);
        uint32_t required_size = required_bits * 128 / 8;
        index->set_offset(offset);
        index->set_bits(required_bits);
        index->set_key_count(p_end - p + 1);
        index->set_value(value);

        compress_block(index, p);
        offset += required_size;
      }

      set_used_size(2 * sizeof(uint32_t)
                        + required_blocks * sizeof(Index)
                        + offset);
    }
};

} // namespace Zint32

} // namespace hamsterdb

#endif /* HAM_BTREE_KEYS_SIMDCOMP_H */