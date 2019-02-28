/*
    Title:  osomem.cpp - Interface to OS memory management

    Copyright (c) 2006, 2017-18 David C.J. Matthews

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License version 2.1 as published by the Free Software Foundation.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x)   assert(x)
#else
#define ASSERT(x)
#endif

#include "osmem.h"
#include "bitmap.h"
#include "locking.h"

// Linux prefers MAP_ANONYMOUS to MAP_ANON 
#ifndef MAP_ANON
#ifdef MAP_ANONYMOUS
#define MAP_ANON MAP_ANONYMOUS 
#endif
#endif


#ifdef POLYML32IN64

bool OSMem::Initialise(size_t space /* = 0 */, void **pBase /* = 0 */)
{
    pageSize = PageSize();
    memBase = (char*)ReserveHeap(space);
    if (memBase == 0)
        return 0;

    if (pBase != 0) *pBase = memBase;

    // Create a bitmap with a bit for each page.
    if (!pageMap.Create(space / pageSize))
        return false;
    lastAllocated = space / pageSize; // Beyond the last page in the area
    // Set the last bit in the area so that we don't use it.
    // This is effectively a work-around for a problem with the heap.
    // If we have a zero-sized cell at the end of the memory its address is
    // going to be zero.  This causes problems with forwarding pointers.
    // There may be better ways of doing this.
    pageMap.SetBit(space / pageSize - 1);
    return true;
}

void *OSMem::Allocate(size_t &space, unsigned permissions)
{
    char *baseAddr;
    {
        PLocker l(&bitmapLock);
        uintptr_t pages = (space + pageSize - 1) / pageSize;
        // Round up to an integral number of pages.
        space = pages * pageSize;
        // Find some space
        while (pageMap.TestBit(lastAllocated - 1)) // Skip the wholly allocated area.
            lastAllocated--;
        uintptr_t free = pageMap.FindFree(0, lastAllocated, pages);
        if (free == lastAllocated)
            return 0; // Can't find the space.
        pageMap.SetBits(free, pages);
        // TODO: Do we need to zero this?  It may have previously been set.
        baseAddr = memBase + free * pageSize;
    }
    return CommitPages(baseAddr, space, permissions);
}

bool OSMem::Free(void *p, size_t space)
{
    char *addr = (char*)p;
    uintptr_t offset = (addr - memBase) / pageSize;
    if (!UncommitPages(p, space))
        return false;
    uintptr_t pages = space / pageSize;
    {
        PLocker l(&bitmapLock);
        pageMap.ClearBits(offset, pages);
        if (offset + pages > lastAllocated) // We allocate from the top down.
            lastAllocated = offset + pages;
    }
    return true;
}
#endif

#if (defined(HAVE_MMAP) && defined(MAP_ANON))
// We don't use autoconf's test for mmap here because that tests for
// file mapping.  Instead the test simply tests for the presence of an mmap
// function.
// We also insist that the OS supports MAP_ANON or MAP_ANONYMOUS.  Older
// versions of Solaris required the use of /dev/zero instead.  We don't
// support that.

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

// How do we get the page size?
#ifndef HAVE_GETPAGESIZE
#ifdef _SC_PAGESIZE
#define getpagesize() sysconf(_SC_PAGESIZE)
#else
// If this fails we're stuck
#define getpagesize() PAGESIZE
#endif
#endif

#ifdef SOLARIS
#define FIXTYPE (caddr_t)
#else
#define FIXTYPE
#endif

static int ConvertPermissions(unsigned perm)
{
    int res = 0;
    if (perm & PERMISSION_READ)
        res |= PROT_READ;
    if (perm & PERMISSION_WRITE)
        res |= PROT_WRITE;
    if (perm & PERMISSION_EXEC)
        res |= PROT_EXEC;
    return res;
}

#ifdef POLYML32IN64
// Unix-specific implementation of the subsidiary functions.

size_t OSMem::PageSize()
{
    return getpagesize();
}

