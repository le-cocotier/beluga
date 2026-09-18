#ifndef GALLOC_H_
#define GALLOC_H_

#include <stdio.h>

void * galloc(size_t size);
void * grealloc(void * address, size_t size);
void gfree(void * data);
void gShowMemory();

/*
  Struct that contains addresses of allocated memory.
  Linked list
*/

struct dataBlock_t {
  void * address;
  struct dataBlock_t * lastBlock;
  struct dataBlock_t * nextBlock;
  size_t size;
};

// Most-recently-allocated tracked block, or NULL. Walk older blocks via
// ->lastBlock. For read-only inspection (e.g. a memory-usage buffer) —
// nothing about the list itself is safe to mutate through this pointer.
struct dataBlock_t * gBlocks(void);

// Number of blocks currently tracked (galloc/grealloc calls not yet
// matched by a gfree).
int gBlockCount(void);

#endif
