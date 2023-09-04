#define _DEFAULT_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "mem_internals.h"
#include "mem.h"
#include "util.h"

void debug_block(struct block_header* b, const char* fmt, ... );
void debug(const char* fmt, ... );

extern inline block_size size_from_capacity( block_capacity cap );
extern inline block_capacity capacity_from_size( block_size sz );

static bool            block_is_big_enough( size_t query, struct block_header* block ) { return block->capacity.bytes >= query; }
static size_t          pages_count   ( size_t mem )                      { return mem / getpagesize() + ((mem % getpagesize()) > 0); }
static size_t          round_pages   ( size_t mem )                      { return getpagesize() * pages_count( mem ) ; }

static void block_init( void* restrict addr, block_size block_sz, void* restrict next ) {
  *((struct block_header*)addr) = (struct block_header) {
    .next = next,
    .capacity = capacity_from_size(block_sz),
    .is_free = true
  };
}

static size_t region_actual_size( size_t query ) { return size_max( round_pages( query ), REGION_MIN_SIZE ); }

extern inline bool region_is_invalid( const struct region* r );



static void* map_pages(void const* addr, size_t length, int additional_flags) {
  return mmap( (void*) addr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | additional_flags , -1, 0 );
}

/*  аллоцировать регион памяти и инициализировать его блоком */
static struct region alloc_region  ( void const * addr, size_t query ) {
    size_t actualSize = region_actual_size(size_from_capacity( (block_capacity){query}).bytes );
    void* mappingAddr = map_pages(addr,actualSize,MAP_FIXED_NOREPLACE);
//    void* resultAddr = NULL;
    if (mappingAddr == MAP_FAILED) mappingAddr = map_pages(addr,actualSize,0);
    if (mappingAddr == MAP_FAILED) mappingAddr = NULL;
    struct region region = (struct region) {mappingAddr, actualSize, mappingAddr==addr};
    if ( !region_is_invalid(&region) ) block_init(mappingAddr, (block_size) {region.size}, NULL);
    return region;
}

static void* block_after( struct block_header const* block )         ;

void* heap_init( size_t initial ) {
  const struct region region = alloc_region( HEAP_START, initial );
  if ( region_is_invalid(&region) ) return NULL;

  return region.addr;
}

#define BLOCK_MIN_CAPACITY 24

/*  --- Разделение блоков (если найденный свободный блок слишком большой )--- */

static bool block_splittable( struct block_header* restrict block, size_t query) {
  return block-> is_free && query + offsetof( struct block_header, contents ) + BLOCK_MIN_CAPACITY <= block->capacity.bytes;
}

//если можно рассплитить, инициализируем лишний блок, а старый ограничиваем только необходимой памятью.
static bool split_if_too_big( struct block_header* block, size_t query ) {
  if ( !block_splittable(block, query)) return false;
  //указатель на конец необходимой памяти
  void* ad = (*block).contents + query;
  //инициализируем блок памяти после нужной нам, размер - всего байтов в исходном splittable блоке - то
  //количество байт, что на нужно, указатель на следующий блок передаём из исиходного.
  block_init( ad, (block_size) { (*block).capacity.bytes - query }, (*block).next);
  //устанавливаем размер нужного нам блока на необходимый
  (*block).capacity.bytes = query;
  //ставим в ссылку на следующий блок наш "фришный"
  (*block).next = ad;
  return true;
}


/*  --- Слияние соседних свободных блоков --- */

static void* block_after( struct block_header const* block )              {
  return  (void*) (block->contents + block->capacity.bytes);
}
static bool blocks_continuous (
                               struct block_header const* fst,
                               struct block_header const* snd ) {
  return (void*)snd == block_after(fst);
}

static bool mergeable(struct block_header const* restrict fst, struct block_header const* restrict snd) {
  return fst->is_free && snd->is_free && blocks_continuous( fst, snd ) ;
}

static bool try_merge_with_next( struct block_header* block ) {
    if ( block->next == NULL || !mergeable(block, block->next) ) return false;
    (*block).capacity.bytes += size_from_capacity( (*block).next->capacity).bytes;
    (*block).next = (*block).next->next;
    return true;
}


/*  --- ... ecли размера кучи хватает --- */

struct block_search_result {
  enum {BSR_FOUND_GOOD_BLOCK, BSR_REACHED_END_NOT_FOUND, BSR_CORRUPTED} type;
  struct block_header* block;
};


static struct block_search_result find_good_or_last  ( struct block_header* restrict block, size_t sz )    {
    if (block == NULL) return (struct block_search_result) {BSR_CORRUPTED, NULL};
    while (block) {
        if ( (*block).is_free ) {
            bool mergeResult = true;
            while (mergeResult) {
                mergeResult = try_merge_with_next(block);
            }
            if (block_is_big_enough(sz, block)) {
                return (struct block_search_result) {BSR_FOUND_GOOD_BLOCK, block};
            }
        }
        if ( (*block).next == NULL) break;
        block = (*block).next;
    }
    return (struct block_search_result) {BSR_REACHED_END_NOT_FOUND, block};

}

/*  Попробовать выделить память в куче начиная с блока `block` не пытаясь расширить кучу
 Можно переиспользовать как только кучу расширили. */
static struct block_search_result try_memalloc_existing ( size_t query, struct block_header* block ) {
    struct block_search_result blockSearchResult = find_good_or_last(block, query);
    if (blockSearchResult.type == BSR_FOUND_GOOD_BLOCK) {
        split_if_too_big(blockSearchResult.block, query);
        blockSearchResult.block->is_free = false;
    }
    return blockSearchResult;
}



static struct block_header* grow_heap( struct block_header* restrict last, size_t query ) {
    struct region region = alloc_region((*last).contents + (*last).capacity.bytes, query);
    if ( !region_is_invalid(&region) && region.extends && (*last).is_free ) {
        (*last).capacity.bytes += region.size;
        return last;
    }
    (*last).next = region.addr;
    return region.addr;
}

/*  Реализует основную логику malloc и возвращает заголовок выделенного блока */
static struct block_header* memalloc( size_t query, struct block_header* heap_start) {
    size_t actualQuery = size_max(query, BLOCK_MIN_CAPACITY);
    struct block_search_result blockSearchResult = try_memalloc_existing(actualQuery, heap_start);
    if (blockSearchResult.type == BSR_FOUND_GOOD_BLOCK) return blockSearchResult.block;
    if (blockSearchResult.type == BSR_REACHED_END_NOT_FOUND && blockSearchResult.block != NULL) {
        blockSearchResult.block = grow_heap(blockSearchResult.block, actualQuery);
        blockSearchResult = try_memalloc_existing( actualQuery, blockSearchResult.block);
        return blockSearchResult.block;
    }
    return NULL;
}

void* _malloc( size_t query ) {
  struct block_header* const addr = memalloc( query, (struct block_header*) HEAP_START );
  if (addr) return addr->contents;
  else return NULL;
}

struct block_header* block_get_header(void* contents) {
  return (struct block_header*) (((uint8_t*)contents)-offsetof(struct block_header, contents));
}

void _free( void* mem ) {
  if (!mem) return ;
  struct block_header* header = block_get_header( mem );
  header->is_free = true;
  bool mergeResult = true;
  while (mergeResult) {
      mergeResult = try_merge_with_next(header);
  }
}
