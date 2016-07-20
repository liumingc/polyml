/*
    Title:      Run-time system.
    Author:     Dave Matthews, Cambridge University Computer Laboratory

    Copyright (c) 2000
        Cambridge University Technical Services Limited

    Further work copyright David C. J. Matthews 2009, 2012, 2015-16

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

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x) assert(x)
#else
#define ASSERT(x) 0
#endif

#include "globals.h"
#include "gc.h"
#include "mpoly.h"
#include "arb.h"
#include "diagnostics.h"
#include "processes.h"
#include "profiling.h"
#include "run_time.h"
#include "sys.h"
#include "polystring.h"
#include "save_vec.h"
#include "rtsentry.h"
#include "memmgr.h"

extern "C" {
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyFullGC(PolyObject *threadId);
    POLYEXTERNALSYMBOL POLYUNSIGNED PolyIsBigEndian();
}

#define SAVE(x) taskData->saveVec.push(x)
#define SIZEOF(x) (sizeof(x)/sizeof(PolyWord))

// used heavily by MD_init_interface_vector in machine_dep.c
void add_word_to_io_area (unsigned sysop, PolyWord val)
{
    ASSERT (sysop > 0 && sysop < 256);
    PolyWord *objAddr = IoEntry(sysop);
    objAddr[0] = val;
}

/******************************************************************************/
/*                                                                            */
/*      STORAGE ALLOCATION                                                    */
/*                                                                            */
/******************************************************************************/

// This is the storage allocator for allocating heap objects in the RTS.
PolyObject *alloc(TaskData *taskData, POLYUNSIGNED data_words, unsigned flags)
/* Allocate a number of words. */
{
    POLYUNSIGNED words = data_words + 1;
    
    if (profileMode == kProfileStoreAllocation)
        taskData->addAllocationProfileCount(words);

    PolyWord *foundSpace = processes->FindAllocationSpace(taskData, words, false);
    if (foundSpace == 0)
    {
        // Failed - the thread is set to raise an exception.
        throw IOException();
    }

    PolyObject *pObj = (PolyObject*)(foundSpace + 1);
    pObj->SetLengthWord(data_words, flags);
    
    // Must initialise object here, because GC doesn't clean store.
    // Is this necessary any more?  This used to be necessary when we used
    // structural equality and wanted to make sure that unused bytes were cleared.
    // N.B.  This sets the store to zero NOT TAGGED(0).
    for (POLYUNSIGNED i = 0; i < data_words; i++) pObj->Set(i, PolyWord::FromUnsigned(0));
    return pObj;
}

/******************************************************************************/
/*                                                                            */
/*      alloc_and_save - called by run-time system                            */
/*                                                                            */
/******************************************************************************/
Handle alloc_and_save(TaskData *taskData, POLYUNSIGNED size, unsigned flags)
/* Allocate and save the result on the vector. */
{
    return SAVE(alloc(taskData, size, flags));
}

POLYUNSIGNED PolyFullGC(PolyObject *threadId)
{
    TaskData *taskData = TaskData::FindTaskForId(threadId);
    ASSERT(taskData != 0);
    taskData->PreRTSCall();

    try {
        // Can this raise an exception e.g. if there is insufficient memory?
        FullGC(taskData);
    } catch (...) { } // If an ML exception is raised

    taskData->PostRTSCall();
    return TAGGED(0).AsUnsigned(); // Returns unit.
}


/******************************************************************************/
/*                                                                            */
/*      Error Messages                                                        */
/*                                                                            */
/******************************************************************************/


// Return the handle to a string error message.  This will return
// something like "Unknown error" from strerror if it doesn't match
// anything.
Handle errorMsg(TaskData *taskData, int err)
{
#ifdef _WIN32
    /* In the Windows version we may have both errno values
       and also GetLastError values.  We convert the latter into
       negative values before returning them. */
    if (err < 0)
    {
        LPTSTR lpMsg = NULL;
        TCHAR *p;
        if (FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_ALLOCATE_BUFFER |
                FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, (DWORD)(-err), 0, (LPTSTR)&lpMsg, 1, NULL) > 0)
        {
            /* The message is returned with CRLF at the end.  Remove them. */
            for (p = lpMsg; *p != '\0' && *p != '\n' && *p != '\r'; p++);
            *p = '\0';
            Handle res = SAVE(C_string_to_Poly(taskData, lpMsg));
            LocalFree(lpMsg);
            return res;
        }
    }
#endif
    // Unix and unknown Windows errors.
    return SAVE(C_string_to_Poly(taskData, strerror(err)));
}


/******************************************************************************/
/*                                                                            */
/*      EXCEPTIONS                                                            */
/*                                                                            */
/******************************************************************************/

