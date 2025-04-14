
#if !defined(lpprint_h)
#define lpprint_h


#include "lptree.h"
#include "lpvm.h"


#if defined(LPEG_DEBUG)

void printcaplist (Capture *cap, Capture *fin);
void printinst (const Instruction *op, const Instruction *p);

void opendebug (lua_State *L);

#else

#define opendebug(L)  { /* no op */ }

#endif

#endif

