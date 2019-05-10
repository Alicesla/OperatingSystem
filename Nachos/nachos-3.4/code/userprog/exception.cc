// exception.cc 
//	Entry point into the Nachos kernel from user programs.
//	There are two kinds of things that can cause control to
//	transfer back to here from user code:
//
//	syscall -- The user code explicitly requests to call a procedure
//	in the Nachos kernel.  Right now, the only function we support is
//	"Halt".
//
//	exceptions -- The user code does something that the CPU can't handle.
//	For instance, accessing memory that doesn't exist, arithmetic errors,
//	etc.  
//
//	Interrupts (which can also cause control to transfer from user
//	code into the Nachos kernel) are handled elsewhere.
//
// For now, this only handles the Halt() system call.
// Everything else core dumps.
//
// Copyright (c) 1992-1993 The Regents of the University of California.
// All rights reserved.  See copyright.h for copyright notice and limitation 
// of liability and disclaimer of warranty provisions.

#include "copyright.h"
#include "system.h"
#include "syscall.h"

//----------------------------------------------------------------------
// ExceptionHandler
// 	Entry point into the Nachos kernel.  Called when a user program
//	is executing, and either does a syscall, or generates an addressing
//	or arithmetic exception.
//
// 	For system calls, the following is the calling convention:
//
// 	system call code -- r2
//		arg1 -- r4
//		arg2 -- r5
//		arg3 -- r6
//		arg4 -- r7
//
//	The result of the system call, if any, must be put back into r2. 
//
// And don't forget to increment the pc before returning. (Or else you'll
// loop making the same system call forever!
//
//	"which" is the kind of exception.  The list of possible exceptions 
//	are in machine.h.
//----------------------------------------------------------------------

void
ExceptionHandler(ExceptionType which)
{
    // Lab4: Page Fault Handling
    if (which == PageFaultException) {
        if (machine->tlb == NULL) { // linear page table page fault
	        DEBUG('m', COLORED(WARNING, "=> Linear page table page fault.\n"));
            // In current Nachos this shouldn't happen
            // because physical page frame == virtual page number
            // (can be found in AddrSpace::AddrSpace in userprog/addrspace.cc)
            // On the other hand, in our Lab we won't use linear page table at all
            ASSERT(FALSE);
        } else { // TLB miss (no TLB entry)
            // Lab4 Exercise 2 and 3 and 7
	        DEBUG('m', COLORED(WARNING, "=> TLB miss (no TLB entry)\n"));
            int BadVAddr = machine->ReadRegister(BadVAddrReg); // The failing virtual address on an exception
            TLBMissHandler(BadVAddr);
        }
        return;
    }

    // System Call
    // The system call codes (SC_[TYPE]) is defined in userprog/syscall.h
    // The system call stubs is defined in test/start.s
    int type = machine->ReadRegister(2); // r2: the standard C calling convention on the MIPS

    if (which == SyscallException) {
        if (type == SC_Halt) {
            DEBUG('a', "Shutdown, initiated by user program.\n");
            PrintTLBStatus(); // TLB debug usage
            interrupt->Halt();
        } else if (type == SC_Exit || type == SC_Exec || type == SC_Join) {
            // Address Space Control (Process Management) System Calls
            AddressSpaceControlHandler(type);
        } else if (type == SC_Create || type == SC_Open || type == SC_Write || type == SC_Read || type == SC_Close) {
            // File System System Calls
            FileSystemHandler(type);
        } else if (type == SC_Fork || type == SC_Yield) {
            // User-level Threads System Calls
            UserLevelThreadsHandler(type);
        }

        // Increment the Program Counter before returning.
        IncrementPCRegs();
        return;
    }

    printf("Unexpected user mode exception %d %d\n", which, type);
	ASSERT(FALSE);
}

/**********************************************************************/
/*************************** Lab4: TLB Miss ***************************/
/**********************************************************************/

// Lab4 Exercise3
// #define TLB_FIFO TRUE
#define TLB_CLOCK TRUE
// #define TLB_LRU TRUE

// Lab4 Exercise2
#if !(TLB_FIFO || TLB_LRU)
int TLBreplaceIdx = 0; // When using TLB_CLOCK, this is circular pointer
#endif

// Print TLB Status
void PrintTLBStatus(void)
{
#ifdef USE_TLB
    // Lab4: Used for calculate TLB Miss rate (debug purpose)
    DEBUG('T', "TLB Miss: %d, TLB Hit: %d, Total Instruction: %d, Total Translate: %d, TLB Miss Rate: %.2lf%%\n",
        TLBMissCount, TranslateCount-2*TLBMissCount, TranslateCount-TLBMissCount, TranslateCount, (double)(TLBMissCount*100)/(TranslateCount-TLBMissCount));
#endif
}

