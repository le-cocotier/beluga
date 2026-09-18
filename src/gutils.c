#include <stdio.h>
#include <stdlib.h>

#include "../include/galloc.h"

static struct dataBlock_t * blocks = NULL;
static int blockAmount = 0;


void * galloc(size_t size){
  struct dataBlock_t * new_block = malloc(sizeof(struct dataBlock_t));
  void * data = malloc(size);
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

  if (!blocks) // First allocation
    blocks = new_block;
  else {
    new_block->lastBlock = blocks;
    blocks->nextBlock = new_block;
    blocks = new_block;
  }

  return data;
}

void * grealloc(void * address, size_t size){
  struct dataBlock_t * data_block = NULL;
  struct dataBlock_t * pBlock = blocks;
  void * new_data;

  if (!address) // realloc(NULL, size) == malloc(size); track it like any other allocation
    return galloc(size);

  // Looking for an already allocated block
  while (pBlock){
    if (pBlock->address == address) {
      data_block = pBlock;
      break;
    }
    pBlock = pBlock->lastBlock;
  }
  // Not one of ours (calloc/strdup/asprintf/getline/...) -> defer to the
  // real allocator so the existing data is preserved; it stays untracked
  // afterwards, same as before this call.
  if (!data_block)
    return realloc(address, size);

  new_data = realloc(data_block->address, size);
  if (!new_data)
    return NULL;
  data_block->address = new_data;
  data_block->size = size;

  return new_data;
}

void gfree(void * data){
  struct dataBlock_t * pBlock = blocks;
  while (pBlock != NULL){
    if (pBlock->address == data) {
      free(pBlock->address);
      blockAmount--;
      // If is the last allocated block
      if (!(pBlock->nextBlock)){
        blocks = pBlock->lastBlock;
        if (blocks)
          pBlock->lastBlock->nextBlock = NULL;
      } else {
         if (pBlock->lastBlock){ // not the first allocated
           pBlock->lastBlock->nextBlock = pBlock->nextBlock;
         }
           pBlock->nextBlock->lastBlock = pBlock->lastBlock;
      }
      free(pBlock);
      return;
    }
    pBlock = pBlock->lastBlock;
  }
  // Not one of ours (calloc/strdup/asprintf/getline/NULL/...) -> defer to
  // the real free() so nothing leaks just because it wasn't galloc'd.
  free(data);
}

void gShowMemory() {
    int i;
    struct dataBlock_t * pBlock = blocks;
    for (i = 0; i < blockAmount; ++i){
        printf("%p | %lu \r\n", pBlock->address, pBlock->size);
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
