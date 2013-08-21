// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef BTREE_DEPTH_FIRST_TRAVERSAL_HPP_
#define BTREE_DEPTH_FIRST_TRAVERSAL_HPP_

#include "btree/keys.hpp"
#include "btree/leaf_node.hpp"
#include "buffer_cache/types.hpp"

class btree_slice_t;
class superblock_t;
class depth_first_traversal_callback_t;

enum class direction_t {
    FORWARD,
    BACKWARD
};

class pair_batch_t {
public:
    std::pair<const btree_key_t *, const void *> next();

private:
    friend bool btree_depth_first_traversal(btree_slice_t *slice, transaction_t *transaction,
                                            buf_lock_t *block, const key_range_t &range,
                                            depth_first_traversal_callback_t *cb, direction_t direction);

    pair_batch_t(direction_t direction, leaf::iterator beg, leaf::iterator end)
        : direction_(direction), beg_(beg), end_(end) { }

    const direction_t direction_;
    leaf::iterator beg_;
    leaf::iterator end_;
};

class depth_first_traversal_callback_t {
public:
    /* Return value of `true` indicates to keep going; `false` indicates to stop
    traversing the tree. */
    virtual bool handle_pair(const btree_key_t *key, const void *value) = 0;
protected:
    virtual ~depth_first_traversal_callback_t() { }
};

/* Returns `true` if we reached the end of the btree or range, and `false` if
`cb->handle_value()` returned `false`. */
bool btree_depth_first_traversal(btree_slice_t *slice, transaction_t *transaction,
        superblock_t *superblock, const key_range_t &range,
        depth_first_traversal_callback_t *cb, direction_t direction);

/* Returns `true` if we reached the end of the subtree or range, and `false` if
`cb->handle_value()` returned `false`. */
bool btree_depth_first_traversal(btree_slice_t *slice, transaction_t *transaction,
        buf_lock_t *block, const key_range_t &range,
        depth_first_traversal_callback_t *cb, direction_t direction);


#endif /* BTREE_DEPTH_FIRST_TRAVERSAL_HPP_ */
