/*
    Title:      Multi-Threaded Garbage Collector - Data sharing phase

    Copyright (c) 2012, 2017, 2019 David C. J. Matthews

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

/*  GC Sharing pass.
    This pass is invoked only if the heap sizing code detects that heap space
    is running very short because it adds a very considerable overhead to GC.
    It aims to reduce the size of the live data in a similar way to the data
    sharing function PolyML.shareCommonData by merging immutable cells that
    contain data that cannot be distinguished.

    This version of the code now does a deep structure merge in a similar
    way to the full sharing function. This code first does a full pass
    over the heap creating lists of cells that could possibly be merged.
    There are separate lists for byte and word objects up to a fixed
    size.  Larger objects and other objects are not considered. Because
    all the items in a list have the same length and type (flag bits)
    we can use the length word to link the items in the list.  A
    consequence of this is that positive long precision values can be
    shared but negative values cannot.  

    There is a sharing function that first distributes items into a
    hash table.  Then each hash table is sorted and as part of the
    sorting process cells with the same contents are merged.  One
    cell is chosen and the length words on the others are set to be
    forwarding pointers to the chosen cell.  Hashing allows for easy
    parallel processing.

    The structure sharing code works by first sharing the byte
    data which cannot contain pointers.  Then the word data is processed
    to separate out "tail" cells that contain only tagged integers or
    pointers to cells that either cannot be merged, such as mutables,
    or those that have already been processed, such as the byte data.
    Any pointers to shared data are updated to point to the merged cell.
    The tail cells are then sorted and shared using the sharing function
    and become part of the "processed" set.  This process is repeated to
    find cells that are now tails and so on.

    Compared with the full sharing code this is expensive since it
    requires repeated scans of the list of unprocessed cells.  In
    particular there may be cells that form loops (basically closures
    for mutually recusive functions) and if they are present they and
    anything that points directly or indirectly at them will never
    be removed from the list.  We stop when it appears that we are
    not making progress and simply do a final bit-wise share of the
    remainder.

    This now uses the forwarding pointer both to indicate that a cell
    shares with another and also to link together cells that have yet
    to be tested for sharing.  To detect the difference the bitmap is
    used.  The initial scan to create the sharing chains sets the bit
    for each visited cell so at the start of the sharing phase all
    reachable cells will be marked.  We remove the mark if the cell
    is to be removed.  This requires the bitmap to be locked.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x)   assert(x)
#else
#define ASSERT(x)
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include "globals.h"
#include "processes.h"
#include "gc.h"
#include "scanaddrs.h"
#include "bitmap.h"
#include "memmgr.h"
#include "diagnostics.h"
#include "gctaskfarm.h"
#include "heapsizing.h"

#ifdef POLYML32IN64
#define ENDOFLIST ((PolyObject*)globalHeapBase)
#else
#define ENDOFLIST 0
#endif

// Set the forwarding so that references to objToSet will be forwarded to
// objToShare.  objToSet will be garbage.
void shareWith(PolyObject *objToSet, PolyObject *objToShare)
{
    // We need to remove the bit from this so that we know it's not
    // a share chain.
    PolyWord *lengthWord = ((PolyWord*)objToSet) - 1;
    LocalMemSpace *space = gMem.LocalSpaceForAddress(lengthWord);
    ASSERT(space);
    PLocker locker(&space->bitmapLock);
    ASSERT(space->bitmap.TestBit(space->wordNo(lengthWord)));
    space->bitmap.ClearBit(space->wordNo(lengthWord));
    // Actually do the forwarding
    objToSet->SetForwardingPtr(objToShare);
}

// When we find an address it could be a cell that:
// 1. is never processed or one that is the copy to be retained,
// 2. has been merged with another and contains a forwarding pointer or
// 3. has not yet been processed.
typedef enum { REALOBJECT, FORWARDED, CHAINED } objectState;

objectState getObjectState(PolyObject *p)
{
    PolyWord *lengthWord = ((PolyWord*)p) - 1;
    LocalMemSpace *space = gMem.LocalSpaceForAddress(lengthWord);
    if (space == 0)
        return REALOBJECT; // May be the address of a permanent or something else.
    PLocker locker(&space->bitmapLock);
    if (!p->ContainsForwardingPtr())
        return REALOBJECT;
    if (space->bitmap.TestBit(space->wordNo(lengthWord)))
        return CHAINED;
    else return FORWARDED;

}

class ObjEntry
{
public:
    ObjEntry(): objList(ENDOFLIST), objCount(0), shareCount(0) {}
    PolyObject *objList;
    POLYUNSIGNED objCount;
    POLYUNSIGNED shareCount;
};

// There is an instance of this class for each combination of size and
// word/byte.
class SortVector
{
public:
    SortVector(): totalCount(0), carryOver(0) {}
    void AddToVector(PolyObject *obj, POLYUNSIGNED length);
    void SortData(void);
    POLYUNSIGNED TotalCount() const { return totalCount; }
    POLYUNSIGNED CurrentCount() const { return baseObject.objCount; }
    POLYUNSIGNED Shared() const;
    void SetLengthWord(POLYUNSIGNED l) { lengthWord = l; }
    POLYUNSIGNED CarryOver() const { return carryOver; }

    static void hashAndSortAllTask(GCTaskId*, void *a, void *b);
    static void sharingTask(GCTaskId*, void *a, void *b);
    static void wordDataTask(GCTaskId*, void *a, void *b);

private:
    void sortList(PolyObject *head, POLYUNSIGNED nItems, POLYUNSIGNED &count);

    ObjEntry baseObject, processObjects[256];
    POLYUNSIGNED totalCount;
    POLYUNSIGNED lengthWord;
    POLYUNSIGNED carryOver;
};

POLYUNSIGNED SortVector::Shared() const
{
    // Add all the sharing counts
    POLYUNSIGNED shareCount = baseObject.shareCount;
    for (unsigned i = 0; i < 256; i++)
        shareCount += processObjects[i].shareCount;
    return shareCount;
}

void SortVector::AddToVector(PolyObject *obj, POLYUNSIGNED length)
{
    obj->SetForwardingPtr(baseObject.objList);
    baseObject.objList = obj;
    baseObject.objCount++;
    totalCount++;
}

// The number of byte and word entries.
// Objects of up to and including this size are shared.
// Byte objects include strings so it is more likely that
// larger objects will share.  Word objects that share
// are much more likely to be 2 or 3 words.
#define NUM_BYTE_VECTORS    23
#define NUM_WORD_VECTORS    11

class GetSharing: public RecursiveScanWithStack
{
public:
    GetSharing();
    void SortData(void);
    static void shareByteData(GCTaskId *, void *, void *);
    static void shareWordData(GCTaskId *, void *, void *);
    static void shareRemainingWordData(GCTaskId *, void *, void *);

    virtual PolyObject *ScanObjectAddress(PolyObject *obj);

protected:
    virtual bool TestForScan(PolyWord *);
    virtual void MarkAsScanning(PolyObject *);
    virtual void StackOverflow(void) { } // Ignore stack overflow
    virtual void Completed(PolyObject *);

private:
    // The head of chains of cells of the same size
    SortVector byteVectors[NUM_BYTE_VECTORS];
    SortVector wordVectors[NUM_WORD_VECTORS];

    POLYUNSIGNED largeWordCount, largeByteCount, excludedCount;
public:
    POLYUNSIGNED totalVisited, byteAdded, wordAdded, totalSize;
};

GetSharing::GetSharing()
{
    for (unsigned i = 0; i < NUM_BYTE_VECTORS; i++)
        byteVectors[i].SetLengthWord((POLYUNSIGNED)i | _OBJ_BYTE_OBJ);

    for (unsigned j = 0; j < NUM_WORD_VECTORS; j++)
        wordVectors[j].SetLengthWord(j);

    largeWordCount = largeByteCount = excludedCount = 0;
    totalVisited = byteAdded = wordAdded = totalSize = 0;
}

// This is called for roots and also for constants in the constant area.
// If we have a code address we MUSTN't call RecursiveScan::ScanObjectAddress
// because that turns the address into a PolyWord and doesn't work in 32-in-64.
// We process the code area explicitly so we can simply skip code addresses.
PolyObject *GetSharing::ScanObjectAddress(PolyObject *obj)
{
    LocalMemSpace *sp = gMem.LocalSpaceForAddress((PolyWord*)obj - 1);

    if (sp == 0)
        return obj;

    return RecursiveScanWithStack::ScanObjectAddress(obj);
}

bool GetSharing::TestForScan(PolyWord *pt)
{
    PolyObject *obj;

    // This may be a forwarding pointer left over from a minor GC that did
    // not complete or it may be a sharing chain pointer that we've set up.
    while (1)
    {
        PolyWord p = *pt;
        ASSERT(p.IsDataPtr());
        obj = p.AsObjPtr();
        PolyWord *lengthWord = ((PolyWord*)obj) - 1;
        LocalMemSpace *space = gMem.LocalSpaceForAddress(lengthWord);
        if (space == 0)
            return false; // Ignore it if it points to a permanent area

        if (space->bitmap.TestBit(space->wordNo(lengthWord)))
            return false;

        // Wasn't marked - must be a forwarding pointer.
        if (obj->ContainsForwardingPtr())
        {
            obj = obj->GetForwardingPtr();
            *pt = obj;
        }
        else break;
    }

    ASSERT(obj->ContainsNormalLengthWord());

    totalVisited += 1;
    totalSize += obj->Length() + 1;

    return true;
}

void GetSharing::MarkAsScanning(PolyObject *obj)
{
    ASSERT(obj->ContainsNormalLengthWord());
    PolyWord *lengthWord = ((PolyWord*)obj) - 1;
    LocalMemSpace *space = gMem.LocalSpaceForAddress(lengthWord);
    ASSERT(! space->bitmap.TestBit(space->wordNo(lengthWord)));
    space->bitmap.SetBit(space->wordNo(lengthWord));
}

void GetSharing::Completed(PolyObject *obj)
{
    // We mustn't include cells in the permanent area.
    // We scan the permanent mutable areas for local addresses
    // but we mustn't add the cells themselves.  Normally they
    // will be mutable so would be ignored but cells that have been
    // locked will now be immutable.  The test in TestForScan is bypassed
    // by ScanAddressesInRegion.
    PolyWord *lengthWord = ((PolyWord*)obj) - 1;
    if (gMem.LocalSpaceForAddress(lengthWord) == 0)
        return;

    POLYUNSIGNED L = obj->LengthWord();
    // We have tables for word objects and byte objects
    // We chain entries together using the length word so it
    // is important that we only do this for objects that
    // have no other bits in the header, such as the sign bit.
    if ((L & _OBJ_PRIVATE_FLAGS_MASK) == 0)
    {
        POLYUNSIGNED length = obj->Length();
        if (length < NUM_WORD_VECTORS)
            wordVectors[length].AddToVector(obj, length);
        else largeWordCount++;
        wordAdded++;
    }
    else if ((L & _OBJ_PRIVATE_FLAGS_MASK) == _OBJ_BYTE_OBJ)
    {
        POLYUNSIGNED length = obj->Length();
        if (length < NUM_BYTE_VECTORS)
            byteVectors[length].AddToVector(obj, length);
        else largeByteCount++;
        byteAdded++;
    }
    else if (! OBJ_IS_CODE_OBJECT(L) && ! OBJ_IS_MUTABLE_OBJECT(L))
        excludedCount++; // Code and mutables can't be shared - see what could be
    // TODO: We don't attempt to share closure cells in 32-in-64.
}

// Quicksort the list to detect cells with the same content.  These are made
// to share and removed from further sorting.
void SortVector::sortList(PolyObject *head, POLYUNSIGNED nItems, POLYUNSIGNED &shareCount)
{
    while (nItems > 2)
    {
        size_t bytesToCompare = OBJ_OBJECT_LENGTH(lengthWord)*sizeof(PolyWord);
        PolyObject *median = head;
        head = head->GetForwardingPtr();
        median->SetLengthWord(lengthWord);
        PolyObject *left = ENDOFLIST, *right = ENDOFLIST;
        POLYUNSIGNED leftCount = 0, rightCount = 0;
        while (head != ENDOFLIST)
        {
            PolyObject *next = head->GetForwardingPtr();
            int res = memcmp(median, head, bytesToCompare);
            if (res == 0)
            {
                // Equal - they can share
                shareWith(head, median);
                shareCount++;
            }
            else if (res < 0)
            {
                head->SetForwardingPtr(left);
                left = head;
                leftCount++;
            }
            else
            {
                head->SetForwardingPtr(right);
                right = head;
                rightCount++;
            }
            head = next;
        }
        // We can now drop the median and anything that shares with it.
        // Process the smaller partition recursively and the larger by
        // tail recursion.
        if (leftCount < rightCount)
        {
            sortList(left, leftCount, shareCount);
            head = right;
            nItems = rightCount;
        }
        else
        {
            sortList(right, rightCount, shareCount);
            head = left;
            nItems = leftCount;
        }
    }
    if (nItems == 1)
        head->SetLengthWord(lengthWord);
    else if (nItems == 2)
    {
        PolyObject *next = head->GetForwardingPtr();
        head->SetLengthWord(lengthWord);
        if (memcmp(head, next, OBJ_OBJECT_LENGTH(lengthWord)*sizeof(PolyWord)) == 0)
        {
            shareWith(next, head);
            shareCount++;
        }
        else next->SetLengthWord(lengthWord);
    }
}

void SortVector::sharingTask(GCTaskId*, void *a, void *b)
{
    SortVector *s = (SortVector *)a;
    ObjEntry *o = (ObjEntry*)b;
    s->sortList(o->objList, o->objCount, o->shareCount);
}

// Process one level of the word data.
// N.B.  The length words are updated without any locking.  This is safe
// because all length words are initially chain entries and a chain entry
// can be replaced by another chain entry, a forwarding pointer or a normal
// length word.  Forwarding pointers and normal length words are only ever
// set once.  There is a small chance that we could lose some sharing as a
// result of a race condition if a thread defers an object because it
// contains a pointer with a chain entry and later sees an otherwise
// equal object where another thread has replaced the chain with a
// normal address, adds it to the list for immediate processing and
// so never compares the two.
void SortVector::wordDataTask(GCTaskId*, void *a, void *)
{
    SortVector *s = (SortVector*)a;
    // Partition the objects between those that have pointers to objects that are
    // still to be processed and those that have been processed.
    if (s->baseObject.objList == ENDOFLIST)
        return;
    PolyObject *h = s->baseObject.objList;
    s->baseObject.objList = ENDOFLIST;
    s->baseObject.objCount = 0;
    POLYUNSIGNED words = OBJ_OBJECT_LENGTH(s->lengthWord);
    s->carryOver = 0;

    for (unsigned i = 0; i < 256; i++)
    {
        // Clear the entries in the hash table but not the sharing count.
        s->processObjects[i].objList = ENDOFLIST;
        s->processObjects[i].objCount = 0;
    }

    while (h != ENDOFLIST)
    {
        PolyObject *next = h->GetForwardingPtr();
        bool deferred = false;
        for (POLYUNSIGNED i = 0; i < words; i++)
        {
            PolyWord w = h->Get(i);
            if (w.IsDataPtr())
            {
                PolyObject *p = w.AsObjPtr();
                objectState state = getObjectState(p);
                if (state == FORWARDED)
                {
                    // Update the addresses of objects that have been merged
                    h->Set(i, p->GetForwardingPtr());
                    s->carryOver++;
                    break;
                }
                else if (state == CHAINED)
                {
                    // If it is still to be shared leave it
                    deferred = true;
                    break; // from the loop
                }
            }
        }
        if (deferred)
        {
            // We can't do it yet: add it back to the list
            h->SetForwardingPtr(s->baseObject.objList);
            s->baseObject.objList = h;
            s->baseObject.objCount++;
        }
        else
        {
            // Add it to the hash table.
            unsigned char hash = 0;
            for (POLYUNSIGNED i = 0; i < words*sizeof(PolyWord); i++)
                hash += h->AsBytePtr()[i];
            h->SetForwardingPtr(s->processObjects[hash].objList);
            s->processObjects[hash].objList = h;
            s->processObjects[hash].objCount++;
        }
        h = next;
    }
    s->SortData();
}

// Sort the entries in the hash table.
void SortVector::SortData()
{
    for (unsigned j = 0; j < 256; j++)
    {
        ObjEntry *oentry = &processObjects[j];
        // Sort this entry.  If it's very small just process it now.
        switch (oentry->objCount)
        {
        case 0: break; // Nothing there

        case 1: // Singleton - just restore the length word
            oentry->objList->SetLengthWord(lengthWord);
            break;

        case 2:
            {
                // Two items - process now
                PolyObject *obj1 = oentry->objList;
                PolyObject *obj2 = obj1->GetForwardingPtr();
                obj1->SetLengthWord(lengthWord);
                if (memcmp(obj1, obj2, OBJ_OBJECT_LENGTH(lengthWord)*sizeof(PolyWord)) == 0)
                {
                    shareWith(obj2, obj1);
                    oentry->shareCount++;
                }
                else obj2->SetLengthWord(lengthWord);
                break;
            }

        default:
            gpTaskFarm->AddWorkOrRunNow(sharingTask, this, oentry);
        }
    }
}

void SortVector::hashAndSortAllTask(GCTaskId*, void *a, void *b)
{
    SortVector *s = (SortVector *)a;
    // Hash the contents of the base object then sort them.
    for (unsigned i = 0; i < 256; i++)
    {
        // Clear the entries in the hash table but not the sharing count.
        s->processObjects[i].objList = ENDOFLIST;
        s->processObjects[i].objCount = 0;
    }
    PolyObject *h = s->baseObject.objList;
    POLYUNSIGNED bytes = OBJ_OBJECT_LENGTH(s->lengthWord)*sizeof(PolyWord);
    while (h != ENDOFLIST)
    {
        PolyObject *next = h->GetForwardingPtr();
        unsigned char hash = 0;
        for (POLYUNSIGNED j = 0; j < bytes; j++)
            hash += h->AsBytePtr()[j];
        h->SetForwardingPtr(s->processObjects[hash].objList);
        s->processObjects[hash].objList = h;
        s->processObjects[hash].objCount++;
        h = next;
    }
    s->SortData();
}

// Look for sharing between byte data.  These cannot contain pointers
// so they can all be processed together.
void GetSharing::shareByteData(GCTaskId *, void *a, void *)
{
    GetSharing *s = (GetSharing*)a;
    for (unsigned i = 0; i < NUM_BYTE_VECTORS; i++)
    {
        if (s->byteVectors[i].CurrentCount() != 0)
            gpTaskFarm->AddWorkOrRunNow(SortVector::hashAndSortAllTask, &(s->byteVectors[i]), 0);
    }
}

// Process word data at this particular level
void GetSharing::shareWordData(GCTaskId *, void *a, void *)
{
    GetSharing *s = (GetSharing*)a;
    for (unsigned i = 0; i < NUM_WORD_VECTORS; i++)
    {
        if (s->wordVectors[i].CurrentCount() != 0)
            gpTaskFarm->AddWorkOrRunNow(SortVector::wordDataTask, &(s->wordVectors[i]), 0);
    }
}

// Share any entries left.
void GetSharing::shareRemainingWordData(GCTaskId *, void *a, void *)
{
    GetSharing *s = (GetSharing*)a;
    for (unsigned i = 0; i < NUM_WORD_VECTORS; i++)
    {
        if (s->wordVectors[i].CurrentCount() != 0)
            gpTaskFarm->AddWorkOrRunNow(SortVector::hashAndSortAllTask, &(s->wordVectors[i]), 0);
    }
}

void GetSharing::SortData()
{
    // First process the byte objects.  They cannot contain pointers.
    // We create a task to do this so that we never have more threads
    // running than given with --gcthreads.
    gpTaskFarm->AddWorkOrRunNow(shareByteData, this, 0);
    gpTaskFarm->WaitForCompletion();

    // Word data may contain pointers to other objects.  If an object
    // has been processed its header will contain either a normal length
    // word or a forwarding pointer if it shares.  We can process an
    // object if every word in it is either a tagged integer or an
    // address we have already processed.  This works provided there
    // are no loops so when we reach a stage where we are unable to
    // process anything we simply run a final scan on the remainder.
    // Loops can arise from the closures of mutually recursive functions.

    // Now process the word entries until we have nothing left apart from loops.
    POLYUNSIGNED lastCount = 0, lastShared = 0;
    for (unsigned n = 0; n < NUM_WORD_VECTORS; n++)
        lastCount += wordVectors[n].CurrentCount();

    for(unsigned pass = 1; lastCount != 0; pass++)
    {
        gpTaskFarm->AddWorkOrRunNow(shareWordData, this, 0);
        gpTaskFarm->WaitForCompletion();

        // At each stage check that we have removed some items
        // from the lists.
        POLYUNSIGNED postCount = 0, postShared = 0, carryOver = 0;
        for (unsigned i = 0; i < NUM_WORD_VECTORS; i++)
        {
            postCount += wordVectors[i].CurrentCount();
            postShared += wordVectors[i].Shared();
            carryOver += wordVectors[i].CarryOver();
        }

        if (debugOptions & DEBUG_GC)
            Log("GC: Share: Pass %u: %" POLYUFMT " removed (%1.1f%%) %" POLYUFMT " shared (%1.1f%%) %" POLYUFMT " remain. %" POLYUFMT " entries updated (%1.1f%%).\n",
                pass, lastCount-postCount, (double)(lastCount-postCount) / (double) lastCount * 100.0,
                postShared - lastShared, (double)(postShared - lastShared) / (double) (lastCount-postCount) * 100.0,
                postCount, carryOver, (double)carryOver / (double)(lastCount-postCount) * 100.0);

        // Condition for exiting the loop.  There are some heuristics here.
        // If we remove less than 10% in a pass it's probably not worth continuing
        // unless the carry over is large.  The "carry over" is the number of words updated as
        // a result of the last pass.  It represents the extra sharing we gained in this pass
        // as a result of the last pass.  If there are deep data structures that can be shared
        // we get better sharing with more passes.  If the data structures are shallow we will
        // get as much sharing by just running the final pass.  The first pass only carries
        // over any sharing from the byte objects so we need to run at least one more before
        // checking the carry over.
        if (pass > 1 && (lastCount - postCount) * 10 < lastCount && (carryOver*2 < (lastCount-postCount) || (lastCount - postCount) * 1000 < lastCount ))
            break;

        lastCount = postCount;
        lastShared = postShared;
    }

    // Process any remaining entries.  There may be loops.
    gpTaskFarm->AddWorkOrRunNow(shareRemainingWordData, this, 0);
    gpTaskFarm->WaitForCompletion();

    if (debugOptions & DEBUG_GC)
    {
        POLYUNSIGNED postShared = 0;
        for (unsigned i = 0; i < NUM_WORD_VECTORS; i++)
            postShared += wordVectors[i].Shared();
        if (debugOptions & DEBUG_GC)
            Log("GC: Share: Final pass %" POLYUFMT " removed %" POLYUFMT " shared (%1.1f%%).\n",
                lastCount, postShared - lastShared,
                (double)(postShared - lastShared) / (double) lastCount * 100.0);
    }

    // Calculate the totals.
    POLYUNSIGNED totalSize = 0, totalShared = 0, totalRecovered = 0;
    for (unsigned k = 0; k < NUM_BYTE_VECTORS; k++)
    {
        totalSize += byteVectors[k].TotalCount();
        POLYUNSIGNED shared = byteVectors[k].Shared();
        totalShared += shared;
        totalRecovered += shared * (k+1); // Add 1 for the length word.
        if (debugOptions & DEBUG_GC)
            Log("GC: Share: Byte objects of size %u: %" POLYUFMT " objects %" POLYUFMT " shared\n",
                k, byteVectors[k].TotalCount(), byteVectors[k].Shared());
    }

    for (unsigned l = 0; l < NUM_WORD_VECTORS; l++)
    {
        totalSize += wordVectors[l].TotalCount();
        POLYUNSIGNED shared = wordVectors[l].Shared();
        totalShared += shared;
        totalRecovered += shared * (l+1);
        if (debugOptions & DEBUG_GC)
            Log("GC: Share: Word objects of size %u: %" POLYUFMT " objects %" POLYUFMT " shared\n",
                l, wordVectors[l].TotalCount(), wordVectors[l].Shared());
    }

    if (debugOptions & DEBUG_GC)
    {
        Log("GC: Share: Total %" POLYUFMT " objects, %" POLYUFMT " shared (%1.0f%%).  %" POLYUFMT " words recovered.\n",
            totalSize, totalShared, (double)totalShared / (double)totalSize * 100.0, totalRecovered);
        Log("GC: Share: Excluding %" POLYUFMT " large word objects %" POLYUFMT " large byte objects and %" POLYUFMT " others\n",
            largeWordCount, largeByteCount, excludedCount);
    }

    gHeapSizeParameters.RecordSharingData(totalRecovered);
}

void GCSharingPhase(void)
{
    mainThreadPhase = MTP_GCPHASESHARING;

    GetSharing sharer;

    for (std::vector<LocalMemSpace*>::iterator i = gMem.lSpaces.begin(); i < gMem.lSpaces.end(); i++)
    {
        LocalMemSpace *lSpace = *i;
        lSpace->bitmap.ClearBits(0, lSpace->spaceSize());
    }

    // Scan the code areas to share any constants.  We don't share the code
    // cells themselves.
    for (std::vector<CodeSpace *>::iterator i = gMem.cSpaces.begin(); i < gMem.cSpaces.end(); i++)
    {
        CodeSpace *space = *i;
        sharer.ScanAddressesInRegion(space->bottom, space->top);
    }

    if (debugOptions & DEBUG_GC)
        Log("GC: Share: After scanning code: Total %" POLYUFMT " (%" POLYUFMT " words) byte %" POLYUFMT " word %" POLYUFMT ".\n",
            sharer.totalVisited, sharer.totalSize, sharer.byteAdded, sharer.wordAdded);

    // Process the permanent mutable areas and the code areas
    for (std::vector<PermanentMemSpace*>::iterator i = gMem.pSpaces.begin(); i < gMem.pSpaces.end(); i++)
    {
        PermanentMemSpace *space = *i;
        if (space->isMutable && ! space->byteOnly)
            sharer.ScanAddressesInRegion(space->bottom, space->top);
    }

    if (debugOptions & DEBUG_GC)
        Log("GC: Share: After scanning permanent: Total %" POLYUFMT " (%" POLYUFMT " words) byte %" POLYUFMT " word %" POLYUFMT ".\n",
            sharer.totalVisited, sharer.totalSize, sharer.byteAdded, sharer.wordAdded);

    // Process the RTS roots.
    GCModules(&sharer);

    if (debugOptions & DEBUG_GC)
        Log("GC: Share: After scanning other roots: Total %" POLYUFMT " (%" POLYUFMT " words) byte %" POLYUFMT " word %" POLYUFMT ".\n",
            sharer.totalVisited, sharer.totalSize, sharer.byteAdded, sharer.wordAdded);

    gHeapSizeParameters.RecordGCTime(HeapSizeParameters::GCTimeIntermediate, "Table");

    // Sort and merge the data.
    sharer.SortData();

    gHeapSizeParameters.RecordGCTime(HeapSizeParameters::GCTimeIntermediate, "Sort");
}
