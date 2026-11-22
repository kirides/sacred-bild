#pragma once
#include <windows.h>

// The exceptions a read of a stray pointer raises: an access violation, or a guard page (a page of a thread's stack
// or a user guard page: the first touch raises STATUS_GUARD_PAGE_VIOLATION once), or a page that cannot be paged in.
// Used by the __except filters of the speculative reads (walks over the game's UI and object trees, which can meet a
// pointer into such memory, as ReBorn's layouts make them do).
namespace Gear
{
    inline bool isMemoryFault(unsigned long code)
    {
        return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_GUARD_PAGE || code == EXCEPTION_IN_PAGE_ERROR;
    }
}