Handle make_exn(TaskData *taskData, int id, Handle arg)
{
    const char *exName;
    switch (id) {
    case EXC_interrupt: exName = "Interrupt"; break;
    case EXC_syserr: exName = "SysErr"; break;
    case EXC_size: exName = "Size"; break;
    case EXC_overflow: exName = "Overflow"; break;
    case EXC_underflow: exName = "Underflow"; break;
    case EXC_divide: exName = "Div"; break;
    case EXC_conversion: exName = "Conversion"; break;
    case EXC_XWindows: exName = "XWindows"; break;
    case EXC_subscript: exName = "Subscript"; break;
    case EXC_foreign: exName = "Foreign"; break;
    case EXC_Fail: exName = "Fail"; break;
    case EXC_thread: exName = "Thread"; break;
    case EXC_extrace: exName = "ExTrace"; break;
    default: ASSERT(0); exName = "Unknown"; // Shouldn't happen.
    }
   

    Handle pushed_name = SAVE(C_string_to_Poly(taskData, exName));
    
    Handle exnHandle = alloc_and_save(taskData, SIZEOF(poly_exn));
    
    DEREFEXNHANDLE(exnHandle)->ex_id   = TAGGED(id);
    DEREFEXNHANDLE(exnHandle)->ex_name = DEREFWORD(pushed_name);
    DEREFEXNHANDLE(exnHandle)->arg     = DEREFWORDHANDLE(arg);
    DEREFEXNHANDLE(exnHandle)->ex_location = TAGGED(0);

    return exnHandle;
}

/******************************************************************************/
/*                                                                            */
/*      raise_exception - called by run-time system                           */
/*                                                                            */
/******************************************************************************/
void raise_exception(TaskData *taskData, int id, Handle arg)
/* Raise an exception with no arguments. */
{
    Handle exn = make_exn(taskData, id, arg);
    /* N.B.  We must create the packet first BEFORE dereferencing the
       process handle just in case a GC while creating the packet
       moves the process and/or the stack. */
    taskData->SetException(DEREFEXNHANDLE(exn));
    throw IOException(); /* Return to Poly code immediately. */
    /*NOTREACHED*/
}


/******************************************************************************/
/*                                                                            */
/*      raise_exception0 - called by run-time system                          */
/*                                                                            */
/******************************************************************************/
void raise_exception0(TaskData *taskData, int id)
/* Raise an exception with no arguments. */
{
    raise_exception(taskData, id, SAVE(TAGGED(0)));
    /*NOTREACHED*/
}

/******************************************************************************/
/*                                                                            */
/*      raise_exception_string - called by run-time system                    */
/*                                                                            */
/******************************************************************************/
void raise_exception_string(TaskData *taskData, int id, const char *str)
/* Raise an exception with a C string as the argument. */
{
    raise_exception(taskData, id, SAVE(C_string_to_Poly(taskData, str)));
    /*NOTREACHED*/
}

// Raise a SysErr exception with a given error code.
// The string part must match the result of OS.errorMsg
void raiseSyscallError(TaskData *taskData, int err)
{
    Handle errornum = Make_fixed_precision(taskData, err);
    Handle pushed_option = alloc_and_save(taskData, 1);
    DEREFHANDLE(pushed_option)->Set(0, DEREFWORDHANDLE(errornum)); /* SOME err */
    Handle pushed_name = errorMsg(taskData, err); // Generate the string.
    Handle pair = alloc_and_save(taskData, 2);
    DEREFHANDLE(pair)->Set(0, DEREFWORDHANDLE(pushed_name));
    DEREFHANDLE(pair)->Set(1, DEREFWORDHANDLE(pushed_option));

    raise_exception(taskData, EXC_syserr, pair);
}

// Raise a SysErr exception which does not correspond to an error code.
void raiseSyscallMessage(TaskData *taskData, const char *errmsg)
{
    Handle pushed_option = SAVE(NONE_VALUE); /* NONE */
    Handle pushed_name = SAVE(C_string_to_Poly(taskData, errmsg));
    Handle pair = alloc_and_save(taskData, 2);
    DEREFHANDLE(pair)->Set(0, DEREFWORDHANDLE(pushed_name));
    DEREFHANDLE(pair)->Set(1, DEREFWORDHANDLE(pushed_option));

    raise_exception(taskData, EXC_syserr, pair);
}

// This was the previous version.  The errmsg argument is ignored unless err is zero.
// Calls to it should really be replaced with calls to either raiseSyscallMessage
// or raiseSyscallError but it's been left because there may be cases where errno
// actually contains zero.
void raise_syscall(TaskData *taskData, const char *errmsg, int err)
{
    if (err == 0) raiseSyscallMessage(taskData, errmsg);
    else raiseSyscallError(taskData, err);
}

// Raises a Fail exception.
void raise_fail(TaskData *taskData, const char *errmsg)
{
    raise_exception_string(taskData, EXC_Fail, errmsg);
}

