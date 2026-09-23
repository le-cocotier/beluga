#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../include/galloc.h"

static struct dataBlock_t * blocks = NULL;
static int blockAmount = 0;

// ─── Address index ──────────────────────────────────────────────────────────
// The list above is kept for ordered inspection (gBlocks(), the memory
// inspector), but finding a block by address through it meant walking from
// the newest block back: a row allocated early in a 50k-line file sits ~50k
// nodes deep, so every keystroke on it paid that walk, and opening the file
// paid it once per row. This chained hash table makes grealloc/gfree O(1).
// It's allocated with the raw allocator — tracking the tracker makes no sense.

static struct dataBlock_t ** g_index = NULL;
static size_t g_index_size = 0; // a power of two, or 0 if never allocated

static size_t indexSlot(const void * address, const size_t size) {
  uint64_t x = (uint64_t) (uintptr_t) address;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (size_t) x & (size - 1);
}

// Rebuilds the index at twice the size, from the list itself — which also
// picks up any block inserted while a previous grow had failed.
static void indexGrow(void) {
  const size_t new_size = g_index_size ? g_index_size * 2 : 1024;
  struct dataBlock_t ** table = calloc(new_size, sizeof(*table));
  struct dataBlock_t * p;
  size_t slot;
  if (!table)
    return; // keep the old index (or none: findBlock falls back to the list)
  for (p = blocks; p; p = p->lastBlock) {
    slot = indexSlot(p->address, new_size);
    p->hashNext = table[slot];
    table[slot] = p;
  }
  free(g_index);
  g_index = table;
  g_index_size = new_size;
}

static void indexInsert(struct dataBlock_t * block) {
  size_t slot;
  if ((size_t) blockAmount > g_index_size) {
    indexGrow(); // rehashes every listed block, `block` included
    if (g_index && (size_t) blockAmount <= g_index_size)
      return;
  }
  if (!g_index)
    return;
  slot = indexSlot(block->address, g_index_size);
  block->hashNext = g_index[slot];
  g_index[slot] = block;
}

static void indexRemove(const struct dataBlock_t * block) {
  struct dataBlock_t ** link;
  if (!g_index)
    return;
  link = &g_index[indexSlot(block->address, g_index_size)];
  while (*link) {
    if (*link == block) {
      *link = block->hashNext;
      return;
    }
    link = &(*link)->hashNext;
  }
}

static struct dataBlock_t * findBlock(const void * address) {
  struct dataBlock_t * p;
  if (g_index) {
    p = g_index[indexSlot(address, g_index_size)];
    for (; p; p = p->hashNext)
      if (p->address == address)
        return p;
    return NULL;
  }
  for (p = blocks; p; p = p->lastBlock)
    if (p->address == address)
      return p;
  return NULL;
}

// Links a freshly allocated `data` into the list and the index.
static void * track(void * data, const size_t size, const char * file, const int line, const char * func) {
  struct dataBlock_t * new_block = malloc(sizeof(struct dataBlock_t));
  if (!new_block || !data) { // Allocation failed
    free(new_block);
    free(data);
    return NULL;
  }

  blockAmount++;
  new_block->address = data;
  new_block->size = size;
  new_block->lastBlock = NULL;
  new_block->nextBlock = NULL;
  new_block->hashNext = NULL;
  new_block->file = file;
  new_block->line = line;
  new_block->func = func;

  if (!blocks) // First allocation
    blocks = new_block;
  else {
    new_block->lastBlock = blocks;
    blocks->nextBlock = new_block;
    blocks = new_block;
  }
  indexInsert(new_block);

  return data;
}

void * galloc_impl(const size_t size, const char * file, const int line, const char * func){
  return track(malloc(size), size, file, line, func);
}

void * gcalloc_impl(const size_t num, const size_t size, const char * file, const int line, const char * func){
  return track(calloc(num, size), num * size, file, line, func);
}

void * grealloc_impl(void * address, const size_t size, const char * file, const int line, const char * func){
  struct dataBlock_t * data_block;
  void * new_data;
  // realloc(NULL, size) == malloc(size); track it like any other
  // allocation. Calls galloc_impl directly (not the galloc() macro) so the
  // block is attributed to the original caller's site, not this line.
  if (!address)
    return galloc_impl(size, file, line, func);

  data_block = findBlock(address);
  // Not one of ours (calloc/strdup/asprintf/getline/...) -> defer to the
  // real allocator so the existing data is preserved; it stays untracked
  // afterwards, same as before this call.
  if (!data_block)
    return realloc(address, size);

  new_data = realloc(data_block->address, size);
  if (!new_data)
    return NULL;
  if (new_data != data_block->address) {
    indexRemove(data_block); // keyed by the old address
    data_block->address = new_data;
    indexInsert(data_block);
  }
  data_block->size = size;
  data_block->file = file;
  data_block->line = line;
  data_block->func = func;

  return new_data;
}

void gfree(void * data){
  struct dataBlock_t * pBlock = data ? findBlock(data) : NULL;
  if (!pBlock) {
    // Not one of ours (calloc/strdup/asprintf/getline/NULL/...) -> defer to
    // the real free() so nothing leaks just because it wasn't galloc'd.
    free(data);
    return;
  }

  indexRemove(pBlock);
  free(pBlock->address);
  blockAmount--;
  // If is the last allocated block
  if (!(pBlock->nextBlock)){
    blocks = pBlock->lastBlock;
    if (blocks)
      pBlock->lastBlock->nextBlock = NULL;
  } else {
    if (pBlock->lastBlock) // not the first allocated
      pBlock->lastBlock->nextBlock = pBlock->nextBlock;
    pBlock->nextBlock->lastBlock = pBlock->lastBlock;
  }
  free(pBlock);
}

void gShowMemory() {
    int i;
    const struct dataBlock_t * pBlock = blocks;
    for (i = 0; i < blockAmount; ++i){
        printf("%p | %lu | %s:%d (%s) \r\n", pBlock->address, pBlock->size,
               pBlock->file, pBlock->line, pBlock->func);
        printf("------------------------\r\n");
        pBlock = pBlock->lastBlock;
    }
}

struct dataBlock_t * gBlocks(void) {
  return blocks;
}

int gBlockCount(void) {
  return blockAmount;
}
