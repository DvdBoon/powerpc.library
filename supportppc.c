// Copyright (c) 2019, 2020 Dennis van der Boon
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma pack(push,2)
#include <exec/exec.h>
#include <powerpc/tasksPPC.h>
#include <powerpc/powerpc.h>
#include <exec/memory.h>
#include "constants.h"
#include "libstructs.h"
#include "Internalsppc.h"
#include "prirule.h"
#pragma pack(pop)

/********************************************************************************************
*
*
*
*********************************************************************************************/

/*
 * EnqueueTask, powerpc.p:3060-3105. The body moved to prirule.h so that
 * kernel.c's SwitchPPC can use the same rule: a direct call from the kernel
 * hunk into this one would land where the callee used to be, and the two
 * copies of a scheduler rule would drift. The signed arithmetic is the change
 * of substance -- tp_Priority and tp_Prioffset are ULONG in tasksPPC.h but the
 * original compares them with cmpw, so a task past its share read as about
 * 4.29 billion here and sorted to the front of the ready list.
 */
PPCFUNCTION VOID InsertOnPri(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list, __reg("r5") struct TaskPPC* myTask)
{
    priInsert(PowerPCBase, (struct MinList*)list, myTask);
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION APTR AllocVec68K(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG size, __reg("r5") ULONG flags)
{
	APTR memBlock = NULL;

	if (size)
	{
		flags = (flags & ~MEMF_CHIP) | MEMF_PPC;
		memBlock = (APTR)myRun68KLowLevel(PowerPCBase, (ULONG)PowerPCBase, _LVOAllocVec32, 0, 0, size, flags);
	
        if (memBlock)
	    {
            mySetCache(PowerPCBase, CACHE_DCACHEINV, memBlock, size); //no longer working...have to check
	    }
	}
	return memBlock;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID FreeVec68K(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") APTR memBlock)
{
    myRun68KLowLevel(PowerPCBase, (ULONG)PowerPCBase, _LVOFreeVec32, 0, (ULONG)memBlock, 0, 0);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID ForbidPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    PowerPCBase->pp_FlagForbid = 1;

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID PermitPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    PowerPCBase->pp_FlagForbid = 0;

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG CallKernel(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SysCall* sc)
{
    /*
     * IN AN EXCEPTION HANDLER, CALL THE KERNEL; DO NOT TRAP INTO IT AGAIN. The
     * five system calls -- SetCache and the message frames -- are an "sc" from
     * a task, which runs in user mode. A handler runs in supervisor mode inside
     * the kernel already, and its "sc" re-entered it: the nested entry saved
     * the handler's registers over the interrupted task's, and the outer exit
     * resumed the handler instead of the task (REVIEW-LIB.md K1). WarpOS's
     * SetCache does the work itself in exception mode (powerpc.p:11092-11103),
     * and its ReplyMsgPPC never calls the 68k from a handler (:12648-12650):
     * THIS MATCHES. pp_KSystemCall is 0 only until libinit.c stores it.
     */
    if ((PowerPCBase->pp_ExceptionMode) && (PowerPCBase->pp_KSystemCall))
    {
        return ((ULONG (*)(__reg("r3") struct PrivatePPCBase*, __reg("r4") struct SysCall*))PowerPCBase->pp_KSystemCall)(PowerPCBase, sc);
    }
    return SystemCall(sc);
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct MsgFrame* CreateMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
	struct SysCall sc;
    sc.sc_Function = SC_CREATEMSG;

    ULONG msgFrame = CallKernel(PowerPCBase, &sc);

//#if 0
    if (msgFrame)
    {
        ULONG clearFrame = msgFrame;
        for (int i=0; i<48; i++)
        {
            *((ULONG*)(clearFrame)) = 0;
            clearFrame += 4;
        }
    }
//#endif
	return (struct MsgFrame*)msgFrame;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct MsgFrame* GetMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    struct SysCall sc;
    sc.sc_Function = SC_GETMSG;

    ULONG msgFrame = CallKernel(PowerPCBase, &sc);

	return (struct MsgFrame*)msgFrame;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID SendMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* msgFrame)
{
	struct SysCall sc;
    sc.sc_Function = SC_SENDMSG;
    sc.sc_Arg[0] = (ULONG)msgFrame;

    CallKernel(PowerPCBase, &sc);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID FreeMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* msgFrame)
{
	struct SysCall sc;
    sc.sc_Function = SC_FREEMSG;
    sc.sc_Arg[0] = (ULONG)msgFrame;

    CallKernel(PowerPCBase, &sc);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG StricmpPPC(__reg("r3") STRPTR string1, __reg("r4") STRPTR string2)
{
    LONG result = 0;
	ULONG offset = 0;
	UBYTE s1,s2;

	do
	{
		s1 = string1[offset];
		s2 = string2[offset];

		if ((0x40 < s1) && (s1< 0x5b))
		{
			s1 |= 0x20;
		}
		else if (s1 > 0x5a)
		{
			if ((0xc0 < s1) && (s1 < 0xe0))
			{
				s1 |= 0x20;
			}
		}
		if ((0x40 < s2) && (s2 < 0x5b))
		{
			s2 |= 0x20;
		}
		else if (s2 > 0x5a)
		{
			if ((0xc0 < s2) && (s2 < 0xe0))
			{
				s2 |= 0x20;
			}
		}
		if (s1 != s2)
		{
			if (s1 < s2)
			{
				result = -1;
			}
			else
			{
				result = 1;
			}
			break;
		}
		offset ++;
	} while (s1);

	return result;
}
/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG GetLen(__reg("r3") STRPTR string)
{
	ULONG offset = 0;

	while (string[offset])
	{
	 	offset++;
	}
	return offset;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION STRPTR CopyStr(__reg("r3") APTR source, __reg("r4") APTR dest)
{
	ULONG offset = -1;

    UBYTE* mySource = (UBYTE*)source;
    UBYTE* myDest   = (UBYTE*)dest;

	do
	{
		offset ++;
		myDest[offset] = mySource[offset];
	} while (mySource[offset]);

    offset ++;

    return &myDest[offset];
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID CauseDECInterrupt(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
	ULONG key;

	if (!(PowerPCBase->pp_ExceptionMode))
	{
		key = mySuper(PowerPCBase);
        PowerPCBase->pp_ExceptionMode = -1;
		setDEC(30);
		myUser(PowerPCBase, key);
        while (PowerPCBase->pp_ExceptionMode);
	}
	return;
}

/********************************************************************************************
*
*	THE PORT'S INTERNAL LIST, HANDED TO THE KERNEL. A handler's PutMsgPPC or ReplyMsgPPC
*	appends to mp_IntMsg without the port's semaphore. A task holding that semaphore asks
*	the kernel to move those messages onto mp_MsgList before it uses the port, and waits
*	until it has: WarpOS's PutMsgPPC, GetMsgPPC, ReplyMsgPPC and WaitPortPPC
*	(powerpc.p:12493-12519, :12579-12603, :12672-12697, :12375-12399, :12418-12444), with
*	pp_CurrentPort / pp_FlagPortInUse for PPC_INTPORT / PPC_FORCEMESSAGE and
*	CauseDECInterrupt for SetTimer(0), as 17.13b. The kernel's half is HandleDeferred
*	(kernel.c). FIXES.md "#1 PART B DESIGNED", 1 Nov 2026.
*
*********************************************************************************************/

PPCFUNCTION VOID MergeIntMsgs(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port)
{
    struct List* intList = &port->mp_IntMsg;

    if (intList->lh_TailPred == (struct Node*)intList)
    {
        return;
    }

    while (1)
    {
        ForbidPPC(PowerPCBase);
        if (!(*((volatile UBYTE*)&PowerPCBase->pp_FlagPortInUse)))
        {
            PowerPCBase->pp_CurrentPort   = port;
            PowerPCBase->pp_FlagPortInUse = -1;
            PermitPPC(PowerPCBase);
            break;
        }
        PermitPPC(PowerPCBase);
        while (*((volatile UBYTE*)&PowerPCBase->pp_FlagPortInUse));
    }

    CauseDECInterrupt(PowerPCBase);

    while (*((volatile UBYTE*)&PowerPCBase->pp_FlagPortInUse));

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG CheckExcSignal(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TaskPPC* myTask, __reg("r5") ULONG signal)
{
	ULONG sigmask;

	while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

	sigmask = (myTask->tp_Task.tc_SigRecvd | signal) & myTask->tp_Task.tc_SigExcept;

	if (!(sigmask))
	{
		FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
		return signal;
	}

	myTask->tp_Task.tc_SigRecvd |= sigmask;
	signal = signal & ~sigmask;
	PowerPCBase->pp_TaskExcept = myTask;
	FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
	CauseDECInterrupt(PowerPCBase);

    /*
     * THE WAIT READS THE FIELD EVERY TIME -- T5 (REVIEW-LIB.md). This was
     * "while (test = (volatile ULONG)PowerPCBase->pp_TaskExcept);": the cast
     * made the VALUE volatile, not the access, and the compiled loop spun on a
     * register. When CauseDECInterrupt returned before the kernel's TaskCheck
     * had cleared the field -- a spurious external interrupt, or a trace or
     * perfmon exit, ends its wait -- the caller spun for ever. Now it waits
     * until TaskCheck runs, at the latest on the next scheduler tick. WarpOS
     * and 17.13b load the field on every turn (CheckExcSignals .check,
     * powerpc.p:3141-3144; .IntWait2, ppcfunctions.p:6237-6239): THIS MATCHES.
     */
    while (*((volatile ULONG*)&PowerPCBase->pp_TaskExcept));

	return signal;

}

/********************************************************************************************
*
*        APTR = AllocatePPC(struct Library*, struct MemHeader*, ULONG byteSize)
*
*********************************************************************************************/

PPCFUNCTION APTR AllocatePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MemHeader* memHeader, __reg("r5") ULONG byteSize)
{
    struct DebugArgs args;
    args.db_Function = 66 | (2<<8) | (1<<16) | (3<<17);
    args.db_Arg[0] = (ULONG)memHeader;
    args.db_Arg[1] = byteSize;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct MemChunk* currChunk = NULL;

    if (byteSize)
    {
        byteSize = (byteSize + 31) & -32;

        if (byteSize <= memHeader->mh_Free)
        {
            struct MemChunk* newChunk;
            struct MemChunk* prevChunk = (struct MemChunk*)&memHeader->mh_First;

            while (currChunk = prevChunk->mc_Next)
            {
                if (currChunk->mc_Bytes == byteSize)
                {
                    prevChunk->mc_Next = currChunk->mc_Next;
                    break;
                }
                else if (currChunk->mc_Bytes > byteSize)
                {
                    newChunk = (struct MemChunk*)((ULONG)currChunk + byteSize);
                    newChunk->mc_Next = currChunk->mc_Next;
                    newChunk->mc_Bytes = currChunk->mc_Bytes - byteSize;
                    prevChunk->mc_Next = newChunk;
                    break;
                }
                prevChunk = currChunk;
            }

            if (currChunk)
            {
                memHeader->mh_Free -= byteSize;

                UBYTE* buffer = (APTR)currChunk;
                for (int i=0; i < byteSize; i++)
                {
                    buffer[i] = 0;
                }
            }
        }
    }

    args.db_Arg[0] = (ULONG)currChunk;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return (APTR)currChunk;
}

/********************************************************************************************
*
*        VOID DeallocatePPC(struct Library*, struct MemHeader*, APTR, ULONG)
*
*********************************************************************************************/

PPCFUNCTION VOID DeallocatePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MemHeader* memHeader,
                   __reg("r5") APTR memoryBlock, __reg("r6") ULONG byteSize)
{
	struct DebugArgs args;
    args.db_Function = 67 | (3<<8) | (1<<16) | (3<<17);
    args.db_Arg[0] = (ULONG)memHeader;
    args.db_Arg[1] = (ULONG)memoryBlock;
    args.db_Arg[2] = byteSize;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    if (byteSize)
    {
        ULONG testSize = (((ULONG)memoryBlock) - ((ULONG)(memoryBlock) & -32));
        struct MemChunk* testChunk = (struct MemChunk*)((ULONG)(memoryBlock) & -32);
        ULONG freeSize = testSize + byteSize + 31;

        if (freeSize &= -32)
        {
            /*
             * F7: THE FREED BLOCK JOINS THE FREE CHUNK BELOW IT WHEREVER IT
             * LIES. The walk stops at the last free chunk below the block; a
             * chunk at the block itself is a double free, one reaching into
             * it an overlap, and one ending at it is joined. As 17.13b
             * (ppcfunctions.p:7123-7153). Before, that test ran only when a
             * free chunk lay above the block: one freed above the last free
             * chunk was linked with no join and no overlap test. testChunk
             * is the 32-aligned start (above), which a second declaration
             * here used to hide.
             */
            struct MemChunk* currChunk = (struct MemChunk*)&memHeader->mh_First;

            while ((currChunk->mc_Next) && (currChunk->mc_Next < testChunk))
            {
                currChunk = currChunk->mc_Next;
            }
            if (currChunk->mc_Next == testChunk)
            {
                HaltError(ERR_EMEM);
            }

            if ((currChunk != (struct MemChunk*)&memHeader->mh_First) && ((ULONG)testChunk <= currChunk->mc_Bytes + (ULONG)currChunk))
            {
                if ((ULONG)testChunk < currChunk->mc_Bytes + (ULONG)currChunk)
                {
                    HaltError(ERR_EMEM);
                }
                currChunk->mc_Bytes += freeSize;
                testChunk = currChunk;
            }
            else
            {
                testChunk->mc_Next = currChunk->mc_Next;
                currChunk->mc_Next = testChunk;
                testChunk->mc_Bytes = freeSize;
            }

	        struct MemChunk* nextChunk = testChunk->mc_Next;
	
            if (nextChunk)
	        {
		        if ((ULONG)nextChunk < (ULONG)(testChunk) + testChunk->mc_Bytes)
		        {
                    HaltError(ERR_EMEM);
		        }
		        else if ((ULONG)nextChunk == (ULONG)(testChunk) + testChunk->mc_Bytes)
		        {
			        testChunk->mc_Next = nextChunk->mc_Next;
			        testChunk->mc_Bytes = nextChunk->mc_Bytes + testChunk->mc_Bytes;
		        }
	        }
	        memHeader->mh_Free += freeSize;
        }
    }
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID printDebug(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct DebugArgs* args)
{
#if 0
    ULONG* mem = (APTR)(PowerPCBase->pp_PPCMemBase + OFFSET_SYSMEM + 0x100);
    mem [args->db_Function & 0xff] += 1;
    mem [-1] = (args->db_Function & 0xff);
#endif
    if ((PowerPCBase->pp_DebugLevel) && (!(PowerPCBase->pp_ExceptionMode)))
    {
        ULONG flag = args->db_Function & (1<<16);
        ULONG level = args->db_Function >> 17;

        if (level > PowerPCBase->pp_DebugLevel - 1)
        {
            return;
        }

        args->db_Function &= ~(7<<16);

        struct MsgFrame* myFrame = CreateMsgFramePPC(PowerPCBase);
        args->db_ProcessName = PowerPCBase->pp_ThisPPCProc->tp_Task.tc_Node.ln_Name;
        UBYTE oldlevel = PowerPCBase->pp_DebugLevel;

        while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
        PowerPCBase->pp_DebugLevel = 0;
        myCopyMemPPC(PowerPCBase, (APTR)args, (APTR)&myFrame->mf_PPCArgs, sizeof(struct DebugArgs));
        PowerPCBase->pp_DebugLevel = oldlevel;
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

        if (flag)
        {
            myFrame->mf_Identifier = ID_DBGS;
        }
        else
        {
            myFrame->mf_Identifier = ID_DBGE;
        }

        SendMsgFramePPC(PowerPCBase, myFrame);

        args->db_Function |= (level << 17);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID GetBATs(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TaskPPC* task)
{
    ULONG key = mySuper(PowerPCBase);

    MoveFromBAT(CHMMU_BAT0, (struct BATArray*)(((ULONG)task->tp_BATStorage) + 0));
    MoveFromBAT(CHMMU_BAT1, (struct BATArray*)(((ULONG)task->tp_BATStorage) + 16));
    MoveFromBAT(CHMMU_BAT2, (struct BATArray*)(((ULONG)task->tp_BATStorage) + 32));
    MoveFromBAT(CHMMU_BAT3, (struct BATArray*)(((ULONG)task->tp_BATStorage) + 48));

    MoveToBAT(CHMMU_BAT0, (struct BATArray*)&PowerPCBase->pp_StoredBATs[0]);
    MoveToBAT(CHMMU_BAT1, (struct BATArray*)&PowerPCBase->pp_StoredBATs[1]);
    MoveToBAT(CHMMU_BAT2, (struct BATArray*)&PowerPCBase->pp_StoredBATs[2]);
    MoveToBAT(CHMMU_BAT3, (struct BATArray*)&PowerPCBase->pp_StoredBATs[3]);

    myUser(PowerPCBase, key);

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID StoreBATs(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TaskPPC* task)
{
    ULONG key = mySuper(PowerPCBase);
    struct BATArray* store = (struct BATArray*)task->tp_BATStorage;

    /*
     * SAVE FIRST, THEN INSTALL, matching GetBATs above. This used to install
     * pp_StoredBATs and only then read the hardware back into the task, so
     * what it stored was the four values it had just written -- the task's
     * own BATs were lost at the moment they were meant to be preserved.
     *
     * AND THE LAST OFFSET WAS +64. struct BATArray is four ULONGs = 16 bytes
     * and tp_BATStorage holds exactly four of them (functionsppc.c allocates
     * sizeof(struct BATArray) * 4; kernel.c points it at nt_BatStore[4]), so
     * valid offsets are 0, 16, 32 and 48 -- which is what GetBATs uses. The
     * +64 wrote 16 bytes PAST the end: onto nt_Name[0..15] via kernel.c,
     * since libstructs.h puts nt_Name[256] straight after nt_BatStore[4].
     * Indexing the array removes the literals so the stride cannot drift
     * again.
     *
     * NOT A LIVE BUG: the only caller of this function and of GetBATs is
     * myChangeMMU, whose first statement is "return; //stub". Fixed here so
     * it is not a trap for whoever implements that.
     */
    MoveFromBAT(CHMMU_BAT0, &store[0]);
    MoveFromBAT(CHMMU_BAT1, &store[1]);
    MoveFromBAT(CHMMU_BAT2, &store[2]);
    MoveFromBAT(CHMMU_BAT3, &store[3]);

    MoveToBAT(CHMMU_BAT0, (struct BATArray*)&PowerPCBase->pp_StoredBATs[0]);
    MoveToBAT(CHMMU_BAT1, (struct BATArray*)&PowerPCBase->pp_StoredBATs[1]);
    MoveToBAT(CHMMU_BAT2, (struct BATArray*)&PowerPCBase->pp_StoredBATs[2]);
    MoveToBAT(CHMMU_BAT3, (struct BATArray*)&PowerPCBase->pp_StoredBATs[3]);

    myUser(PowerPCBase, key);
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID MoveToBAT(__reg("r3") ULONG BATnumber, __reg("r4") struct BATArray* batArray)
{
    switch (BATnumber)
    {
       case CHMMU_BAT0:
       {
           mvtoBAT0(batArray);
           break;
       }
       case CHMMU_BAT1:
       {
           mvtoBAT1(batArray);
           break;
       }
       case CHMMU_BAT2:
       {
           mvtoBAT2(batArray);
           break;
       }
       case CHMMU_BAT3:
       {
           mvtoBAT3(batArray);
           break;
       }
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID MoveFromBAT(__reg("r3") ULONG BATnumber, __reg("r4") struct BATArray* batArray)
{
    switch (BATnumber)
    {
       case CHMMU_BAT0:
       {
           mvfrBAT0(batArray);
           break;
       }
       case CHMMU_BAT1:
       {
           mvfrBAT1(batArray);
           break;
       }
       case CHMMU_BAT2:
       {
           mvfrBAT2(batArray);
           break;
       }
       case CHMMU_BAT3:
       {
           mvfrBAT3(batArray);
           break;
       }
    }
    return;
}

/********************************************************************************************
*
*    Function to set up the system processes.
*
*********************************************************************************************/

PPCFUNCTION VOID SystemStart(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    struct TaskPPC* myTask;
    APTR myPool;
    struct MemList* myMem;
    struct Message* myNotice;

    while (1)
    {
        myWaitTime(PowerPCBase, 0, 0x4c0000); // Around 5 seconds.
        while (myTask = (struct TaskPPC*)myRemHeadPPC(PowerPCBase, (struct List*)&PowerPCBase->pp_RemovedTasks))
        {
            /*
             * NOT WHILE A HANDLER'S SIGNAL FOR IT STILL WAITS on pp_TaskLinks:
             * its tp_Link is in the task's memory. The kernel's next pass
             * takes it off (HandleDeferred), and a handler queues no task that
             * is already TS_REMOVED. FIXES.md "#1 PART B DESIGNED".
             */
            while (*((volatile UWORD*)&myTask->tp_Link.tl_Used))
            {
                CauseDECInterrupt(PowerPCBase);
            }

            if ((myTask->tp_Flags & TASKPPCF_CRASHED) && (myTask->tp_Flags & TASKPPCF_CREATORPPC))
            {
                myDeleteTaskPPC(PowerPCBase, myTask);
            }
            while (myPool = (APTR)myRemHeadPPC(PowerPCBase, (struct List*)&myTask->tp_TaskPools))
            {
                myDeletePoolPPC(PowerPCBase, myPool);
            }
            while (myMem = (struct MemList*)myRemHeadPPC(PowerPCBase, (struct List*)&myTask->tp_Task.tc_MemEntry))
            {
                FreeVec68K(PowerPCBase, myMem->ml_ME[0].me_Un.meu_Addr);
                FreeVec68K(PowerPCBase, myMem);
            }
            /*
             * THE PORT, THEN THE NOTICE, THEN THE TASK -- T3 B and W4 A
             * (REVIEW-LIB.md), in the order of WarpOS's reaper, USSVoyager
             * (powerpc.p:3700-3735): what the task owned, the notice, the task.
             * - THE PORT of a CreateTaskPPC task is freed here, no longer by the
             *   dying task itself (functionsppc.c, DeleteTaskPPC): its own
             *   FreeVec68K waits for the 68k's reply ON that port. A task killed
             *   by ID_END (KillTask) never freed it at all. The waits here are
             *   Kryten's own, on Kryten's port. A 68k-started task's port is
             *   inside its NewTask and goes with it.
             * - THE NOTICE is the message TASKATTR_NOTIFYMSG gave the task
             *   (tp_MessageRIP, functionsppc.c CreateTaskPPC), put on the
             *   message's own reply port once the task is removed from the
             *   system (WarpOS History.txt, 23.8.2000; powerpc.p:3724-3732, which
             *   sends nothing for a message with no reply port). Kryten runs
             *   every ~5 s, so it can come that late: WarpOS signals its reaper
             *   at once (powerpc.p:9512-9514), this does not.
             * - THE TaskPtr is inside the task block and goes with it.
             */
            if ((myTask->tp_Flags & TASKPPCF_CREATORPPC) && (myTask->tp_Msgport))
            {
                FreeVec68K(PowerPCBase, myTask->tp_Msgport->mp_Semaphore.ssppc_reserved);
                FreeVec68K(PowerPCBase, myTask->tp_Msgport);
            }
            if ((myNotice = myTask->tp_MessageRIP) && (myNotice->mn_ReplyPort))
            {
                myPutMsgPPC(PowerPCBase, (struct MsgPortPPC*)myNotice->mn_ReplyPort, myNotice);
            }
            FreeVec68K(PowerPCBase, myTask);
         }
    }
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID FreeAllExcMem(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct ExcInfo* myInfo)
{
    APTR myData;

    if (myData = myInfo->ei_MachineCheck)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_DataAccess)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_InstructionAccess)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_Alignment)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_Program)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_FPUnavailable)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_Decrementer)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_SystemCall)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_Trace)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_PerfMon)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_IABR)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    if (myData = myInfo->ei_Interrupt)
    {
        FreeVec68K(PowerPCBase,myData);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID AddExcList(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct ExcInfo* excInfo, __reg("r5") struct ExcData* newData, __reg("r6") ULONG* currExc, __reg("r7") ULONG flag)
{
    /*
     * FILLS THE ENTRY AND RECORDS IT IN excInfo; LinkExcList below links it
     * (E1, REVIEW-LIB.md). This added each entry to pp_ReadyExc at once.
     * SetExcHandler's next AllocVec68K is a 68k round trip, so meanwhile the
     * kernel's decrementer arm could move the entry onto a live list
     * (kernel.c:338-362). If a later allocation failed, FreeAllExcMem freed
     * it still linked, and the next exception of its type called its
     * handler through freed memory. 17.15 has it (src_175/supportppc.c:781-788).
     */
    myCopyMemPPC(PowerPCBase, (APTR)excInfo, (APTR)newData, sizeof(struct ExcData));
    currExc[0] = (ULONG)newData;
    newData->ed_ExcID = flag;
    excInfo->ei_ExcData.ed_LastExc = newData;
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID LinkExcList(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct ExcInfo* excInfo)
{
    /*
     * EVERY ENTRY AT ONCE, AFTER THE LAST ALLOCATION (E1): one pp_Mutex
     * section adds each entry AddExcList recorded to pp_ReadyExc, in
     * SetExcHandler's allocation order, so pp_ReadyExc ends as it did.
     * MATCHES WarpOS, which links only after every allocation succeeded,
     * under FORBID (powerpc.p:11424-11428, InsExcHandler :2300), and
     * 17.13b's .MakeLists under its atomic lock (ppcfunctions.p:9021-9035,
     * :9077), onto the same pending list.
     */
    struct Node* entry[12];

    entry[0]  = excInfo->ei_MachineCheck;
    entry[1]  = excInfo->ei_DataAccess;
    entry[2]  = excInfo->ei_InstructionAccess;
    entry[3]  = excInfo->ei_Interrupt;
    entry[4]  = excInfo->ei_Alignment;
    entry[5]  = excInfo->ei_Program;
    entry[6]  = excInfo->ei_FPUnavailable;
    entry[7]  = excInfo->ei_Decrementer;
    entry[8]  = excInfo->ei_SystemCall;
    entry[9]  = excInfo->ei_Trace;
    entry[10] = excInfo->ei_PerfMon;
    entry[11] = excInfo->ei_IABR;

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
    for (int i = 0; i < 12; i++)
    {
        if (entry[i])
        {
            myAddHeadPPC(PowerPCBase, (struct List*)&PowerPCBase->pp_ReadyExc, entry[i]);
        }
    }
    FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID SetupRunPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* myFrame)
{
    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    myTask->tp_Task.tc_SigRecvd |= myFrame->mf_Signals;
    FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

    ULONG myCode = (ULONG)myFrame->mf_PPCArgs.PP_Code + myFrame->mf_PPCArgs.PP_Offset;
    if (myFrame->mf_PPCArgs.PP_Offset)
    {
        myCode = *((ULONG*)(myCode + 2));
    }

    struct SnoopData* currSnoop = (struct SnoopData*)PowerPCBase->pp_Snoop.mlh_Head;
    struct SnoopData* nextSnoop;

    while (nextSnoop = (struct SnoopData*)currSnoop->sd_Node.ln_Succ)
    {
        if (currSnoop->sd_Type == SNOOP_START)
        {
            ULONG (*runSnoop)(__reg("r2") ULONG, __reg("r3") struct TaskPPC*,
            __reg("r4") ULONG, __reg("r5") struct Task*,
            __reg("r6") ULONG) = currSnoop->sd_Code;
            ULONG tempR2 = getR2();
            runSnoop(currSnoop->sd_Data, myTask, myCode, (struct Task*)myFrame->mf_Arg[2], CREATOR_68K);
            storeR2(tempR2);
        }
        currSnoop = nextSnoop;
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

    if ((myFrame->mf_PPCArgs.PP_Stack) && (myFrame->mf_PPCArgs.PP_StackSize))
    {
        mySetCache(PowerPCBase, CACHE_DCACHEINV, myFrame->mf_PPCArgs.PP_Stack, myFrame->mf_PPCArgs.PP_StackSize);
    }

    /* assign the 68k mask over this task's -- Sonnet's StartCode does
     * exactly this here, ppcfunctions.p:10334-10335; see kernel.c's ID_DONE
     * handler for the other sites and the reasoning */
    myTask->tp_Task.tc_SigAlloc = myFrame->mf_Arg[1];

    struct iframe storeFrame;

    RunCPP((struct iframe*)&storeFrame, myCode, &myFrame->mf_PPCArgs);

    struct MsgFrame* newFrame = CreateMsgFramePPC(PowerPCBase);

    newFrame->mf_Identifier = ID_FPPC;
    newFrame->mf_Message.mn_Length = MSGLEN;
    newFrame->mf_Message.mn_Node.ln_Type = NT_MESSAGE;

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    newFrame->mf_Arg[0]  = myTask->tp_Task.tc_SigAlloc;
    newFrame->mf_Signals = myTask->tp_Task.tc_SigRecvd & 0xfffff000;
    myTask->tp_Task.tc_SigRecvd &= 0xfff;

    FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

    newFrame->mf_Message.mn_ReplyPort = myFrame->mf_Message.mn_ReplyPort;
    newFrame->mf_PPCTask = myTask;

    myCopyMemPPC(PowerPCBase, &myFrame->mf_PPCArgs.PP_Regs, &newFrame->mf_PPCArgs.PP_Regs, (15*4) + (8*8));

    SendMsgFramePPC(PowerPCBase, newFrame);
    FreeMsgFramePPC(PowerPCBase, myFrame);

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID StartTask(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* myFrame)
{
#ifdef G4_EARLY_VEC
    /* The early-VEC test: the task's switch-on happens here, first thing,
     * instead of at its first aligned CopyMemPPC -- SetupRunPPC's own copy
     * just below, realloc, or Run68K's argument copy. */
    vecProbe();
#endif
    SetupRunPPC(PowerPCBase, myFrame);

    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;
    struct PrivateTask* myPTask = (struct PrivateTask*)myTask;
    struct MsgFrame* newFrame;
    struct Task* mirror68K;
    ULONG signals;
    ULONG mask;

    while(1)
    {
        signals = myWaitPPC(PowerPCBase, myTask->tp_Task.tc_SigAlloc & 0xfffff100);

        if (signals & SIGF_DOS)
        {
            if (mask = signals & ~SIGF_DOS)
            {
                while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

                myTask->tp_Task.tc_SigRecvd |= mask;

                FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
            }
            while (newFrame = (struct MsgFrame*)myGetMsgPPC(PowerPCBase, myTask->tp_Msgport))
            {
                switch (newFrame->mf_Identifier)
                {
                    case ID_TPPC:
                    {
                        SetupRunPPC(PowerPCBase, newFrame);
                        break;
                    }
                    case ID_END:
                    {
                        KillTask(PowerPCBase, newFrame);
                        break;
                    }
                    default:
                    {
                        HaltError(0xBAD0BAD0);
                        break; //error
                    }
                }
            }
        }
        else if (mask = signals & ~SIGF_DOS)
        {
            //X2: to pt_Mirror68K, as myWaitFor68K -- "pt_MirrorPort->mp_SigTask" read the 68k caller's port, a DSI through the 68k on every relay. DispatchPPC sets both (kernel.c:1569-1570). REVIEW-LIB.md X2
            if (mirror68K = myPTask->pt_Mirror68K)
            {
                mySignal68K(PowerPCBase, mirror68K, mask);
            }
        }
    }
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID EndTask(VOID)
{
    struct PPCZeroPage *myZP = 0;
    ULONG key = mySuper(NULL);             //Super does not use PowerPCBase
    struct PrivatePPCBase* PowerPCBase = (struct PrivatePPCBase*)myZP->zp_PowerPCBase;
    myUser(PowerPCBase, key);
    myDeleteTaskPPC(PowerPCBase, NULL);

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID KillTask(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* myFrame)
{
    FreeMsgFramePPC(PowerPCBase, myFrame);

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

    struct SnoopData* currSnoop = (struct SnoopData*)PowerPCBase->pp_Snoop.mlh_Head;
    struct SnoopData* nextSnoop;

    struct TaskPPC* PPCtask = PowerPCBase->pp_ThisPPCProc;

    while (nextSnoop = (struct SnoopData*)currSnoop->sd_Node.ln_Succ)
    {
        if (currSnoop->sd_Type == SNOOP_EXIT)
        {
            ULONG tempR2 = getR2();
            ULONG (*runSnoop)(__reg("r2") ULONG, __reg("r3") struct TaskPPC*) = currSnoop->sd_Code;
            runSnoop(currSnoop->sd_Data, PPCtask);
            storeR2(tempR2);
        }
        currSnoop = nextSnoop;
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

    if (PPCtask->tp_TaskPtr)
    {
        myRemovePPC(PowerPCBase, (struct Node*)PPCtask->tp_TaskPtr);
    }

    PowerPCBase->pp_NumAllTasks -= 1;

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

    PPCtask->tp_Task.tc_State = TS_REMOVED;
    PowerPCBase->pp_FlagReschedule = -1;
    CauseDECInterrupt(PowerPCBase);
    TaskHalt();
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG getNum(__reg("r3") struct RDFData* rdfData)
{
    ULONG result = 0;

    while (1)
    {
        UBYTE myChar = rdfData->rd_FormatString[0];
        rdfData->rd_FormatString += 1;
        if ((myChar >= '0') && (myChar <= '9'))
        {
            result = result * 10;
            myChar -= '0';
            result = result + myChar;
        }
        else
        {
            rdfData->rd_FormatString -= 1;
            break;
        }
    }
    return result;
}

/********************************************************************************************/

PPCFUNCTION LONG AdjustParamInt(__reg("r3") struct RDFData* rdfData)
{
    ULONG* mem = (ULONG*)rdfData->rd_DataStream;
    LONG value = mem[0];
    mem = (ULONG*)((ULONG)mem + 4);
    rdfData->rd_DataStream = (APTR)mem;
    return value;
}

/********************************************************************************************/

PPCFUNCTION LONG AdjustParam(__reg("r3") struct RDFData* rdfData, __reg("r4") ULONG flag)
{
    if (flag & RDFF_LONG)
    {
        return(AdjustParamInt(rdfData));
    }
    else
    {
        WORD value;
        UWORD* mem = (UWORD*)rdfData->rd_DataStream;
        value = mem[0];
        mem = (UWORD*)((ULONG)mem + 2);
        rdfData->rd_DataStream = (APTR)mem;
        return (LONG)value;
    }
}

/********************************************************************************************/

PPCFUNCTION VOID MakeDecimal(__reg("r3") struct RDFData* rdfData, __reg("r4") BOOL sign, __reg("r5") LONG value)
{
    if ((sign) && (value < 0))
    {
        rdfData->rd_BufPointer[0] = '-';
        rdfData->rd_BufPointer += 1;
        value = -value;
    }
    ULONG* myTable = GetDecTable();
    ULONG tableValue, number;
    ULONG cmpnumber = '0';
    {
        while (tableValue = myTable[0])
        {
            number = '0';
            while (tableValue <= value)
            {
               value -= tableValue;
               number += 1;
            }
            if (number != cmpnumber)
            {
               cmpnumber = 0;
               rdfData->rd_BufPointer[0] = (UBYTE)number;
               rdfData->rd_BufPointer += 1;
            }
            myTable += 1;
        }
    }
    rdfData->rd_BufPointer[0] = (UBYTE)(value + '0');
    rdfData->rd_BufPointer += 1;
    return;
}

/********************************************************************************************/

PPCFUNCTION VOID MakeHex(__reg("r3") struct RDFData* rdfData, __reg("r4") ULONG flag, __reg("r5") LONG value)
{
    ULONG iterations, currNibble;
    ULONG mySwitch = 0;

    if (value)
    {
        if (flag & RDFF_LONG)
        {
            iterations = 8;
        }
        else
        {
            iterations = 4;
            value = roll(value, 16);
        }
        /*
         * F2: THE VALUE ROTATES IN PLACE, a digit per turn, from the top.
         * The loop took roll(value, 4) without keeping it, so every digit was
         * the first one ("%lx" of 0x12345678 printed "11111111"), and roll()
         * itself rotated by 4 whatever it was given (Internalsppc.h). MATCHES
         * WarpOS's BinToHex, "rotlwi r21,r21,16" for a word, then
         * "rotlwi r21,r21,4" per digit (powerpc.p:14522-14552), and 17.13b
         * (ppcfunctions.p:9851-9885).
         */
        for (int i = 0; i < iterations; i++)
        {
            value = roll(value, 4);
            currNibble = value & 0xf;
            if ((currNibble) || (mySwitch))
            {
                mySwitch = -1;
                if (currNibble > 9)
                {
                    currNibble += 55;
                }
                else
                {
                    currNibble += 48;
                }

                rdfData->rd_BufPointer[0] = currNibble;
                rdfData->rd_BufPointer += 1;
            }
        }
        return;
    }
    rdfData->rd_BufPointer[0] = (UBYTE)(value + '0');
    rdfData->rd_BufPointer += 1;
    return;
}

/********************************************************************************************/

PPCFUNCTION VOID PerformPad(__reg("r3") struct RDFData* rdfData, __reg("r4") ULONG flag, __reg("r5") APTR (*putchproc)(), __reg("r6") LONG prependNum)
{
    UBYTE currChar;
    UBYTE useChar;

    if (flag & RDFF_PREPEND)
    {
        currChar = rdfData->rd_BufPointer[0];
        if (currChar == '-')
        {
            rdfData->rd_BufPointer += 1;
            rdfData->rd_TruncateNum -= 1;
            if (putchproc)
            {
                rdfData->rd_PutChData = putchproc(rdfData->rd_PutChData, currChar);
            }
            else
            {
                STRPTR myData = (STRPTR)rdfData->rd_PutChData;
                myData[0] = currChar;
                rdfData->rd_PutChData = (APTR)((ULONG)rdfData->rd_PutChData + 1);
            }
         }
        useChar = '0';
    }
    else
    {
        useChar = ' ';
    }
    if (prependNum)
    {
        for (int i = 0; i < prependNum; i++)
        {
            if (putchproc)
            {
                rdfData->rd_PutChData  = putchproc(rdfData->rd_PutChData, useChar);
            }
            else
            {
                STRPTR myData = (STRPTR)rdfData->rd_PutChData;
                myData[0] = useChar;
                rdfData->rd_PutChData = (APTR)((ULONG)rdfData->rd_PutChData + 1);
            }
        }
    }
    return;
}

/********************************************************************************************/
