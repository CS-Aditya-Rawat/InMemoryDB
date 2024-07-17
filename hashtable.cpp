// hashtable node, should be embedded into the payload
#include "hashtable.h"
#include <assert.h>
#include <stdlib.h>

// n must be power of 2
static void h_init(HashTable *htab, size_t n) {
  assert(n > 0 && ((n - 1) & n) == 0);
  htab->tab = (HashNode **)calloc(sizeof(HashNode *), n);
  htab->mask = n - 1;
  htab->size = 0;
}

// hashtable insertion
static void h_insert(HashTable *htab, HashNode *node) {
  size_t pos = node->hcode & htab->mask; // slot index
  HashNode *next = htab->tab[pos];          // prepend the list
  node->next = next;
  htab->tab[pos] = node;
  htab->size++;
}
// hashtable look up subroutine
// Pay attention to the return value. It returns the address of
// the parent pointer that owns the target node
// which can be used to delete the target node.
static HashNode **h_lookup(HashTable *htab, HashNode *key, bool (*eq)(HashNode *, HashNode *)) {
  if (!htab->tab) {
    return NULL;
  }
  size_t pos = key->hcode & htab->mask;
  HashNode **from = &htab->tab[pos]; // incoming pointer to the result

  for (HashNode *cur; (cur = *from) != NULL; from = &cur->next) {
    if (cur->hcode == key->hcode && eq(cur, key)) {
      return from;
    }
  }
  return NULL;
}

// remove a node from the chain
static HashNode *h_detach(HashTable *htab, HashNode **from) {
  HashNode *node = *from;
  *from = node->next;
  htab->size--;
  return node;
}

const size_t k_resizing_work = 128; // constant work

static void hm_help_resizing(HashMap *hmap) {
  size_t nwork = 0;
  while (nwork < k_resizing_work && hmap->ht2.size > 0) {
    // scan for nodes from ht2 and move them to ht1
    HashNode **from = &hmap->ht2.tab[hmap->resizing_pos];
    if (!*from) {
      hmap->resizing_pos++;
      continue;
    }

    h_insert(&hmap->ht1, h_detach(&hmap->ht2, from));
    nwork++;
  }
  if (hmap->ht2.size == 0 && hmap->ht2.tab) {
    // done
    free(hmap->ht2.tab);
    hmap->ht2 = HashTable();
  }
}

static void hm_start_resizing(HashMap *hmap) {
  assert(hmap->ht2.tab == NULL);
  // create a bigger hashtable and swap them
  hmap->ht2 = hmap->ht1;
  h_init(&hmap->ht1, (hmap->ht1.mask + 1) * 2);
  hmap->resizing_pos = 0;
}

HashNode *hm_lookup(HashMap *hmap, HashNode *key, bool (*eq)(HashNode *, HashNode *)) {
  hm_help_resizing(hmap);
  HashNode **from = h_lookup(&hmap->ht1, key, eq);
  from = from ? from : h_lookup(&hmap->ht2, key, eq);
  return from ? *from : NULL;
}

const size_t k_max_load_factor = 8;

void hm_insert(HashMap *hmap, HashNode *node) {
  if (!hmap->ht1.tab) {
    h_init(&hmap->ht1, 4); // 1. Initialize the table if it is empty
  }
  h_insert(&hmap->ht1, node); // 2. Insert the key into the newer table.
  if (!hmap->ht2.tab) { // 3. Check the load factor, whether we need to resize
    size_t load_factor = hmap->ht1.size / (hmap->ht2.mask + 1);
    if (load_factor >= k_max_load_factor) {
      hm_start_resizing(hmap); // create a larger table
    }
  }
  hm_help_resizing(hmap); // 4. Move some keys into the newer table
}
HashNode *hm_pop(HashMap *hmap, HashNode *key, bool (*eq)(HashNode *, HashNode *)) {
  hm_help_resizing(hmap);
  if (HashNode **from = h_lookup(&hmap->ht1, key, eq)) {
    return h_detach(&hmap->ht1, from);
  }
  if (HashNode **from = h_lookup(&hmap->ht2, key, eq)) {
    return h_detach(&hmap->ht2, from);
  }
  return NULL;
}
size_t hm_size(HashMap *hmap) { return hmap->ht1.size + hmap->ht2.size; }

void hm_destroy(HashMap *hmap) {
  free(hmap->ht1.tab);
  free(hmap->ht1.tab);
  *hmap = HashMap();
}