void *OSMem::ReserveHeap(size_t space)
{
    return mmap(0, space, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
}

bool OSMem::UnreserveHeap(void *p, size_t space)
{
    return munmap(FIXTYPE p, space) == 0;
}

void *OSMem::CommitPages(void *baseAddr, size_t space, unsigned permissions)
{
    if (mmap(baseAddr, space, ConvertPermissions(permissions), MAP_FIXED|MAP_PRIVATE|MAP_ANON, -1, 0) == MAP_FAILED)
        return 0;
    msync(baseAddr, space, MS_SYNC|MS_INVALIDATE);

    return baseAddr;
}

bool OSMem::UncommitPages(void *p, size_t space)
{
    // Remap the pages as new entries.  This should remove the old versions.
    if (mmap(p, space, PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANON, -1, 0) == MAP_FAILED)
        return false;
    msync(p, space, MS_SYNC|MS_INVALIDATE);
    return true;
}

bool OSMem::SetPermissions(void *p, size_t space, unsigned permissions)
{
    int res = mprotect(FIXTYPE p, space, ConvertPermissions(permissions));
    return res != -1;
}

#else

bool OSMem::Initialise(size_t space /* = 0 */, void **pBase /* = 0 */)
{
    pageSize = getpagesize();
    return true;
}

// Allocate space and return a pointer to it.  The size is the minimum
// size requested and it is updated with the actual space allocated.
// Returns NULL if it cannot allocate the space.
void *OSMem::Allocate(size_t &space, unsigned permissions)
{
    int prot = ConvertPermissions(permissions);
    // Round up to an integral number of pages.
    space = (space + pageSize-1) & ~(pageSize-1);
    int fd = -1; // This value is required by FreeBSD.  Linux doesn't care
    void *result = mmap(0, space, prot, MAP_PRIVATE|MAP_ANON, fd, 0);
    // Convert MAP_FAILED (-1) into NULL
    if (result == MAP_FAILED)
        return 0;
    return result;
}

// Release the space previously allocated.  This must free the whole of
// the segment.  The space must be the size actually allocated.
bool OSMem::Free(void *p, size_t space)
{
    return munmap(FIXTYPE p, space) == 0;
}

// Adjust the permissions on a segment.  This must apply to the
// whole of a segment.
bool OSMem::SetPermissions(void *p, size_t space, unsigned permissions)
{
    int res = mprotect(FIXTYPE p, space, ConvertPermissions(permissions));
    return res != -1;
}

#endif

#elif defined(_WIN32)
// Use Windows memory management.
#include <windows.h>

static int ConvertPermissions(unsigned perm)
{
    if (perm & PERMISSION_WRITE)
    {
        // Write.  Always includes read permission.
        if (perm & PERMISSION_EXEC)
            return PAGE_EXECUTE_READWRITE;
        else
            return PAGE_READWRITE;
    }
    else if (perm & PERMISSION_EXEC)
    {
        // Execute but not write.
        if (perm & PERMISSION_READ)
            return PAGE_EXECUTE_READ;
        else
            return PAGE_EXECUTE; // Execute only
    }
    else if(perm & PERMISSION_READ)
        return PAGE_READONLY;
    else 
        return PAGE_NOACCESS;
}

#ifdef POLYML32IN64

// Windows-specific implementations of the subsidiary functions.
size_t OSMem::PageSize()
{
    // Get the page size and round up to that multiple.
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    // Get the page size.  Put it in a size_t variable otherwise the rounding
    // up of "space" may go wrong on 64-bits.
    return sysInfo.dwPageSize;
}

void *OSMem::ReserveHeap(size_t space)
{
    void *memBase = VirtualAlloc(0, space, MEM_RESERVE, PAGE_NOACCESS);
    if (memBase == 0) return 0;
    // We need the heap to be such that the top 32-bits are non-zero.
    if ((uintptr_t)memBase >= ((uintptr_t)1 << 32))
        return memBase;
    // Allocate again.
    void *newSpace = ReserveHeap(space);
    UnreserveHeap(memBase, space); // Free the old area that isn't suitable.
    // Return what we got, or zero if it failed.
    return newSpace;
}

bool OSMem::UnreserveHeap(void *p, size_t space)
{
    return VirtualFree(p, 0, MEM_RELEASE) == TRUE;
}

void *OSMem::CommitPages(void *baseAddr, size_t space, unsigned permissions)
{
    return VirtualAlloc(baseAddr, space, MEM_COMMIT, ConvertPermissions(permissions));
}

bool OSMem::UncommitPages(void *baseAddr, size_t space)
{
    return VirtualFree(baseAddr, space, MEM_DECOMMIT) == TRUE;
}

bool OSMem::SetPermissions(void *p, size_t space, unsigned permissions)
{
    DWORD oldProtect;
    return VirtualProtect(p, space, ConvertPermissions(permissions), &oldProtect) == TRUE;
}

#else

bool OSMem::Initialise(size_t space /* = 0 */, void **pBase /* = 0 */)
{
    // Get the page size and round up to that multiple.
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    // Get the page size.  Put it in a size_t variable otherwise the rounding
    // up of "space" may go wrong on 64-bits.
    pageSize = sysInfo.dwPageSize;
    return true;
}

// Allocate space and return a pointer to it.  The size is the minimum
// size requested and it is updated with the actual space allocated.
// Returns NULL if it cannot allocate the space.
void *OSMem::Allocate(size_t &space, unsigned permissions)
{
    space = (space + pageSize - 1) & ~(pageSize - 1);
    DWORD options = MEM_RESERVE | MEM_COMMIT;
    return VirtualAlloc(0, space, options, ConvertPermissions(permissions));
}

// Release the space previously allocated.  This must free the whole of
// the segment.  The space must be the size actually allocated.
bool OSMem::Free(void *p, size_t space)
{
    return VirtualFree(p, 0, MEM_RELEASE) == TRUE;
}

// Adjust the permissions on a segment.  This must apply to the
// whole of a segment.
bool OSMem::SetPermissions(void *p, size_t space, unsigned permissions)
{
    DWORD oldProtect;
    return VirtualProtect(p, space, ConvertPermissions(permissions), &oldProtect) == TRUE;
}

#endif

#else

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef POLYML32IN64
#error "32 bit in 64-bits requires either mmap or VirtualAlloc"
#endif

// Use calloc to allocate the memory.  Using calloc ensures the memory is
// zeroed and is compatible with the other allocators.
void *OSMem::Allocate(size_t &bytes, unsigned permissions)
{
    return calloc(bytes, 1);
}

bool OSMem::Free(void *p, size_t/*space*/)
{
    free(p);
    return true;
}

// We can't do this if we don't have mprotect.
bool OSMem::SetPermissions(void *p, size_t space, unsigned permissions)
{
    return true; // Let's hope this is all right.
}

#endif