//----------------------------------------------------------------------
// TLBMissHandler
// 	1. Calculate vpn (virtual page number)
//  2. See if the page is valid, if not this is a page fault
//  3. Update TLB (algorithms to find a position to replace)
//----------------------------------------------------------------------

void
TLBMissHandler(int virtAddr)
{
    unsigned int vpn;
    vpn = (unsigned) virtAddr / PageSize;

    // Find the Page
    TranslationEntry page = machine->pageTable[vpn];
#ifndef DEMAND_PAGING
    ASSERT_MSG(page.valid, "Invalid virtual page number");
#else // Lab4: Demand paging
    if (!page.valid) {
        DEBUG('m', COLORED(WARNING, "\t=> Page miss\n"));
        page = PageFaultHandler(vpn);
    }
#endif

    // Update TLB
#if TLB_FIFO
    TLBAlgoFIFO(page);
#elif TLB_CLOCK
    TLBAlgoClock(page);
#elif TLB_LRU
    TLBAlgoLRU(page);
#else
    // ONLY USE FOR TESTING Lab4 Exercise2
    // i.e. assume TLBSize = 2
    machine->tlb[TLBreplaceIdx] = page;
    TLBreplaceIdx = TLBreplaceIdx ? 0 : 1;
#endif

}

//----------------------------------------------------------------------
// TLBAlgoFIFO
//  The most naive replacement algorithm
//----------------------------------------------------------------------

#ifdef TLB_FIFO
void
TLBAlgoFIFO(TranslationEntry page)
{
    int TLBreplaceIdx = -1;
    // Find the empty entry
    for (int i = 0; i < TLBSize; i++) {
        if (machine->tlb[i].valid == FALSE) {
            TLBreplaceIdx = i;
            break;
        }
    }
    // If full then move everything forward and remove the last one
    if (TLBreplaceIdx == -1) {
        TLBreplaceIdx = TLBSize - 1;
        for (int i = 0; i < TLBSize - 1; i++) {
            machine->tlb[i] = machine->tlb[i+1];
        }
    }
    // Update TLB
    machine->tlb[TLBreplaceIdx] = page;
}
#endif

//----------------------------------------------------------------------
// TLBAlgoClock
//  Basically the second-chance replacement algorithm with circular list/pointer	
//----------------------------------------------------------------------

#ifdef TLB_CLOCK
void
TLBAlgoClock(TranslationEntry page)
{
    // Find the next one
    // if used then clear to 0 and continue find the next one.
    // until find the one that is not used.
    while (1) {
        TLBreplaceIdx %= TLBSize;
        if (machine->tlb[TLBreplaceIdx].valid == FALSE) {
            break;
        } else  {
            if (machine->tlb[TLBreplaceIdx].use) {
                // Found the entry is recently used
                // clear the R bit and find next
                machine->tlb[TLBreplaceIdx].use = FALSE;
                TLBreplaceIdx++;
            } else {
                // Evict the entry
                break;
            }
        }
    }

    // Update TLB
    machine->tlb[TLBreplaceIdx] = page;
    machine->tlb[TLBreplaceIdx].use = TRUE;
}
#endif

//----------------------------------------------------------------------
// TLBAlgoLRU
// 	
//----------------------------------------------------------------------

#ifdef TLB_LRU
void
TLBAlgoLRU(TranslationEntry page)
{

}
#endif

/**********************************************************************/
/************************ Lab4: Demand Paging *************************/
/**********************************************************************/

#ifdef DEMAND_PAGING
//----------------------------------------------------------------------
// NaivePageReplacement
//  1. Find an non-dirty page frame to replace.
//  2. If not found, then replace a dirty page and write back to disk.
//  3. Return the page frame number when founded or after replacement.
//----------------------------------------------------------------------

