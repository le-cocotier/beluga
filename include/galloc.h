#ifndef GALLOC_H_
#define GALLOC_H_

#include <stdio.h>

// galloc/grealloc are function-like macros so every call site automatically
// records where it allocated from — no changes needed at any of the ~270
// call sites across the codebase. Call the _impl functions directly only
// from inside gutils.c itself (see grealloc_impl's NULL-address fallback),
// where re-expanding the macro would attribute the allocation to gutils.c
// instead of the real caller.
void * galloc_impl(size_t size, const char * file, int line, const char * func);
void * gcalloc_impl(size_t num, size_t size, const char * file, int line, const char * func);
void * grealloc_impl(void * address, size_t size, const char * file, int line, const char * func);
void gfree(void * data);
void gShowMemory();

#define galloc(size) galloc_impl((size), __FILE__, __LINE__, __func__)
#define gcalloc(num, size) gcalloc_impl((num), (size), __FILE__, __LINE__, __func__)
#define grealloc(address, size) grealloc_impl((address), (size), __FILE__, __LINE__, __func__)

/*
  Struct that contains addresses of allocated memory.
  Linked list
*/

struct dataBlock_t {
  void * address;
  struct dataBlock_t * lastBlock;
  struct dataBlock_t * nextBlock;
  size_t size;
  const char * file;   // __FILE__ at the (re)allocation call site
  int line;             // __LINE__ at the (re)allocation call site
  const char * func;    // __func__ at the (re)allocation call site
  struct dataBlock_t * hashNext; // gutils.c's address index; not for inspection
};

// Most-recently-allocated tracked block, or NULL. Walk older blocks via
// ->lastBlock. For read-only inspection (e.g. a memory-usage buffer) —
// nothing about the list itself is safe to mutate through this pointer.
struct dataBlock_t * gBlocks(void);

// Number of blocks currently tracked (galloc/grealloc calls not yet
// matched by a gfree).
int gBlockCount(void);

#endif