/* "Polymorphic" function to generate a list. */
Handle makeList(TaskData *taskData, int count, char *p, int size, void *arg,
                       Handle (mkEntry)(TaskData *, void*, char*))
{
    Handle saved = taskData->saveVec.mark();
    Handle list = SAVE(ListNull);
    /* Start from the end of the list. */
    p += count*size;
    while (count > 0)
    {
        Handle value, next;
        p -= size; /* Back up to the last entry. */
        value = mkEntry(taskData, arg, p);
        next  = alloc_and_save(taskData, SIZEOF(ML_Cons_Cell));

        DEREFLISTHANDLE(next)->h = DEREFWORDHANDLE(value); 
        DEREFLISTHANDLE(next)->t = DEREFLISTHANDLE(list);

        taskData->saveVec.reset(saved);
        list = SAVE(DEREFHANDLE(next));
        count--;
    }
    return list;
}

void CheckAndGrowStack(TaskData *taskData, POLYUNSIGNED minSize)
/* Expands the current stack if it has grown. We cannot shrink a stack segment
   when it grows smaller because the frame is checked only at the beginning of
   a function to ensure that there is enough space for the maximum that can
   be allocated. */
{
    /* Get current size of new stack segment. */
    POLYUNSIGNED old_len = taskData->stack->spaceSize();

    if (old_len >= minSize) return; /* Ok with present size. */

    // If it is too small double its size.
    POLYUNSIGNED new_len; /* New size */
    for (new_len = old_len; new_len < minSize; new_len *= 2);
    POLYUNSIGNED limitSize = getPolyUnsigned(taskData, taskData->threadObject->mlStackSize);

    // Do not grow the stack if its size is already too big.
    if ((limitSize != 0 && old_len >= limitSize) || ! gMem.GrowOrShrinkStack(taskData, new_len))
    {
        /* Cannot expand the stack any further. */
        extern FILE *polyStderr;
        fprintf(polyStderr, "Warning - Unable to increase stack - interrupting thread\n");
        if (debugOptions & DEBUG_THREADS)
            Log("THREAD: Unable to grow stack for thread %p from %lu to %lu\n", taskData, old_len, new_len);
        // We really should do this only if the thread is handling interrupts
        // asynchronously.  On the other hand what else do we do?
        Handle exn = make_exn(taskData, EXC_interrupt, SAVE(TAGGED(0)));
        taskData->SetException(DEREFEXNHANDLE(exn));
    }
    else
    {
        if (debugOptions & DEBUG_THREADS)
            Log("THREAD: Growing stack for thread %p from %lu to %lu\n", taskData, old_len, new_len);
    }
}

Handle Make_fixed_precision(TaskData *taskData, int val)
{
    if (val > MAXTAGGED || val < -MAXTAGGED-1)
        raise_exception0(taskData, EXC_overflow);
    return taskData->saveVec.push(TAGGED(val));
}

Handle Make_fixed_precision(TaskData *taskData, unsigned uval)
{
    if (uval > MAXTAGGED)
        raise_exception0(taskData, EXC_overflow);
    return taskData->saveVec.push(TAGGED(uval));
}

Handle Make_fixed_precision(TaskData *taskData, long val)
{
    if (val > MAXTAGGED || val < -MAXTAGGED-1)
        raise_exception0(taskData, EXC_overflow);
    return taskData->saveVec.push(TAGGED(val));
}

Handle Make_fixed_precision(TaskData *taskData, unsigned long uval)
{
    if (uval > MAXTAGGED)
        raise_exception0(taskData, EXC_overflow);
    return taskData->saveVec.push(TAGGED(uval));
}

#if (SIZEOF_LONG_LONG != 0) && (SIZEOF_LONG_LONG <= SIZEOF_VOIDP)
Handle Make_fixed_precision(TaskData *taskData, long long val)
{
    if (val > MAXTAGGED || val < -MAXTAGGED-1)
        raise_exception0(taskData, EXC_overflow);
    return taskData->saveVec.push(TAGGED(val));
}

Handle Make_fixed_precision(TaskData *taskData, unsigned long long uval)
{
    if (uval > MAXTAGGED)
        raise_exception0(taskData, EXC_overflow);
    return taskData->saveVec.push(TAGGED(uval));
}
#endif

Handle Make_sysword(TaskData *taskData, uintptr_t p)
{
    Handle result = alloc_and_save(taskData, 1, F_BYTE_OBJ);
    *(uintptr_t*)(result->Word().AsCodePtr()) = p;
    return result;
}

// This is used to determine the endian-ness that Poly/ML is running under.
// It's really only needed for the interpreter.  In particular the pre-built
// compiler may be running under either byte order and has to check at
// run-time.
POLYUNSIGNED PolyIsBigEndian()
{
#ifdef WORDS_BIGENDIAN
    return TAGGED(1).AsUnsigned();
#else
    return TAGGED(0).AsUnsigned();
#endif
}

struct _entrypts runTimeEPT[] =
{
    { "PolyFullGC",                     (polyRTSFunction)&PolyFullGC},
    { "PolyIsBigEndian",                (polyRTSFunction)&PolyIsBigEndian},

    { NULL, NULL} // End of list.
};
