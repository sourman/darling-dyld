/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2008 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <sys/mman.h>

#include "dyld2.h"
#include "dyldLibSystemInterface.h"

//
//	 dyld allocates all memory from a private 16-byte-aligned pool for life.
//   Never hand off to gLibSystemHelpers (process heap / PartitionAlloc): bind,
//   TLV, and exception traffic must not re-enter the process allocator.
//

#if __LP64__
	// room for about ~1000 initial dylibs
	#define DYLD_POOL_CHUNK_SIZE 200*1024
#else
	// room for about ~900 initial dylibs
	#define DYLD_POOL_CHUNK_SIZE 150*1024
#endif

struct dyld_static_pool {
	dyld_static_pool*	previousPool;
	uint8_t*			current;
	uint8_t*			end;

    // libunwind, and probably others, need the pool to be 16-byte aligned as malloc guarantees that
    __attribute__((__aligned__(16)))
    uint8_t				pool[1];
};

// allocate initial pool independently of pool header to take less space on disk
static uint8_t initialPoolContent[DYLD_POOL_CHUNK_SIZE] __attribute__((__aligned__(16)));
static dyld_static_pool initialPool = { NULL, initialPoolContent, &initialPoolContent[DYLD_POOL_CHUNK_SIZE] };
static dyld_static_pool* currentPool = &initialPool;

struct dyld_oversized_block {
	dyld_oversized_block*	next;
	vm_size_t				vm_size;
};
static dyld_oversized_block* oversizedList = NULL;

static vm_size_t dyld_round_page(vm_size_t s)
{
	const vm_size_t mask = 0xFFFU;
	return (s + mask) & ~mask;
}

static void* allocate_oversized(size_t size)
{
	vm_size_t vm_size = dyld_round_page(size + sizeof(dyld_oversized_block));
	vm_address_t addr = 0;
	kern_return_t r = ::vm_allocate(mach_task_self(), &addr, vm_size, VM_FLAGS_ANYWHERE);
	if ( r != KERN_SUCCESS ) {
		dyld::halt("out of address space for dyld oversized malloc\n");
	}
	dyld_oversized_block* blk = (dyld_oversized_block*)addr;
	blk->next = oversizedList;
	blk->vm_size = vm_size;
	oversizedList = blk;
	return (void*)(blk + 1);
}


void* malloc(size_t size)
{
	// keep allocations 16-byte aligned
	size = ((size + 15) & -16);

	if ( size > DYLD_POOL_CHUNK_SIZE ) {
		return allocate_oversized(size);
	}
	size = (size+sizeof(void*)-1) & (-sizeof(void*)); // pointer align
	uint8_t* result = currentPool->current;
	currentPool->current += size;
	if ( currentPool->current > currentPool->end ) {
		vm_address_t addr = 0;
		kern_return_t r = ::vm_allocate(mach_task_self(), &addr, DYLD_POOL_CHUNK_SIZE, VM_FLAGS_ANYWHERE);
		if ( r != KERN_SUCCESS ) {
			dyld::halt("out of address space for dyld memory pool\n");
		}
		dyld_static_pool* newPool = (dyld_static_pool*)addr;
		newPool->previousPool = NULL;
		newPool->current = newPool->pool;
		newPool->end = (uint8_t*)(addr + DYLD_POOL_CHUNK_SIZE);
		newPool->previousPool = currentPool;
		currentPool = newPool;
		if ( (currentPool->current + size) > currentPool->end ) {
			return allocate_oversized(size);
		}
		result = currentPool->current;
		currentPool->current += size;
	}
	return result;
}


void free(void* ptr)
{
	if ( ptr == NULL )
		return;
	if ( (ptr >= (void*)initialPoolContent) && (ptr < (void*)&initialPoolContent[DYLD_POOL_CHUNK_SIZE]) )
		return;
	for (dyld_static_pool* p = currentPool; p != NULL; p = p->previousPool) {
		if ( p == &initialPool )
			continue;
		if ( (p->pool <= (uint8_t*)ptr) && ((uint8_t*)ptr < p->end) )
			return;
	}
	dyld_oversized_block** pp = &oversizedList;
	while ( *pp ) {
		dyld_oversized_block* blk = *pp;
		if ( (void*)(blk + 1) == ptr ) {
			*pp = blk->next;
			::vm_deallocate(mach_task_self(), (vm_address_t)blk, blk->vm_size);
			return;
		}
		pp = &blk->next;
	}
}


void* calloc(size_t count, size_t size)
{
    // Check for overflow of integer multiplication
    size_t total = count * size;
    if ( total/count != size ) {
        dyld::log("dyld calloc overflow: count=%zu, size=%zu\n", count, size);
        dyld::halt("dyld calloc overflow");
    }
	void* result = malloc(total);
	if ( result != NULL )
		bzero(result, total);
	return result;
}


void* realloc(void *ptr, size_t size)
{
	void* result = malloc(size);
#ifdef DARLING
	if (ptr) { memcpy(result, ptr, size); }
#else
	memcpy(result, ptr, size);
#endif
	return result;
}

//     void* reallocf(void *ptr, size_t size);
//     void* valloc(size_t size);

// needed __libc_init()
extern "C" int _malloc_lock;
int _malloc_lock = 0;


int vm_alloc(vm_address_t* addr, vm_size_t size, uint32_t flags)
{
	return ::vm_allocate(mach_task_self(), addr, size, flags);
}

void* xmmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	return ::mmap(addr, len, prot, flags, fd, offset);
}



