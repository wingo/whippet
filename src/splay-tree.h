// A splay tree, originally derived from Octane's `splay.js', whose
// copyright is as follows:
//
// Copyright 2009 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// The splay tree has been modified to allow nodes to store spans of
// keys, for example so that we can look up an object given any address
// pointing into that object.

#ifndef SPLAY_TREE_PREFIX
#error define SPLAY_TREE_PREFIX before including splay-tree.h
#endif

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include "gc-assert.h"

#define SPLAY___(p, n) p ## n
#define SPLAY__(p, n) SPLAY___(p, n)
#define SPLAY_(n) SPLAY__(SPLAY_TREE_PREFIX, n)

// Data types used by the splay tree.
#define SPLAY_KEY_SPAN SPLAY_(key_span)
#define SPLAY_KEY SPLAY_(key)
#define SPLAY_VALUE SPLAY_(value)

// Functions used by the splay tree.
// key_span, key -> -1|0|1
#define SPLAY_COMPARE SPLAY_(compare)
// key_span -> key
#define SPLAY_SPAN_START SPLAY_(span_start)

// Data types defined by the splay tree.
#define SPLAY_TREE SPLAY_(tree)
#define SPLAY_NODE SPLAY_(node)

// Functions defined by the splay tree.
#define SPLAY_NODE_NEW SPLAY_(node_new)
#define SPLAY_INIT SPLAY_(tree_init)
#define SPLAY_SPLAY SPLAY_(tree_splay)
#define SPLAY_PREVIOUS SPLAY_(tree_previous)
#define SPLAY_LOOKUP SPLAY_(tree_lookup)
#define SPLAY_CONTAINS SPLAY_(tree_contains)
#define SPLAY_INSERT SPLAY_(tree_insert)
#define SPLAY_REMOVE SPLAY_(tree_remove)

struct SPLAY_NODE {
  SPLAY_KEY_SPAN key;
  SPLAY_VALUE value;
  struct SPLAY_NODE *left;
  struct SPLAY_NODE *right;
};

struct SPLAY_TREE {
  struct SPLAY_NODE *root;
};

static inline struct SPLAY_NODE*
SPLAY_NODE_NEW(SPLAY_KEY_SPAN key, SPLAY_VALUE value) {
  struct SPLAY_NODE *ret = malloc(sizeof(*ret));
  if (!ret) GC_CRASH();
  ret->key = key;
  ret->value = value;
  ret->left = ret->right = NULL;
  return ret;
}

static inline void
SPLAY_INIT(struct SPLAY_TREE *tree) {
  tree->root = NULL;
}

static struct SPLAY_NODE*
SPLAY_SPLAY(struct SPLAY_TREE *tree, SPLAY_KEY key) {
  struct SPLAY_NODE *current = tree->root;
  if (!current)
    return NULL;
  // The use of the dummy node is a bit counter-intuitive: The right
  // child of the dummy node will hold the L tree of the algorithm.  The
  // left child of the dummy node will hold the R tree of the algorithm.
  // Using a dummy node, left and right will always be nodes and we
  // avoid special cases.
  struct SPLAY_NODE dummy;
  memset(&dummy, 0, sizeof(dummy));
  struct SPLAY_NODE *left = &dummy;
  struct SPLAY_NODE *right = &dummy;

loop:
  switch (SPLAY_COMPARE(key, current->key)) {
  case -1:
    if (!current->left)
      break;
    if (SPLAY_COMPARE(key, current->left->key) < 0LL) {
      // Rotate right.
      struct SPLAY_NODE *tmp = current->left;
      current->left = tmp->right;
      tmp->right = current;
      current = tmp;
      if (!current->left)
        break;
    }
    // Link right.
    right->left = current;
    right = current;
    current = current->left;
    goto loop;

  case 0:
    break;

  case 1:
    if (!current->right)
      break;
    if (SPLAY_COMPARE(key, current->right->key) > 0LL) {
      // Rotate left.
      struct SPLAY_NODE *tmp = current->right;
      current->right = tmp->left;
      tmp->left = current;
      current = tmp;
      if (!current->right)
        break;
    }
    // Link left.
    left->right = current;
    left = current;
    current = current->right;
    goto loop;

  default:
    GC_CRASH();
  }

  left->right = current->left;
  right->left = current->right;
  current->left = dummy.right;
  current->right = dummy.left;
  tree->root = current;
  return current;
}

static inline struct SPLAY_NODE*
SPLAY_PREVIOUS(struct SPLAY_NODE *node) {
  node = node->left;
  if (!node) return NULL;
  while (node->right)
    node = node->right;
  return node;
}

static inline struct SPLAY_NODE*
SPLAY_LOOKUP(struct SPLAY_TREE *tree, SPLAY_KEY key) {
  struct SPLAY_NODE *node = SPLAY_SPLAY(tree, key);
  if (node && SPLAY_COMPARE(key, node->key) == 0)
    return node;
  return NULL;
}

static inline int
SPLAY_CONTAINS(struct SPLAY_TREE *tree, SPLAY_KEY key) {
  return !!SPLAY_LOOKUP(tree, key);
}

static inline struct SPLAY_NODE*
SPLAY_INSERT(struct SPLAY_TREE* tree, SPLAY_KEY_SPAN key, SPLAY_VALUE value) {
  if (!tree->root) {
    tree->root = SPLAY_NODE_NEW(key, value);
    return tree->root;
  }
  SPLAY_KEY scalar = SPLAY_SPAN_START(key);
  struct SPLAY_NODE *node = SPLAY_SPLAY(tree, scalar);
  switch (SPLAY_COMPARE(scalar, node->key)) {
  case -1:
    node = SPLAY_NODE_NEW(key, value);
    node->right = tree->root;
    node->left = tree->root->left;
    tree->root->left = NULL;
    tree->root = node;
    break;
  case 0:
    GC_ASSERT(memcmp(&key, &node->key, sizeof(SPLAY_KEY_SPAN)) == 0);
    node->value = value;
    break;
  case 1:
    node = SPLAY_NODE_NEW(key, value);
    node->left = tree->root;
    node->right = tree->root->right;
    tree->root->right = NULL;
    tree->root = node;
    break;
  default:
    GC_CRASH();
  }
  return node;
}

static inline SPLAY_VALUE
SPLAY_REMOVE(struct SPLAY_TREE *tree, SPLAY_KEY key) {
  GC_ASSERT(tree->root);
  struct SPLAY_NODE *removed = SPLAY_SPLAY(tree, key);
  GC_ASSERT(removed);
  SPLAY_VALUE value = removed->value;
  if (!removed->left) {
    tree->root = removed->right;
  } else {
    struct SPLAY_NODE *right = removed->right;
    tree->root = removed->left;
    // Splay to make sure that the new root has an empty right child.
    SPLAY_SPLAY(tree, key);
    tree->root->right = right;
  }
  free(removed);
  return value;
}

#undef SPLAY_TREE_PREFIX
#undef SPLAY_KEY_SPAN
#undef SPLAY_KEY
#undef SPLAY_VALUE
#undef SPLAY_COMPARE
#undef SPLAY_SPAN_START
#undef SPLAY_SPANS_EQUAL
#undef SPLAY_TREE
#undef SPLAY_NODE
#undef SPLAY_NODE_NEW
#undef SPLAY_INIT
#undef SPLAY_SPLAY
#undef SPLAY_PREVIOUS
#undef SPLAY_LOOKUP
#undef SPLAY_CONTAINS
#undef SPLAY_INSERT
#undef SPLAY_REMOVE