int
NaivePageReplacement(int vpn)
{
    int pageFrame = -1;
    for (int temp_vpn = 0; temp_vpn < machine->pageTableSize, temp_vpn != vpn; temp_vpn++) {
        if (machine->pageTable[temp_vpn].valid) {
            if (!machine->pageTable[temp_vpn].dirty) {
                pageFrame = machine->pageTable[temp_vpn].physicalPage;
                break;
            }
        }
    }
    if (pageFrame == -1) { // No non-dirty entry
        for (int replaced_vpn = 0; replaced_vpn < machine->pageTableSize, replaced_vpn != vpn; replaced_vpn++) {
            if (machine->pageTable[replaced_vpn].valid) {
                machine->pageTable[replaced_vpn].valid = FALSE;
                pageFrame = machine->pageTable[replaced_vpn].physicalPage;

                // Store the page back to disk
                OpenFile *vm = fileSystem->Open("VirtualMemory");
                ASSERT_MSG(vm != NULL, "fail to open virtual memory");
                vm->WriteAt(&(machine->mainMemory[pageFrame*PageSize]), PageSize, replaced_vpn*PageSize);
                delete vm; // close file
                break;
            }
        }
    }
    return pageFrame;
}

//----------------------------------------------------------------------
// PageFaultHandler
// 	1. Find an empty space in memory
//  2. Load the page frame from disk to memory
//      * If memory out of space then find a page to replace
//          * If all pages are dirty, then it need to write back to disk.
//----------------------------------------------------------------------

TranslationEntry
PageFaultHandler(int vpn)
{
    // Get a Memory space (page frame) to allocate
#ifdef USE_BITMAP
    int pageFrame = machine->allocateFrame(); // ppn
#else
    ASSERT(FALSE); // No other free memory allocation mechnism yet.
#endif
    if (pageFrame == -1) { // Need page replacement
        pageFrame = NaivePageReplacement(vpn);
    }
    machine->pageTable[vpn].physicalPage = pageFrame;

    // Load the Page from virtual memory
    DEBUG('a', "Demand paging: loading page from virtual memory!\n");
    OpenFile *vm = fileSystem->Open("VirtualMemory"); // This file is created in userprog/addrspace.cc
    ASSERT_MSG(vm != NULL, "fail to open virtual memory");
    vm->ReadAt(&(machine->mainMemory[pageFrame*PageSize]), PageSize, vpn*PageSize);
    delete vm; // close the file

    // Set the page attributes
    machine->pageTable[vpn].valid = TRUE;
    machine->pageTable[vpn].use = FALSE;
    machine->pageTable[vpn].dirty = FALSE;
    machine->pageTable[vpn].readOnly = FALSE;

    currentThread->space->PrintState(); // debug with -d M to show bitmap
}
#endif

/**********************************************************************/
/*************************** Lab6: Syscall ****************************/
/**********************************************************************/

//----------------------------------------------------------------------
// IncrementPCRegs
// 	Because when Nachos cause the exception. The PC won't increment
//  (i.e. PC+4) in Machine::OneInstruction in machine/mipssim.cc.
//  Thus, when invoking a system call, we need to advance the program
//  counter. Or it will cause the infinity loop.
//----------------------------------------------------------------------

void IncrementPCRegs(void) {
    // Debug usage
    machine->WriteRegister(PrevPCReg, machine->ReadRegister(PCReg));

    // Advance program counter
    machine->WriteRegister(PCReg, machine->ReadRegister(NextPCReg));
    machine->WriteRegister(NextPCReg, machine->ReadRegister(NextPCReg)+4);
}

//----------------------------------------------------------------------
// AddressSpaceControlHandler
// 	Handling address space control related system call.
//  1. Exit
//  2. Exec
//  3. Join
//----------------------------------------------------------------------

void AddressSpaceControlHandler(int type)
{
    if (type == SC_Exit) {

        PrintTLBStatus(); // TLB debug usage

        int status = machine->ReadRegister(4); // r4: first arguments to functions

        currentThread->setExitStatus(status);
        if (status == 0) {
            DEBUG('S', COLORED(GREEN, "User program exit normally. (status 0)\n"));
        } else {
            DEBUG('S', COLORED(FAIL, "User program exit with status %d\n"), status);
        }

        // TODO: release children

#ifdef USER_PROGRAM
        if (currentThread->space != NULL) {
#ifdef USE_BITMAP
            machine->freeMem(); // ONLY USE FOR TEST Lab4 Exercise4
#endif
            delete currentThread->space;
            currentThread->space = NULL;
        }
#endif
        // TODO: if it has parent, then set this to zombie and signal
        currentThread->Finish();
    }
}

//----------------------------------------------------------------------
// FileSystemHandler
// 	
//----------------------------------------------------------------------

void FileSystemHandler(int type)
{

}

//----------------------------------------------------------------------
// UserLevelThreadsHandler
// 	
//----------------------------------------------------------------------

void UserLevelThreadsHandler(int type)
{

}
