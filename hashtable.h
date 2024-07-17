#pragma once

#include <stddef.h>
#include <stdint.h>

struct HashNode {
  HashNode *next = NULL;
  uint64_t hcode = 0; // cached hash value
};

struct HashTable {
  HashNode **tab = NULL; // array of 'HNode *'
  size_t mask = 0; // 2^n - 1
  size_t size = 0;
};

// the real hashtable interface
// it uses 2 hashtables for progressive resizing
struct HashMap {
  HashTable ht1; // newer
  HashTable ht2; // olrder
  size_t resizing_pos = 0;
};

HashNode *hm_lookup(HashMap *hmap, HashNode *key, bool(*eq)(HashNode *, HashNode *));
void hm_insert(HashMap *hmap, HashNode *node);
HashNode *hm_pop(HashMap *hmap, HashNode *key, bool (*eq)(HashNode *, HashNode *));
size_t hm_size(HashMap *hmap);
void hm_destroy(HashMap *hmap);
