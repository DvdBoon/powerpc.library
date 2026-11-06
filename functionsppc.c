// Copyright (c) 2019, 2020, 2024 Dennis van der Boon
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
#include <exec/types.h>
#include <devices/timer.h>
#include <powerpc/powerpc.h>
#include <powerpc/tasksPPC.h>
#include <powerpc/memoryPPC.h>
#include <powerpc/semaphoresPPC.h>
#include <powerpc/portsPPC.h>
#include "constants.h"
#include "libstructs.h"
#include "Internalsppc.h"
#pragma pack(pop)

/********************************************************************************************
*
*        LONG = Run68K(struct Library* PowerPCBase, struct PPCArgs* PPStruct)
*
*********************************************************************************************/

PPCFUNCTION LONG myRun68K(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct PPCArgs* PPStruct)
{
    if (PPStruct->PP_Offset)
	{
		if ((PPStruct->PP_Code) && (PPStruct->PP_Code == PowerPCBase->pp_UtilityBase) && (PPStruct->PP_Offset == _LVOStricmp))
		{
            PPStruct->PP_Regs[0] = (ULONG)StricmpPPC((STRPTR)PPStruct->PP_Regs[8], (STRPTR)PPStruct->PP_Regs[9]);
			return PPERR_SUCCESS;
		}
	}

    struct DebugArgs args;
    args.db_Function = 0 | (3<<8) | (1<<16) | (1<<17);
    args.db_Arg[0] = (ULONG)PPStruct;
    args.db_Arg[1] = (ULONG)PPStruct->PP_Code;            //Extra info
    args.db_Arg[2] = PPStruct->PP_Offset;                 //Extra info
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    /*
     * NO CALLER IS KILLED HERE, AND AN ASYNC CALL IS RUN -- Q1 (REVIEW-LIB.md).
     * Both tests below were HaltError, which removed the calling task.
     * - A NULL PP_Code returns PPERR_MISCERR, as 17.13b (ppcfunctions.p:
     *   3675-3678). WarpOS does not test it.
     * - PPF_ASYNC is run as WarpOS runs it (powerpc.p:16218-16251): the call
     *   is sent and Run68K returns PPERR_SUCCESS at once; WaitFor68K collects
     *   the reply (myWaitFor68KLVO, below). Another Run68K while one is out
     *   returns PPERR_ASYNCERR, as WarpOS's .asyncerr. PP_Flags' other bits
     *   are ignored, as WarpOS tests PPP_ASYNC only.
     */
	if (!(PPStruct->PP_Code))
	{
		return PPERR_MISCERR;
	}
	if (PowerPCBase->pp_ThisPPCProc->tp_Flags & TASKPPCF_ASYNC68K)
	{
		return PPERR_ASYNCERR;
	}

    PowerPCBase->pp_NumRun68k += 1;

	struct MsgFrame* myFrame = CreateMsgFramePPC(PowerPCBase);

    myCopyMemPPC(PowerPCBase, (APTR)PPStruct, (APTR)&myFrame->mf_PPCArgs, sizeof(struct PPCArgs));

	struct PrivateTask* privTask = (struct PrivateTask*)PowerPCBase->pp_ThisPPCProc;
    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

	myFrame->mf_Identifier = ID_T68K;
	myFrame->mf_PPCTask = (struct TaskPPC*)myTask;
	myFrame->mf_MirrorPort = privTask->pt_MirrorPort;
	myFrame->mf_Arg[0] = myTask->tp_Task.tc_SigAlloc;
    myFrame->mf_Arg[1] = 0;
	myFrame->mf_Arg[2] = 0;

	while(!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

	myFrame->mf_Signals = myTask->tp_Task.tc_SigRecvd & ~0xfff;
	myTask->tp_Task.tc_SigRecvd &= 0xfff;

	FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

	if (PPStruct->PP_Stack)
	{
		if(PPStruct->PP_StackSize)
		{
            mySetCache(PowerPCBase, CACHE_DCACHEFLUSH, PPStruct->PP_Stack, PPStruct->PP_StackSize);
		}
		else
		{
			myFrame->mf_PPCArgs.PP_Stack = NULL;
		}
	}

	myFrame->mf_Message.mn_Node.ln_Type = NT_MESSAGE;
	myFrame->mf_Message.mn_Length = sizeof(struct MsgFrame);

	if (PowerPCBase->pp_PowerPCBase.PPC_SysLib == PPStruct->PP_Code)
	{
		if((PPStruct->PP_Offset == _LVOAllocMem) || (PPStruct->PP_Offset == _LVOAllocVec))
		{
			if(!(PPStruct->PP_Regs[1] & MEMF_CHIP))
			{
				myFrame->mf_PPCArgs.PP_Regs[1] |= MEMF_PPC;
			}
		}
	}

	SendMsgFramePPC(PowerPCBase, myFrame);

	if (PPStruct->PP_Flags & PPF_ASYNC)
	{
		myTask->tp_Flags |= TASKPPCF_ASYNC68K;      //Q1: out until WaitFor68K
		return PPERR_SUCCESS;
	}

	myWaitFor68K(PowerPCBase, PPStruct);

    args.db_Arg[0] = PPStruct->PP_Regs[0];
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return PPERR_SUCCESS;
}

/********************************************************************************************
*
*        LONG = WaitFor68K(struct Library*, struct PPCArgs*)
*
*********************************************************************************************/


PPCFUNCTION LONG myWaitFor68K(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct PPCArgs* PPStruct)
{
    struct DebugArgs args;
    args.db_Function = 1 | (1<<8) | (1<<16) | (1<<17);
    args.db_Arg[0] = (ULONG)PPStruct;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    ULONG recvdSigs, signals;
    ULONG sigMask = SIGF_DOS;
    ULONG heldSigs = 0;     //Q2: signals no 68k mirror could take, given back when the call returns
    struct MsgFrame* deferred = NULL;   //Q1: an async call's reply that came during another wait

    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    while (1)
    {
        recvdSigs = myWaitPPC(PowerPCBase, myTask->tp_Task.tc_SigAlloc & 0xfffff100 & ~heldSigs);
        if (recvdSigs & sigMask)
        {
            if (signals = recvdSigs & ~sigMask)
            {
                while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
                myTask->tp_Task.tc_SigRecvd |= signals;
                FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
            }
            struct MsgFrame* myFrame;
            while (myFrame = (struct MsgFrame*)myGetMsgPPC(PowerPCBase, myTask->tp_Msgport))
            {
                switch (myFrame->mf_Identifier)
                {
                    case ID_DONE:
                    {
                        /*
                         * Q1: WHILE AN ASYNC Run68K IS OUT, a reply that comes
                         * during any other wait -- Run68KLowLevel's, below --
                         * is that call's. It is kept and put back on the port
                         * when this wait ends, for WaitFor68K. myWaitFor68KLVO
                         * clears the flag before it waits, so its own wait
                         * takes the reply here as a normal call's.
                         */
                        if (myTask->tp_Flags & TASKPPCF_ASYNC68K)
                        {
                            deferred = myFrame;
                            break;
                        }
                        struct NewTask* myNewTask = (struct NewTask*)myTask;
                        if (!(myNewTask->nt_Task.pt_MirrorPort))
                        {
                            myNewTask->nt_Task.pt_MirrorPort = myFrame->mf_MirrorPort;
                            myNewTask->nt_Task.pt_Mirror68K = (struct Task*)myFrame->mf_Arg[0];
                        }

                        while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
                        myTask->tp_Task.tc_SigRecvd |= myFrame->mf_Signals | heldSigs;
                        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

                        myCopyMemPPC(PowerPCBase, (APTR)&myFrame->mf_PPCArgs, (APTR)PPStruct, sizeof(struct PPCArgs));
                        FreeMsgFramePPC(PowerPCBase, myFrame);

                        return PPERR_SUCCESS;
                    }
                    case ID_TPPC:
                    {
                        SetupRunPPC(PowerPCBase, myFrame);
                        break;
                    }
                    case ID_DNLL:
                    {
                        ULONG value = myFrame->mf_PPCArgs.PP_Regs[0];
                        FreeMsgFramePPC(PowerPCBase, myFrame);
                        if (heldSigs)       //Q2: as ID_DONE
                        {
                            while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
                            myTask->tp_Task.tc_SigRecvd |= heldSigs;
                            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
                        }
                        if (deferred)       //Q1: the async call's reply, back on the port for WaitFor68K
                        {
                            myPutMsgPPC(PowerPCBase, myTask->tp_Msgport, (struct Message*)deferred);
                        }
                        return (LONG)value;
                    }
                    case ID_END:
                    {
                        KillTask(PowerPCBase, myFrame);
                        break;
                    }
                    default:
                    {
                        HaltError(ERR_EFIF);
                        break;
                    }
                }
            }

        }
        else
        {
            struct NewTask* myNewTask = (struct NewTask*)myTask;
            struct Task* mirror68K;
            if (signals = recvdSigs & ~sigMask)
            {
                //X2: TO THE MIRROR THE TASK HOLDS, pt_Mirror68K. This read "pt_MirrorPort->mp_SigTask": that port is in Amiga memory (MirrorTask creates it with TF_PPC off), so every relay was a DSI proxied through the 68k, the PPC spinning with interrupts off until the answer -- subdemo 3, the first step that relays, froze the machine. pt_Mirror68K is set with pt_MirrorPort (:168, kernel.c:1569). WarpOS relays to CORE_PROC68K, held on the PPC side (powerpc.p:15697-15698). K2 had been overwriting pt_Mirror68K below the iframe. REVIEW-LIB.md X2
                if ((mirror68K = myNewTask->nt_Task.pt_Mirror68K) && (!(PowerPCBase->pp_PowerPCBase.PPC_Flags & LIB_FLAGPPC_SCC)))
                {
                    mySignal68K(PowerPCBase, mirror68K, signals);
                }
                else
                {
                    //Q2: NO MIRROR TO RELAY THEM TO -- a CreateTaskPPC task before its first Run68K returns (pt_MirrorPort is set from its ID_DONE), or SetCPUComm. WaitPPC has already cleared them, so dropping them lost them for good (17.13b too, ".WasNoMsg -- No 68k mirror exists (yet)"). Keep them, out of the wait, for this task. WarpOS always has CORE_PROC68K to relay to (powerpc.p:15694-15700). REVIEW-LIB.md Q2
                    heldSigs |= signals;
                }
            }
        }
    }
    return 0;
}

/********************************************************************************************
*
*        LONG = WaitFor68K(struct Library*, struct PPCArgs*), the library call -- Q1
*
*        Collects an async Run68K. With none out it returns PPERR_WAITERR at once, as
*        WarpOS's WaitFor68K (powerpc.p:16277-16315); it used to be myWaitFor68K itself,
*        which then waited for a reply that never came. Run68K and Run68KLowLevel go on
*        calling myWaitFor68K directly.
*
*********************************************************************************************/

PPCFUNCTION LONG myWaitFor68KLVO(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct PPCArgs* PPStruct)
{
    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    if (!(myTask->tp_Flags & TASKPPCF_ASYNC68K))
    {
        return PPERR_WAITERR;
    }
    myTask->tp_Flags &= ~TASKPPCF_ASYNC68K;

    return myWaitFor68K(PowerPCBase, PPStruct);
}

/********************************************************************************************
*
*        VOID mySPrintF (struct Library*, STRPTR, APTR)
*
*********************************************************************************************/

PPCFUNCTION VOID mySPrintF(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") STRPTR Formatstring, __reg("r5") APTR Values)
{
    myRun68KLowLevel(PowerPCBase, (ULONG)PowerPCBase, (LONG)_LVOSprintF68K,
                        (ULONG)Formatstring, (ULONG)Values, 0, 0);
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION APTR myAllocVecPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG size, __reg("r5") ULONG flags, __reg("r6") ULONG align)
{
    struct DebugArgs args;
    args.db_Function = 2 | (3<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = size;
    args.db_Arg[1] = flags;
    args.db_Arg[2] = align;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);
    APTR mem = 0;

    flags = (flags & ~MEMF_CHIP) | MEMF_PPC;

    struct poolHeader* currPool = (struct poolHeader*)PowerPCBase->pp_ThisPPCProc->tp_TaskPools.lh_Head;
    struct poolHeader* nextPool;

    while (nextPool = (struct poolHeader*)currPool->ph_Node.mln_Succ)
    {
        if ((currPool->ph_ThresholdSize == 0x40020) && (currPool->ph_Requirements == flags))
        {
            break;
        }
        currPool = nextPool;
    }

    if (!(align) || (align & 0x1f))
    {
        align = 32;
    }
    else
    {
        ULONG zeros = getLeadZ(align - 1);
        zeros = 32 - zeros;
        align = 1 << zeros;
    }
    if (!(nextPool))
    {
        currPool = myCreatePoolPPC(PowerPCBase, flags, 0x80200, 0x40020);
    }
    if (currPool)
    {
        mem = myAllocPooledPPC(PowerPCBase, currPool, size + align + 32);
    }
    if (mem)
    {
        ULONG origmem = (ULONG)mem;
        mem = (APTR)(((ULONG)mem + 31 + align) & (-align));
        ULONG* values = mem;
        values[-1] = size + align + 32;
        values[-2] = (ULONG)currPool;
        values[-3] = origmem;
        values[-4] = MAGIC_COOKIE;
    }
    args.db_Arg[0] = (ULONG)mem;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return mem;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myFreeVecPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") APTR memblock)
{
    struct DebugArgs args;
    args.db_Function = 3 | (1<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)memblock;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	if (memblock)
	{
		ULONG* myMemblock = memblock;
        if (myMemblock[-4] != MAGIC_COOKIE)
        {
            HaltError(ERR_ECOR);
        }
        else
        {
		    myFreePooledPPC(PowerPCBase, (APTR)myMemblock[-2], (APTR)myMemblock[-3], myMemblock[-1]);
        }
	}
	
    return MEMERR_SUCCESS;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct TaskPPC* myCreateTaskPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TagItem* taglist)
{
    struct DebugArgs args;
    args.db_Function = 4 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)taglist;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    ULONG value, memStack, myCode;
    LONG longvalue;
    APTR memBATStorage, memName;

    struct TaskPPC* newTask;
    struct MemList* memBATml;
    struct MemList* memNameml;
    struct MemList* memStackml;
    struct MemList* memFrameml;

    struct iframe* memFrame;
    struct MsgPortPPC* memPort;
    struct TaskPtr* taskPtr;

    struct TaskPPC* currTask = PowerPCBase->pp_ThisPPCProc;
    currTask->tp_Flags |= TASKPPCF_CHOWN;

    if (myCode = myGetTagDataPPC(PowerPCBase, TASKATTR_CODE, 0, taglist))
    {
        if (newTask = AllocVec68K(PowerPCBase, sizeof(struct PrivateTask), MEMF_PUBLIC | MEMF_CLEAR))
        {
            newTask->tp_Link.tl_Task = newTask;
            newTask->tp_Link.tl_Sig = 0xfff;
            myNewListPPC(PowerPCBase, (struct List*)&newTask->tp_Task.tc_MemEntry);
            newTask->tp_Task.tc_Node.ln_Type = NT_PPCTASK;
            newTask->tp_Task.tc_Flags = TF_PROCTIME;
            newTask->tp_PowerPCBase = PowerPCBase;
            if (memBATStorage = AllocVec68K(PowerPCBase, sizeof(struct BATArray) * 4, MEMF_PUBLIC | MEMF_CLEAR))
            {
                newTask->tp_BATStorage = memBATStorage;
                if (memBATml = AllocVec68K(PowerPCBase, sizeof(struct MemList), MEMF_PUBLIC | MEMF_CLEAR))
                {
                    memBATml->ml_NumEntries = 1;
                    memBATml->ml_ME[0].me_Un.meu_Addr = memBATStorage;
                    memBATml->ml_ME[0].me_Length = sizeof(struct BATArray) * 4;
                    myAddHeadPPC(PowerPCBase, (struct List*)&newTask->tp_Task.tc_MemEntry, (struct Node*)memBATml);

                    if (value = myGetTagDataPPC(PowerPCBase, TASKATTR_NAME, 0, taglist))
                    {
                        ULONG nameSize = GetLen((STRPTR)value) + 1;
                        if (memName = AllocVec68K(PowerPCBase, nameSize, MEMF_PUBLIC | MEMF_CLEAR))
                        {
                            if (memNameml = AllocVec68K(PowerPCBase, sizeof(struct MemList), MEMF_PUBLIC | MEMF_CLEAR))
                            {
                                memNameml->ml_NumEntries = 1;
                                memNameml->ml_ME[0].me_Un.meu_Addr = memName;
                                memNameml->ml_ME[0].me_Length = nameSize;
                                myAddHeadPPC(PowerPCBase, (struct List*)&newTask->tp_Task.tc_MemEntry, (struct Node*)memNameml);
                                CopyStr((APTR)value, (APTR)memName);
                                newTask->tp_Task.tc_Node.ln_Name = memName;

                                if (value = myGetTagDataPPC(PowerPCBase, TASKATTR_SYSTEM, 0, taglist))
                                {
                                    newTask->tp_Flags = TASKPPCF_SYSTEM;
                                }

                                if (value = myGetTagDataPPC(PowerPCBase, TASKATTR_ATOMIC, 0, taglist))
                                {
                                    newTask->tp_Flags |= TASKPPCF_ATOMIC;
                                }

                                myNewListPPC(PowerPCBase, (struct List*)&newTask->tp_TaskPools);

                                newTask->tp_Task.tc_Node.ln_Pri = myGetTagDataPPC(PowerPCBase, TASKATTR_PRI, 0, taglist);

                                longvalue = (LONG)myGetTagDataPPC(PowerPCBase, TASKATTR_NICE, 0, taglist);

                                if (longvalue < -20)
                                {
                                    longvalue = -20;
                                }
                                else if (longvalue > 20)
                                {
                                    longvalue = 20;
                                }

                                newTask->tp_Nice = longvalue;

                                if (value = myGetTagDataPPC(PowerPCBase, TASKATTR_MOTHERPRI, 0, taglist))
                                {
                                    newTask->tp_Task.tc_Node.ln_Pri = currTask->tp_Task.tc_Node.ln_Pri;
                                    newTask->tp_Nice = currTask->tp_Nice;
                                }

                                value = myGetTagDataPPC(PowerPCBase, TASKATTR_STACKSIZE, 0x10000, taglist);

                                if (value < 0x10000)
                                {
                                    value = 0x10000;
                                }

                                newTask->tp_StackSize = value;

                                if (memStack = (ULONG)AllocVec68K(PowerPCBase, value, MEMF_PUBLIC | MEMF_CLEAR))
                                {
                                    ULONG SPointer = memStack + value;
                                    newTask->tp_Task.tc_SPLower = (APTR)memStack;
                                    newTask->tp_Task.tc_SPUpper = (APTR)SPointer;
                                    SPointer = (SPointer - 56) & -32;
                                    newTask->tp_Task.tc_SPReg = (APTR)SPointer;
                                    newTask->tp_StackMem = (APTR)memStack;

                                    if (memStackml = AllocVec68K(PowerPCBase, sizeof(struct MemList), MEMF_PUBLIC | MEMF_CLEAR))
                                    {
                                        memStackml->ml_NumEntries = 1;
                                        memStackml->ml_ME[0].me_Un.meu_Addr = (APTR)memStack;
                                        memStackml->ml_ME[0].me_Length = value;
                                        myAddHeadPPC(PowerPCBase, (struct List*)&newTask->tp_Task.tc_MemEntry, (struct Node*)memStackml);

                                        if (memFrame = AllocVec68K(PowerPCBase, sizeof(struct iframe), MEMF_PUBLIC | MEMF_CLEAR))
                                        {
                                            newTask->tp_ContextMem = memFrame;
                                            memFrame->if_Context.ec_GPR[1] = SPointer;
                                            if (memFrameml = AllocVec68K(PowerPCBase, sizeof(struct MemList), MEMF_PUBLIC | MEMF_CLEAR))
                                            {
                                                memFrameml->ml_NumEntries = 1;
                                                memFrameml->ml_ME[0].me_Un.meu_Addr = (APTR)memFrame;
                                                memFrameml->ml_ME[0].me_Length = sizeof(struct iframe);
                                                myAddHeadPPC(PowerPCBase, (struct List*)&newTask->tp_Task.tc_MemEntry, (struct Node*)memFrameml);

                                                memFrame->if_Context.ec_UPC.ec_SRR0 = myCode;
                                                memFrame->if_Context.ec_SRR1 = MACHINESTATE_DEFAULT;
                                                memFrame->if_Context.ec_GPR[2] = getR2();

                                                /*
                                                 * TASKATTR_BAT: THE FLAG, AND EVERY TASK'S BATs. WarpOS gives
                                                 * such a task PPC_BAT and sets TASKPPCF_BAT (powerpc.p:9130-9143),
                                                 * but PPC_BAT maps the Amiga space with EA == PA
                                                 * (powerpc.p:529-578), which no PCI card has, and this library
                                                 * has no BAT mode (ChangeMMU is a stub, as in 17.13b). This used
                                                 * to copy pp_StoredBATs, which nothing fills: the task started
                                                 * with all four BATs zero ("Not working"). 17.13b set the flag
                                                 * and copied an array nothing filled either
                                                 * (ppcfunctions.p:4977-5004), so its tasks all ran alike. Now
                                                 * the flag is set as in both originals and the BATs are the
                                                 * system ones every task gets. FIXES.md "ITEM 14C", 31 Oct 2026.
                                                 */
                                                if (value = myGetTagDataPPC(PowerPCBase, TASKATTR_BAT, 0, taglist))
                                                {
                                                    newTask->tp_Flags |= TASKPPCF_BAT;
                                                }
                                                myCopyMemPPC(PowerPCBase, &PowerPCBase->pp_SystemBATs, &memFrame->if_BATs, sizeof(struct BATArray) * 4);
                                                myCopyMemPPC(PowerPCBase, &PowerPCBase->pp_SystemBATs, newTask->tp_BATStorage, sizeof(struct BATArray) * 4);

                                                for (int i = 0; i < 16; i++)
                                                {
                                                    memFrame->if_Segments[i] = PowerPCBase->pp_SystemSegs[i];
                                                }
                                                memFrame->if_SDR1 = 0;      /* the library's table, kernel.s exit */

                                                value = *((ULONG*)((ULONG)PowerPCBase + _LVOEndTask + 2));

                                                memFrame->if_Context.ec_LR = myGetTagDataPPC(PowerPCBase, TASKATTR_EXITCODE, value, taglist);

                                                if (value = myGetTagDataPPC(PowerPCBase, TASKATTR_PRIVATE, 0, taglist))
                                                {
                                                    memFrame->if_Context.ec_GPR[13] = value | 1;
                                                }
                                                else
                                                {
                                                    memFrame->if_Context.ec_GPR[13] = (ULONG)PowerPCBase->pp_ThisPPCProc;
                                                }

                                                if (value = myGetTagDataPPC(PowerPCBase, TASKATTR_INHERITR2, 0, taglist))
                                                {
                                                    memFrame->if_Context.ec_GPR[2] = getR2();
                                                }
                                                else
                                                {
                                                    memFrame->if_Context.ec_GPR[2] = myGetTagDataPPC(PowerPCBase, TASKATTR_R2, 0, taglist);
                                                }

                                                memFrame->if_Context.ec_GPR[3] = myGetTagDataPPC(PowerPCBase, TASKATTR_R3, 0, taglist);
                                                memFrame->if_Context.ec_GPR[4] = myGetTagDataPPC(PowerPCBase, TASKATTR_R4, 0, taglist);
                                                memFrame->if_Context.ec_GPR[5] = myGetTagDataPPC(PowerPCBase, TASKATTR_R5, 0, taglist);
                                                memFrame->if_Context.ec_GPR[6] = myGetTagDataPPC(PowerPCBase, TASKATTR_R6, 0, taglist);
                                                memFrame->if_Context.ec_GPR[7] = myGetTagDataPPC(PowerPCBase, TASKATTR_R7, 0, taglist);
                                                memFrame->if_Context.ec_GPR[8] = myGetTagDataPPC(PowerPCBase, TASKATTR_R8, 0, taglist);
                                                memFrame->if_Context.ec_GPR[9] = myGetTagDataPPC(PowerPCBase, TASKATTR_R9, 0, taglist);
                                                memFrame->if_Context.ec_GPR[10] = myGetTagDataPPC(PowerPCBase, TASKATTR_R10, 0, taglist);

                                                if (memPort = AllocVec68K(PowerPCBase, sizeof(struct MsgPortPPC), MEMF_PUBLIC | MEMF_CLEAR))
                                                {
                                                    myNewListPPC(PowerPCBase, (struct List*)&memPort->mp_IntMsg);
                                                    myNewListPPC(PowerPCBase, (struct List*)&memPort->mp_Port.mp_MsgList);

                                                    newTask->tp_Task.tc_SigAlloc = SYS_SIGALLOC;

                                                    memPort->mp_Port.mp_SigBit = SIGB_DOS;

                                                    if (myInitSemaphorePPC(PowerPCBase, &memPort->mp_Semaphore) == -1)
                                                    {
                                                        memPort->mp_Port.mp_SigTask = newTask;
                                                        memPort->mp_Port.mp_Flags = PA_SIGNAL;
                                                        memPort->mp_Port.mp_Node.ln_Type = NT_MSGPORTPPC;
                                                        newTask->tp_Msgport = memPort;

                                                        newTask->tp_PoolMem = myGetTagDataPPC(PowerPCBase, TASKATTR_NOTIFYMSG, 0, taglist);

                                                        if (newTask->tp_MessageRIP = AllocVec68K(PowerPCBase, 928, MEMF_PUBLIC | MEMF_CLEAR))
                                                        {
                                                            if (taskPtr = AllocVec68K(PowerPCBase, sizeof(struct TaskPtr), MEMF_PUBLIC | MEMF_CLEAR))
                                                            {
                                                                taskPtr->tptr_Task = newTask;
                                                                newTask->tp_TaskPtr = taskPtr;
                                                                taskPtr->tptr_Node.ln_Name = newTask->tp_Task.tc_Node.ln_Name;

                                                                myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);
                                                                myAddTailPPC(PowerPCBase, (struct List*)&PowerPCBase->pp_AllTasks, (struct Node*)taskPtr);
                                                                PowerPCBase->pp_NumAllTasks += 1;
                                                                myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

                                                                while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

                                                                if (newTask->tp_Flags & TASKPPCF_SYSTEM)
                                                                {
                                                                    PowerPCBase->pp_IdSysTasks += 1;
                                                                    newTask->tp_Id = PowerPCBase->pp_IdSysTasks;
                                                                }
                                                                else
                                                                {
                                                                    PowerPCBase->pp_IdUsrTasks += 1;
                                                                    newTask->tp_Id = PowerPCBase->pp_IdUsrTasks;
                                                                }

                                                                newTask->tp_Quantum = PowerPCBase->pp_StdQuantum;
                                                                newTask->tp_Task.tc_State = TS_READY;
                                                                /*
                                                                 * tp_Desired IS COMPUTED HERE, NOT LEFT TO CPUStats,
                                                                 * powerpc.p:9329-9337 inside IFNE NEWSCHEDULER:
                                                                 *
                                                                 *     lwz   r7,TASKPPC_NICE(r31)
                                                                 *     addi  r8,r7,NICE_RANGE/2
                                                                 *     slwi  r8,r8,2
                                                                 *     lwz   r7,PPC_NICETAB(r23)
                                                                 *     lwzx  r8,r7,r8
                                                                 *     srwi  r8,r8,8
                                                                 *     liw   r7,PRI_OFFSET*65536
                                                                 *     divwu r0,r7,r8
                                                                 *     stw   r0,TASKPPC_DESIRED(r31)
                                                                 *
                                                                 * which is CPUStats' formula with activity 0, since a
                                                                 * task that has never run has no activity to measure.
                                                                 *
                                                                 * This said "= 0; //CPUStats computes it from the NICE
                                                                 * table" and that mattered as soon as the V14 rule went
                                                                 * in: priDecay declines to judge a task whose tp_Desired
                                                                 * is zero -- it cannot, the quotient would be a division
                                                                 * by zero -- so a new task took NO penalty until the
                                                                 * first CPUStats pass, and a nice +19 task created and
                                                                 * run inside that window was scheduled as nice 0.
                                                                 *
                                                                 * tp_Nice is already clamped to -20..20 above, so the
                                                                 * index is 0..40 and in range. The table is read through
                                                                 * pp_NICETable rather than by calling GetNiceTable: that
                                                                 * lives in the SETUP hunk and this is the FUNCTIONS
                                                                 * hunk, and a direct call between them would land where
                                                                 * the callee used to be.
                                                                 */
                                                                newTask->tp_Desired = 0;

                                                                if (PowerPCBase->pp_NICETable)
                                                                {
                                                                    const ULONG* niceTab = (const ULONG*)PowerPCBase->pp_NICETable;
                                                                    ULONG weight = niceTab[(LONG)newTask->tp_Nice + (NICE_RANGE / 2)] >> 8;

                                                                    if (weight)
                                                                    {
                                                                        newTask->tp_Desired = (PRI_OFFSET << 16) / weight;
                                                                    }
                                                                }
                                                                newTask->tp_Flags |= TASKPPCF_CREATORPPC;

                                                                InsertOnPri(PowerPCBase, (struct List*)&PowerPCBase->pp_ReadyTasks, newTask);

                                                                PowerPCBase->pp_FlagReschedule = -1;

                                                                FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

                                                                myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

                                                                struct SnoopData* currSnoop = (struct SnoopData*)PowerPCBase->pp_Snoop.mlh_Head;
                                                                struct SnoopData* nextSnoop;

                                                                while (nextSnoop = (struct SnoopData*)currSnoop->sd_Node.ln_Succ)
                                                                {
                                                                    if (currSnoop->sd_Type == SNOOP_START)
                                                                    {
                                                                        ULONG (*runSnoop)(__reg("r2") ULONG, __reg("r3") struct TaskPPC*,
                                                                                          __reg("r4") ULONG, __reg("r5") struct TaskPPC*,
                                                                                          __reg("r6") ULONG) = currSnoop->sd_Code;
                                                                        ULONG tempR2 = getR2();
                                                                        runSnoop(currSnoop->sd_Data, newTask, myCode, currTask, CREATOR_PPC);
                                                                        storeR2(tempR2);
                                                                    }
                                                                    currSnoop = nextSnoop;
                                                                }

                                                                myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

                                                                CauseDECInterrupt(PowerPCBase);

                                                                currTask->tp_Flags &= ~TASKPPCF_CHOWN;
                                                                args.db_Arg[0] = (ULONG)newTask;
                                                                printDebug(PowerPCBase, (struct DebugArgs*)&args);

                                                                return newTask;
                                                            }
                                                            FreeVec68K(PowerPCBase, newTask->tp_MessageRIP);
                                                        }
                                                        FreeVec68K(PowerPCBase, memPort->mp_Semaphore.ssppc_reserved);
                                                    }
                                                    FreeVec68K(PowerPCBase, memPort);
                                                }
                                                FreeVec68K(PowerPCBase, memFrameml);
                                            }
                                            FreeVec68K(PowerPCBase, memFrame);
                                        }
                                        FreeVec68K(PowerPCBase, memStackml);
                                    }
                                    FreeVec68K(PowerPCBase, (APTR)memStack);
                                }
                                FreeVec68K(PowerPCBase, memNameml);
                            }
                            FreeVec68K(PowerPCBase, memName);
                        }
                    }
                    FreeVec68K(PowerPCBase, memBATml);
                }
                FreeVec68K(PowerPCBase, memBATStorage);
            }
            FreeVec68K(PowerPCBase, newTask);
        }
    }
    currTask->tp_Flags &= ~TASKPPCF_CHOWN;                              //R4: on failure too, as WarpOS ".end" (powerpc.p:9387-9393) and 17.13b ".SkipToEnd" (ppcfunctions.p:5409-5414). FIXES.md "CODE REVIEW OF lib/"
    args.db_Arg[0] = NULL;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);
    return NULL;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myDeleteTaskPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TaskPPC* PPCtask)
{
    struct DebugArgs args;
    args.db_Function = 5 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)PPCtask;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    ULONG ownFlag = 0;

    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;
    if ((!(PPCtask)) || (PPCtask == myTask))
    {
        ownFlag = 1;
        PPCtask = myTask;
    }

    /*
     * ONE TEARDOWN PER TASK (W3, REVIEW-LIB.md). Kryten (supportppc.c
     * SystemStart) deletes a crashed task its parent may already have
     * deleted: the second call returns here, before the snoops, the port
     * and ID_END are done again. The flag is tested and set under
     * pp_SemTaskList, which every DeleteTaskPPC takes. The task list and
     * the count follow tp_TaskPtr below, which the first removal clears --
     * here, or in CommonExcError for a crashed task. 17.13b's Kryten never
     * calls DeleteTaskPPC (ppcfunctions.p:9905-9944); WarpOS has no Kryten.
     */
    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);
    ULONG wasDeleted = PPCtask->tp_Flags & TASKPPCF_DELETED;
    PPCtask->tp_Flags |= TASKPPCF_DELETED;
    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

    if (wasDeleted)
    {
        return;
    }

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

    struct SnoopData* currSnoop = (struct SnoopData*)PowerPCBase->pp_Snoop.mlh_Head;
    struct SnoopData* nextSnoop;

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

    if ((PPCtask->tp_Flags & TASKPPCF_CREATORPPC) && (PPCtask->tp_Msgport))
    {
        FreeVec68K(PowerPCBase, PPCtask->tp_Msgport->mp_Semaphore.ssppc_reserved);
        FreeVec68K(PowerPCBase, PPCtask->tp_Msgport);
    }

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

    if (PPCtask->tp_TaskPtr)
    {
        myRemovePPC(PowerPCBase, (struct Node*)PPCtask->tp_TaskPtr);
        PPCtask->tp_TaskPtr = NULL;                     //W3: removed and counted once
        PowerPCBase->pp_NumAllTasks -= 1;
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

    struct MsgPort* mirrorPort;
    struct NewTask* PPCNewtask = (struct NewTask*)PPCtask;

    if (mirrorPort = PPCNewtask->nt_Task.pt_MirrorPort)
    {
        struct MsgFrame* myFrame = CreateMsgFramePPC(PowerPCBase);
        myFrame->mf_Identifier = ID_END;
        myFrame->mf_MirrorPort = mirrorPort;
        SendMsgFramePPC(PowerPCBase, myFrame);
    }

    if (ownFlag)
    {
        PPCtask->tp_Task.tc_State = TS_REMOVED;
        PowerPCBase->pp_FlagReschedule = -1;
        CauseDECInterrupt(PowerPCBase);
        TaskHalt();
    }
    else if (!(PPCtask->tp_Flags & (TASKPPCF_CRASHED)))
    {
        /*
         * ITS TIMER NODE COMES OFF pp_WaitTime FIRST (T2, REVIEW-LIB.md). A
         * task deleted inside WaitTime left its node -- a local on its own
         * stack (myWaitTime) -- on pp_WaitTime. At the deadline the kernel's
         * sweep (kernel.c TaskCheck) marked the deleted task TS_READY and
         * unlinked the node; once Kryten had freed the stack, it walked and
         * wrote freed memory instead.
         * The node is found by wt_Task and taken off as WaitTime handles
         * the list: pp_SemWaitList against another WaitTime's AddHead,
         * pp_Mutex against its removal after WaitPPC, and pp_FlagWait raised
         * so the sweep skips the list (it runs only with pp_Mutex free,
         * kernel.c:310 and :392, but WaitTime raises it there too).
         * DIVERGES from WarpOS, which has the same gap: the node is on the
         * stack (powerpc.p:11740-11741) and this path only moves the task
         * (:9523-9531). 17.13b the same (ppcfunctions.p:9267-9270, :6463-6484).
         * NOT DONE: the SemWait node of a task deleted while it waits for a
         * semaphore (myObtainSemaphorePPC, a stack local) stays on that
         * semaphore's queue -- the same class, REVIEW-LIB.md T2.
         */
        myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemWaitList);
        while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
        PowerPCBase->pp_FlagWait++;

        struct WaitTime* currWait = (struct WaitTime*)PowerPCBase->pp_WaitTime.mlh_Head;
        struct WaitTime* nxtWait;

        while (nxtWait = (struct WaitTime*)currWait->wt_Node.ln_Succ)
        {
            if (currWait->wt_Task == PPCtask)
            {
                myRemovePPC(PowerPCBase, (struct Node*)currWait);
            }
            currWait = nxtWait;
        }

        PowerPCBase->pp_FlagWait--;
        myRemovePPC(PowerPCBase, (struct Node*)PPCtask);
        myAddTailPPC(PowerPCBase, (struct List*)&PowerPCBase->pp_RemovedTasks, (struct Node*)PPCtask);
        PPCtask->tp_Task.tc_State = TS_REMOVED;
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
        myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemWaitList);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct TaskPPC* myFindTaskPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") STRPTR name)
{
    struct DebugArgs args;
    args.db_Function = 6 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)name;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	if(!(name))
	{
		return PowerPCBase->pp_ThisPPCProc;
	}

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

	struct TaskPPC* fndTask;
	struct TaskPtr* taskPtr = (struct TaskPtr*)myFindNamePPC(PowerPCBase,
                              (struct List*)&PowerPCBase->pp_AllTasks, name);
	if (taskPtr)
	{
		fndTask = taskPtr->tptr_Task;
	}
	else
	{
		fndTask = NULL;
	}
	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);

    args.db_Arg[0] = (ULONG)fndTask;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return fndTask;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myInitSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 7 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	LONG result = 0;
	APTR reserved;

	myNewListPPC(PowerPCBase, (struct List*)&SemaphorePPC->ssppc_SS.ss_WaitQueue);
	SemaphorePPC->ssppc_SS.ss_Owner = 0;
	SemaphorePPC->ssppc_SS.ss_NestCount = 0;
	SemaphorePPC->ssppc_SS.ss_QueueCount = -1;

	if (reserved = AllocVec68K(PowerPCBase, 32, MEMF_PUBLIC|MEMF_CLEAR|MEMF_PPC)) //sizeof?
	{
		SemaphorePPC->ssppc_reserved = reserved;
		result = -1;
	}

    args.db_Arg[0] = result;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return result;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myFreeSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 8 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	if (SemaphorePPC)
	{
		if (SemaphorePPC->ssppc_reserved)
        {
        FreeVec68K(PowerPCBase, SemaphorePPC->ssppc_reserved);
        SemaphorePPC->ssppc_reserved = NULL;
        }
	}
	
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myAddSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 9 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);
	
    LONG result = 0;

	if (result = myInitSemaphorePPC(PowerPCBase, SemaphorePPC))
	{
		myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);

		myEnqueuePPC(PowerPCBase, (struct List*)&PowerPCBase->pp_Semaphores, (struct Node*)SemaphorePPC);

		myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);

	}

    args.db_Arg[0] = result;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return result;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myRemSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 10 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);

	myRemovePPC(PowerPCBase, (struct Node*)SemaphorePPC);

	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);

	myFreeSemaphorePPC(PowerPCBase, SemaphorePPC);
	
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myObtainSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 11 | (1<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

	SemaphorePPC->ssppc_SS.ss_QueueCount += 1;

	struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

	if ((SemaphorePPC->ssppc_SS.ss_QueueCount) && ((SemaphorePPC->ssppc_SS.ss_Owner != (struct Task*)myTask)))
	{
        struct SemWait myWait;
		myWait.sw_Task = myTask;
		myTask->tp_Task.tc_SigRecvd &= ~SIGF_SINGLE;

		myAddTailPPC(PowerPCBase, (struct List*)&SemaphorePPC->ssppc_SS.ss_WaitQueue, (struct Node*)&myWait);

		FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

		myWaitPPC(PowerPCBase, SIGF_SINGLE);
	}
	else
	{
		SemaphorePPC->ssppc_SS.ss_Owner = (struct Task*)myTask;
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
        SemaphorePPC->ssppc_SS.ss_NestCount += 1;
	}

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myAttemptSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 12 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	LONG result = 0;

	while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

	struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

	if ((SemaphorePPC->ssppc_SS.ss_QueueCount + 1) && (!(SemaphorePPC->ssppc_SS.ss_Owner == (struct Task*)myTask)))
	{
		result = ATTEMPT_FAILURE;
	}
	else
	{
		SemaphorePPC->ssppc_SS.ss_Owner = (struct Task*)myTask;
		SemaphorePPC->ssppc_SS.ss_QueueCount += 1;
		SemaphorePPC->ssppc_SS.ss_NestCount += 1;
		result = ATTEMPT_SUCCESS;
	}

	FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

    args.db_Arg[0] = result;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return result;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myReleaseSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 13 | (1<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    SemaphorePPC->ssppc_SS.ss_NestCount -= 1;

    if (SemaphorePPC->ssppc_SS.ss_NestCount < 0)
    {
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
        HaltError(ERR_ESEM);
    }

    if (SemaphorePPC->ssppc_SS.ss_NestCount > 0)
    {
        SemaphorePPC->ssppc_SS.ss_QueueCount -= 1;
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
    }
    else
    {
        /*
         * THE HAND-OFF IS DECIDED WHILE pp_Mutex IS HELD -- R1, a deliberate
         * divergence from WarpOS and 17.13b (FIXES.md "CODE REVIEW OF lib/").
         * Both free their lock before the hand-off writes the new owner --
         * PERMIT at powerpc.p:10079, the owner at :10097; AtomicDone in
         * 17.13b's .NotLast -- and both ObtainSemaphoreSharedPPCs grant at
         * once when the owner is 0 (powerpc.p:13756-13760). A task switch in
         * that window gave a shared caller the semaphore while an exclusive
         * waiter was being made its owner: two holders. Here every grant --
         * the waiters taken off the queue, the owner, the nest count -- is
         * made before pp_Mutex is freed, and only the wake-ups come after,
         * because SignalPPC and ReplyMsgPPC take pp_Mutex themselves.
         *
         * Who is woken: an exclusive task by its pointer, wakeTask -- its
         * wait node is not written; nothing reads it after the grant (until
         * T4, a granted TrySemaphorePPC waiter whose time ran out took that
         * node off the queue a second time) -- and
         * everything else, shared waiters and messages, chained through
         * their own nodes, which nobody reads after the wake-up. Each link
         * is read before its node is woken (R5). The grants, the order of
         * the wake-ups and the walks are the ones the code made before.
         */
        struct TaskPPC* wakeTask = NULL;
        struct SemWait* wakeHead = NULL;
        struct SemWait* wakeTail = NULL;
        ULONG           handOff  = 0;

#define SEMWAKE(w)  { (w)->sw_Node.mln_Succ = NULL;                                  \
                      if (wakeTail) wakeTail->sw_Node.mln_Succ = (struct MinNode*)(w); \
                      else wakeHead = (w);                                           \
                      wakeTail = (w); }

        SemaphorePPC->ssppc_SS.ss_Owner = 0;
        SemaphorePPC->ssppc_SS.ss_QueueCount -= 1;

        if (SemaphorePPC->ssppc_SS.ss_QueueCount != -1)
        {
            handOff = 1;
            SemaphorePPC->ssppc_lock = 1;   //WarpOS's flag for TrySemaphorePPC (powerpc.p:10078, :10185), kept; since T4 TrySemaphorePPC reads ss_Owner and nothing in lib/ reads the flag
            struct SemWait* myWait = (struct SemWait*)myRemHeadPPC(PowerPCBase, (struct List*)&SemaphorePPC->ssppc_SS.ss_WaitQueue);

            if (myWait)
            {
                struct TaskPPC* currTask = myWait->sw_Task;
                struct TaskPPC* sigTask = (struct TaskPPC*)((ULONG)currTask & ~SM_SHARED);
                if (!(SM_SHARED & (ULONG)currTask)) //not shared
                {
                    if (sigTask)
                    {
                        SemaphorePPC->ssppc_SS.ss_Owner = (struct Task*)sigTask;
                        SemaphorePPC->ssppc_SS.ss_NestCount += 1;
                        wakeTask = sigTask;
                    }
                    else
                    {
                        SemaphorePPC->ssppc_SS.ss_Owner = (struct Task*)myWait->sw_Semaphore;
                        myWait->sw_Semaphore = SemaphorePPC;
                        SemaphorePPC->ssppc_SS.ss_NestCount += 1;
                        SEMWAKE(myWait);

                        myWait = (struct SemWait*)SemaphorePPC->ssppc_SS.ss_WaitQueue.mlh_Head;
                        struct SemWait* nxtWait;

                        while (nxtWait = (struct SemWait*)myWait->sw_Node.mln_Succ)
                        {
                            if (!(myWait->sw_Task))
                            {
                                if (!(myWait->sw_Semaphore))
                                {
                                    struct MinNode* predWait = myWait->sw_Node.mln_Pred;
                                    predWait->mln_Succ = (struct MinNode*)nxtWait;
                                    nxtWait->sw_Node.mln_Pred = predWait;

                                    SemaphorePPC->ssppc_SS.ss_NestCount += 2;
                                    myWait->sw_Semaphore = SemaphorePPC;
                                    SEMWAKE(myWait);
                                }
                            }
                            else if (myWait->sw_Task == (struct TaskPPC*)SemaphorePPC->ssppc_SS.ss_Owner)
                            {
                                struct MinNode* predWait = myWait->sw_Node.mln_Pred;
                                predWait->mln_Succ = (struct MinNode*)nxtWait;
                                nxtWait->sw_Node.mln_Pred = predWait;

                                SemaphorePPC->ssppc_SS.ss_NestCount += 1;
                                wakeTask = myWait->sw_Task;
                                break;
                            }
                            myWait = nxtWait;
                        }
                    }
                }
                else //shared
                {
                    //the walk starts at the queue's head, read now, as WarpOS .wakeshared (powerpc.p:10152): R5
                    struct SemWait* nxtWait = (struct SemWait*)SemaphorePPC->ssppc_SS.ss_WaitQueue.mlh_Head;

                    while (myWait)
                    {
                        SemaphorePPC->ssppc_SS.ss_NestCount += 1;
                        if (!((ULONG)myWait->sw_Task & ~SM_SHARED))     //a message: granted and its waiter word cleared
                        {
                            myWait->sw_Semaphore = SemaphorePPC;
                            myWait->sw_Task = 0;
                        }
                        SEMWAKE(myWait);

                        myWait = NULL;
                        while (nxtWait)
                        {
                            struct SemWait* candWait = nxtWait;
                            nxtWait = (struct SemWait*)candWait->sw_Node.mln_Succ;
                            //no successor: the list's tail marker, whose "sw_Task" is ss_MultipleLink -- stop, as 17.13b's .link31 (FIXES.md "CODE REVIEW OF lib/", #2)
                            if (!(nxtWait))
                            {
                                break;
                            }
                            if ((ULONG)candWait->sw_Task & SM_SHARED)
                            {
                                myRemovePPC(PowerPCBase, (struct Node*)candWait);
                                myWait = candWait;
                                break;
                            }
                        }
                    }
                }
            }
        }
#undef SEMWAKE

        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

        //the wake-ups, after the lock; each link read before its node is woken (R5)
        while (wakeHead)
        {
            struct SemWait* doneWait = wakeHead;
            struct TaskPPC* doneTask = (struct TaskPPC*)((ULONG)doneWait->sw_Task & ~SM_SHARED);
            wakeHead = (struct SemWait*)doneWait->sw_Node.mln_Succ;

            if (doneTask)
            {
                mySignalPPC(PowerPCBase, doneTask, SIGF_SINGLE);
            }
            else
            {
                myReplyMsgPPC(PowerPCBase, (struct Message*)doneWait);
            }
        }
        if (wakeTask)
        {
            mySignalPPC(PowerPCBase, wakeTask, SIGF_SINGLE);
        }
        if (handOff)
        {
            SemaphorePPC->ssppc_lock = 0;
        }
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct SignalSemaphorePPC* myFindSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") STRPTR name)
{
    struct DebugArgs args;
    args.db_Function = 14 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)name;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);

	struct SignalSemaphorePPC* mySem = (struct SignalSemaphorePPC*)myFindNamePPC(PowerPCBase, (struct List*)&PowerPCBase->pp_Semaphores, name);

	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);

    args.db_Arg[0] = (ULONG)mySem;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return mySem;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myInsertPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list, __reg("r5") struct Node* node, __reg("r6") struct Node* pred)
{
	if (pred == 0)
	{
		myAddHeadPPC(PowerPCBase, list, node);
        return;
	}

	struct Node* succ = pred->ln_Succ;

	if (succ)
	{
		node->ln_Succ = succ;
		node->ln_Pred = pred;
		succ->ln_Pred = node;
		pred->ln_Succ = node;
		return;
	}

	struct Node* predPred = pred->ln_Pred;

	node->ln_Succ = pred;
	pred->ln_Pred = node;
	predPred->ln_Succ = node;

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myAddHeadPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list, __reg("r5") struct Node* node)
{
	struct Node* succ = list->lh_Head;
	list->lh_Head = node;
	node->ln_Succ = succ;
	node->ln_Pred = (struct Node*)list;
	succ->ln_Pred = node;

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myAddTailPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list, __reg("r5") struct Node* node)
{
	struct Node* tailpred = list->lh_TailPred;
	list->lh_TailPred = node;
	node->ln_Succ = (struct Node*)&list->lh_Tail;
	node->ln_Pred = tailpred;
	tailpred->ln_Succ = node;

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myRemovePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct Node* node)
{
	struct Node* succ = node->ln_Succ;
	struct Node* pred = node->ln_Pred;
	succ->ln_Pred = pred;
	pred->ln_Succ = succ;

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct Node* myRemHeadPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list)
{
	struct Node* firstNode = list->lh_Head;
	struct Node* succ = firstNode->ln_Succ;
	if (succ)
	{
		list->lh_Head = succ;
		succ->ln_Pred = (struct Node*)list;
		return firstNode;
	}
	return 0;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct Node* myRemTailPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list)
{
	struct Node* tailpred = list->lh_TailPred;
	struct Node* pred = tailpred->ln_Pred;
	if (pred)
	{
		list->lh_TailPred = pred;
		pred->ln_Succ = (struct Node*)&list->lh_Tail;
		return tailpred;
	}
	return 0;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myEnqueuePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list, __reg("r5") struct Node* node)
{
    LONG myPrio = (LONG)node->ln_Pri;
	struct Node* nextNode = list->lh_Head;

	while(nextNode->ln_Succ)
	{
		LONG cmpPrio = (LONG)nextNode->ln_Pri;

		if (myPrio > cmpPrio)
		{
			break;
		}
		nextNode = nextNode->ln_Succ;
	}
	struct Node* pred = nextNode->ln_Pred;
	nextNode->ln_Pred = node;
	node->ln_Succ = nextNode;
	node->ln_Pred = pred;
	pred->ln_Succ = node;

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct Node* myFindNamePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list, __reg("r5") STRPTR name)
{
    struct Node* nxtNode;
	struct Node* curNode;

	curNode = list->lh_Head;
	while (nxtNode = curNode->ln_Succ)
	{
		STRPTR nodeName = curNode->ln_Name;
		ULONG offset = 0;

		while (nodeName[offset] == name[offset])
		{
			if (!(nodeName[offset]))
			{
				return curNode;
			}
            offset++;
		}
	    curNode = nxtNode;
	}
	return NULL;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct TagItem* myFindTagItemPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG tagvalue, __reg("r5") struct TagItem* taglist)
{
	struct TagItem* myTagItem = NULL;

    struct TagItemPtr tagPtr;
    tagPtr.tip_TagItem = taglist;

	while (myTagItem = myNextTagItemPPC(PowerPCBase, (struct TagItem**)&tagPtr))
	{
        if (myTagItem->ti_Tag == tagvalue)
		{
			break;
		}
	}

	return myTagItem;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG myGetTagDataPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG tagvalue, __reg("r5") ULONG tagdefault, __reg("r6") struct TagItem* taglist)
{
    ULONG result;
    struct TagItem* myTagItem;

    if (myTagItem = myFindTagItemPPC(PowerPCBase, tagvalue, taglist))
    {
        result = myTagItem->ti_Data;
    }
    else
    {
        result = tagdefault;
    }

    return result;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct TagItem* myNextTagItemPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TagItem** tagitem)
{
	struct TagItemPtr* tagitemptr = (struct TagItemPtr*)tagitem;
	struct TagItem* myTagItem;
    LONG myTag;

	if (myTagItem = tagitemptr->tip_TagItem)
    {
	    while (myTag = (LONG)myTagItem->ti_Tag)
	    {
            if ((myTag < 0) || (myTag > 3))
            {
                tagitemptr->tip_TagItem = myTagItem + 1;
                return myTagItem;
            }
            else if (myTag == 2)
            {
                if (!(myTagItem = (struct TagItem*)myTagItem->ti_Data))
                {
                    break;
                }
            }
            else if (myTag == 1)
            {
                myTagItem += 1;
            }
            else    //my Tag == 3
            {
                 ULONG skipValue = 1 + myTagItem->ti_Data;
                 myTagItem += skipValue;
            }
        }
        myTagItem = NULL;
        tagitemptr->tip_TagItem = myTagItem;
    }

    return myTagItem;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myAllocSignalPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") LONG signum)
{
    struct DebugArgs args;
    args.db_Function = 15 | (1<<8) | (1<<16);
    args.db_Arg[0] = signum;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	LONG resAlloc, testAlloc;

	ULONG myAlloc = PowerPCBase->pp_ThisPPCProc->tp_Task.tc_SigAlloc;

	if (!((BYTE)signum == -1))
	{
		ULONG testAlloc = 1 << signum;
		resAlloc = signum;
		if (testAlloc & myAlloc)
		{
			resAlloc = -1;
		}
	}
	else
	{
		resAlloc = 0x1f;
        do
        {
			testAlloc = 1 << resAlloc;
			if (!(testAlloc & myAlloc))
			{
				break;
			}
        resAlloc--;
		} while(resAlloc > -1);
	}

    if (resAlloc > -1)
    {
        PowerPCBase->pp_ThisPPCProc->tp_Task.tc_SigAlloc |= 1 << resAlloc;

        while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
        PowerPCBase->pp_ThisPPCProc->tp_Task.tc_SigRecvd &= ~(1 << resAlloc);
        PowerPCBase->pp_ThisPPCProc->tp_Task.tc_SigWait &= ~(1 << resAlloc);
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
    }

    args.db_Arg[0] = resAlloc;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return resAlloc;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myFreeSignalPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") LONG signum)
{
    struct DebugArgs args;
    args.db_Function = 16 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)signum;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    BYTE testSig = (BYTE)signum;
	if (!(testSig == -1))
	{
		PowerPCBase->pp_ThisPPCProc->tp_Task.tc_SigAlloc &= ~(1<<signum);
		struct TaskLink* taskLink = &PowerPCBase->pp_ThisPPCProc->tp_Link;
		taskLink->tl_Sig &= ~(1<<signum);
	}
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG mySetSignalPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG signals, __reg("r5") ULONG mask)
{
    struct DebugArgs args;
    args.db_Function = 17 | (2<<8) | (1<<16);
    args.db_Arg[0] = signals;
    args.db_Arg[1] = mask;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;
	while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

	ULONG oldSig = myTask->tp_Task.tc_SigRecvd;
	myTask->tp_Task.tc_SigRecvd = (signals & mask) | (oldSig & ~mask);  //X1: oldSig is RETURNED, so it must not change. "oldSig &= ~mask" returned the old signals with the changed bits cleared, and "SetSignalPPC(0, mask) & mask" -- the usual Ctrl-C poll -- was always 0. WarpOS returns the untouched old value ("andc r7,r31,r5" on a copy, then r31, powerpc.p:10673-10700). REVIEW-LIB.md X1

	FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

	CheckExcSignal(PowerPCBase, myTask, 0);

    args.db_Arg[0] = oldSig;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return oldSig;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID mySignalPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TaskPPC* task, __reg("r5") ULONG signals)
{
    struct DebugArgs args;
    args.db_Function = 18 | (2<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = signals;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    /*
     * EVERYTHING THAT IS NOT A PPC TASK GOES TO THE 68K SIDE, and this was
     * "case NT_TASK:" -- FIXES.md item 19 gap 1.
     *
     * With only NT_TASK and NT_PPCTASK handled and no default, a target of any
     * other type fell out of the switch and the signal was dropped in
     * SILENCE. NT_PROCESS is 13 (exec/nodes.h:56) and **most 68k programs are
     * Processes, not Tasks** -- anything started from the shell or Workbench
     * -- so SignalPPC to one of those did nothing at all.
     *
     * The original is structured as the negation and therefore has no such
     * hole: powerpc.p:10730 tests "ln_Type != NT_PPCTASK" and sends everything
     * else down one path. This reproduces that shape.
     *
     * A default cannot misroute a PPC task: both creation paths set
     * NT_PPCTASK, at functionsppc.c:360 and kernel.c:1241, so the only way to
     * reach here is a genuinely non-PPC node.
     *
     * WHAT IT DOES WITH IT IS OURS, NOT THE ORIGINAL'S, deliberately. The
     * original translates a 68k task to its PPC partner
     * (FindPPCCoreEntryM -> CORE_PROCPPC) and signals that, because the task
     * it is handed is expected to be a mirror. This library has no such
     * lookup yet -- that is item 19 gap 2, and pp_MirrorList already holds the
     * data for it -- so the signal is DELIVERED to the 68k task through
     * mySignal68K, which is what this switch already did for NT_TASK and is
     * the only sensible action for a 68k task that is not a mirror.
     */
    switch (task->tp_Task.tc_Node.ln_Type)
    {
        default:
        {
            mySignal68K(PowerPCBase, (struct Task*)task, signals);
            break;
        }
        case NT_PPCTASK:
        {
            if (PowerPCBase->pp_ExceptionMode)
            {
                /*
                 * FROM AN EXCEPTION HANDLER, as WarpOS (powerpc.p:10744-10780):
                 * the task is not touched -- the pp_Mutex below and
                 * CheckExcSignal's wait would spin for ever in a handler that
                 * interrupted their holder. The signals go into the task's own
                 * tp_Link, queued on pp_TaskLinks for kernel.c's
                 * HandleDeferred, and a waiting task gets the decrementer soon,
                 * WarpOS's SetTimer(0). A second signal before the kernel has
                 * run is ORed in; WarpOS stores only the new one there
                 * (:10777-10779, "or r0,r0,r5" then "stw r5"), not copied. A
                 * task already being removed is not queued. FIXES.md "#1 PART B
                 * DESIGNED", 1 Nov 2026.
                 */
                if (task->tp_Task.tc_State != TS_REMOVED)
                {
                    struct TaskLink* link = &task->tp_Link;

                    if (!(link->tl_Used))
                    {
                        link->tl_Task = (APTR)task;
                        link->tl_Sig  = signals;
                        link->tl_Used = -1;
                        myAddTailPPC(PowerPCBase, (struct List*)&PowerPCBase->pp_TaskLinks, (struct Node*)link);
                    }
                    else
                    {
                        link->tl_Sig |= signals;
                    }

                    if (task->tp_Task.tc_State == TS_WAIT)
                    {
                        setDEC(30);
                    }
                }
                return;
            }

            signals = CheckExcSignal(PowerPCBase, task, signals);
            while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

            task->tp_Task.tc_SigRecvd |= signals;

            switch (task->tp_Task.tc_State)
            {
                case TS_WAIT:
                {
                    if (task->tp_Task.tc_SigWait & signals)
                    {
                        myRemovePPC(PowerPCBase, (struct Node*)task);
                        task->tp_Task.tc_State = TS_READY;
                        InsertOnPri(PowerPCBase, (struct List*)&PowerPCBase->pp_ReadyTasks, task);
                        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
                        struct TaskPPC* topTask = (struct TaskPPC*)PowerPCBase->pp_ReadyTasks.mlh_Head;

                        if (topTask == task)
                        {
                            PowerPCBase->pp_FlagReschedule = -1;
                            CauseDECInterrupt(PowerPCBase);
                        }
                        return;
                    }
                    break;
                }
                case TS_CHANGING:
                {
                    task->tp_Task.tc_State = TS_RUN;
                    break;
                }
            }
            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
        }
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG myWaitPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG signals)
{
    struct DebugArgs args;
    args.db_Function = 19 | (1<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = signals;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

	while(!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

	myTask->tp_Task.tc_SigWait = signals;
	ULONG maskSig;

	while(!(maskSig = myTask->tp_Task.tc_SigRecvd & signals))
	{
		FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
		myTask->tp_Task.tc_State = TS_CHANGING;

		CauseDECInterrupt(PowerPCBase);

		while((volatile)myTask->tp_Task.tc_State != TS_RUN);

		while(!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

		signals = myTask->tp_Task.tc_SigWait;
	}

	myTask->tp_Task.tc_SigRecvd ^= maskSig;

	FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

    args.db_Arg[0] = maskSig;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return maskSig;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG mySetTaskPriPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TaskPPC* task, __reg("r5") LONG pri)
{
    struct DebugArgs args;
    args.db_Function = 20 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)task;
    args.db_Arg[1] = pri;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	while(!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

	LONG oldPri = task->tp_Task.tc_Node.ln_Pri;
	task->tp_Task.tc_Node.ln_Pri = (BYTE)pri;

	if (PowerPCBase->pp_ThisPPCProc != task)
	{
		if((task->tp_Task.tc_State != TS_REMOVED) && (task->tp_Task.tc_State != TS_WAIT))
		{
			myRemovePPC(PowerPCBase, (struct Node*)task);
			task->tp_Task.tc_State = TS_READY;
			InsertOnPri(PowerPCBase, (struct List*)&PowerPCBase->pp_ReadyTasks, task);
			if(task == (struct TaskPPC*)PowerPCBase->pp_ReadyTasks.mlh_Head)
			{
				FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
				CauseDECInterrupt(PowerPCBase);
			}
			else
			{
				FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
			}
		}
		else
		{
			FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
		}
	}
	else
	{
		FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
		CauseDECInterrupt(PowerPCBase);
	}

    args.db_Arg[0] = oldPri;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return oldPri;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID mySignal68K(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct Task* task, __reg("r5") ULONG signals)
{
    struct DebugArgs args;
    args.db_Function = 21 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)task;
    args.db_Arg[1] = signals;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	struct MsgFrame* myFrame = CreateMsgFramePPC(PowerPCBase);

	myFrame->mf_Identifier = ID_SIG;
	myFrame->mf_Signals = signals;
	myFrame->mf_Arg[0] = (ULONG)task;

	SendMsgFramePPC(PowerPCBase, myFrame);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID mySetCache(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG flags, __reg("r5") APTR start, __reg("r6") ULONG length)
{
    struct DebugArgs args;
    args.db_Function = 22 | (3<<8) | (1<<16);
    args.db_Arg[0] = flags;
    args.db_Arg[1] = (ULONG)start;
    args.db_Arg[2] = length;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct SysCall sc;
    sc.sc_Function = SC_SETCACHE;
    sc.sc_Arg[0] = flags;
    sc.sc_Arg[1] = (ULONG)start;
    sc.sc_Arg[2] = length;

    CallKernel(PowerPCBase, &sc);   //in a handler, the kernel directly, as WarpOS (powerpc.p:11092-11103); REVIEW-LIB.md K1

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION APTR mySetExcHandler(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TagItem* taglist)
{
    struct DebugArgs args;
    args.db_Function = 23 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)taglist;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct ExcInfo* excInfo;
    ULONG value;
    APTR excdata;

    if (!(excInfo = AllocVec68K(PowerPCBase, sizeof(struct ExcInfo), MEMF_PUBLIC | MEMF_CLEAR)))
    {
        printDebug(PowerPCBase, (struct DebugArgs*)&args);
        return NULL;
    }

    if (!(value = myGetTagDataPPC(PowerPCBase, EXCATTR_CODE, 0, taglist)))
    {
        FreeVec68K(PowerPCBase, excInfo);
        printDebug(PowerPCBase, (struct DebugArgs*)&args);
        return NULL;
    }

    excInfo->ei_ExcData.ed_Code = (APTR)value;
    value = myGetTagDataPPC(PowerPCBase, EXCATTR_FLAGS, 0, taglist);

    if (!(value & (EXCF_SMALLCONTEXT | EXCF_LARGECONTEXT)) || !(value & (EXCF_GLOBAL | EXCF_LOCAL)))
    {
        FreeVec68K(PowerPCBase, excInfo);
        printDebug(PowerPCBase, (struct DebugArgs*)&args);
        return NULL;
    }           

    struct TaskPPC* value2 = (struct TaskPPC*)myGetTagDataPPC(PowerPCBase, EXCATTR_TASK, 0, taglist);

    if ((value & EXCF_GLOBAL) || ((value & EXCF_LOCAL) && (value2)))
    {
        excInfo->ei_ExcData.ed_Task = value2;
    }
    else
    {
        excInfo->ei_ExcData.ed_Task = PowerPCBase->pp_ThisPPCProc;
    }

    excInfo->ei_ExcData.ed_Flags = value;
    excInfo->ei_ExcData.ed_Data = myGetTagDataPPC(PowerPCBase, EXCATTR_DATA, 0, taglist);
    excInfo->ei_ExcData.ed_Node.ln_Name = (APTR)myGetTagDataPPC(PowerPCBase, EXCATTR_NAME, 0, taglist);
    excInfo->ei_ExcData.ed_Node.ln_Pri = (BYTE)myGetTagDataPPC(PowerPCBase, EXCATTR_PRI, 0, taglist);
    excInfo->ei_ExcData.ed_Node.ln_Type = NT_INTERRUPT;
    excInfo->ei_ExcData.ed_TimeBasePtr = (APTR)myGetTagDataPPC(PowerPCBase, EXCATTR_TIMEDREMOVAL, 0, taglist);
    value = myGetTagDataPPC(PowerPCBase, EXCATTR_EXCID, 0, taglist);
    excInfo->ei_ExcData.ed_ExcID = value;

    if (!(value))
    {
        FreeVec68K(PowerPCBase, excInfo);
        printDebug(PowerPCBase, (struct DebugArgs*)&args);
        return NULL;
    }

    ULONG flag = 0;
    while (1)
    {
        struct ExcData* newData;

        if (value & EXCF_MCHECK)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_MachineCheck, EXCF_MCHECK);
        }
        if (value & EXCF_DACCESS)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_DataAccess, EXCF_DACCESS);
        }
        if (value & EXCF_IACCESS)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_InstructionAccess, EXCF_IACCESS);
        }
        if (value & EXCF_INTERRUPT)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_Interrupt, EXCF_INTERRUPT);
        }
        if (value & EXCF_ALIGN)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_Alignment, EXCF_ALIGN);
        }
        if (value & EXCF_PROGRAM)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_Program, EXCF_PROGRAM);
        }
        if (value & EXCF_FPUN)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_FPUnavailable, EXCF_FPUN);
        }
        if (value & EXCF_DECREMENTER)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_Decrementer, EXCF_DECREMENTER);
        }
        if (value & EXCF_SC)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_SystemCall, EXCF_SC);
        }
        if (value & EXCF_TRACE)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_Trace, EXCF_TRACE);
        }
        if (value & EXCF_PERFMON)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_PerfMon, EXCF_PERFMON);
        }
        if (value & EXCF_IABR)
        {
            if (!(newData = AllocVec68K(PowerPCBase, sizeof(struct ExcData), MEMF_PUBLIC)))
            {
                flag = 1;
                break;
            }
            AddExcList(PowerPCBase, excInfo, newData, (ULONG*)&excInfo->ei_IABR, EXCF_IABR);
        }
        break;
    }

    if (flag)
    {
        FreeAllExcMem(PowerPCBase, excInfo);    //E1: nothing is linked yet, so freeing is safe
        FreeVec68K(PowerPCBase, excInfo);
        printDebug(PowerPCBase, (struct DebugArgs*)&args);
        return NULL;
    }

    LinkExcList(PowerPCBase, excInfo);          //E1: every entry at once, after the last allocation (supportppc.c)

    CauseDECInterrupt(PowerPCBase);

    /*
     * E2: NO ENTRY, NO WAIT. An EXCATTR_EXCID with none of the twelve bits
     * above (EXCF_ALTIVECUNAV alone, or an EXCB_ number used as a mask)
     * makes no entry, so ed_LastExc stays NULL, and the wait read through
     * it: the task was killed, or under NoZeroGuard it spun for ever.
     * MATCHES WarpOS (powerpc.p:11432-11433) and 17.13b
     * (ppcfunctions.p:9041-9042), which skip the wait and return the
     * handle. RemExcHandler below frees such a handle at once.
     */
    if (excInfo->ei_ExcData.ed_LastExc)
    {
        while (!((volatile)excInfo->ei_ExcData.ed_LastExc->ed_Flags & EXCF_ACTIVE));
    }

    args.db_Arg[0] = (ULONG)excInfo;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return excInfo;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myRemExcHandler(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") APTR xlock)
{
    struct DebugArgs args;
    args.db_Function = 24 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)xlock;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    if (xlock)
    {
        struct ExcInfo* myInfo = xlock;

        /*
         * E2: A HANDLE WITH NO ENTRY WAS NEVER LINKED (SetExcHandler above),
         * so it is freed at once. Queued for the kernel, its NULL ed_LastExc
         * would be written through by the removal (kernel.c:379-380) and
         * read by the wait below. BEYOND BOTH ORIGINALS: their RemExcHandler
         * waits on that entry with no test (powerpc.p:11487-11491;
         * ppcfunctions.p:8557-8560), and 17.13b's kernel writes through it as
         * kernel.c does (setupppc.p:4450-4454); WarpOS's kernel tests each
         * entry (powerpc.p:4313-4318).
         */
        if (myInfo->ei_ExcData.ed_LastExc)
        {
            while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

            myAddHeadPPC(PowerPCBase,(struct List*)&PowerPCBase->pp_RemovedExc, (struct Node*)xlock);

            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

            CauseDECInterrupt(PowerPCBase);

            while ((volatile)myInfo->ei_ExcData.ed_LastExc->ed_Flags & EXCF_ACTIVE);

            FreeAllExcMem(PowerPCBase, myInfo);
        }

        FreeVec68K(PowerPCBase, xlock);
    }

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG mySuper(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    /*
     * RECOGNISED BY ADDRESS, as WarpOS (warp.p:803-807, :4588-4602) and
     * 17.13b (ppcfunctions.p:2988-2995, setupppc.p:6302-6317) do: the mfpvr
     * at SuperLocation traps in user mode, and kernel.c's VEC_PROGRAM arm
     * takes only that address for Super(). It used to be SUPERKEY in r4,
     * which any program exception carrying that value matched. Returns 0
     * when this switched to supervisor, -1 when already there. FIXES.md
     * "ITEM 3, Super()", 31 Oct 2026.
     */
    return SuperTrap();
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myUser(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG key)
{
    if (!(key))
    {
        ULONG msrbits = getMSR();
        msrbits |= PSL_PR;
        setMSR(msrbits);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG mySetHardware(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG flags, __reg("r5") APTR param)
{
    struct DebugArgs args;
    args.db_Function = 25 | (2<<8) | (1<<16);
    args.db_Arg[0] = flags;
    args.db_Arg[1] = (ULONG)param;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    ULONG status = HW_AVAILABLE;

    if (flags == HW_TRACEON) //case will result in a need for SDA_BASE
    {
        ULONG key = mySuper(PowerPCBase);
        setMSR(getMSR() | PSL_SE);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_TRACEOFF)
    {
        ULONG key = mySuper(PowerPCBase);
        setMSR(getMSR() & ~PSL_SE);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_BRANCHTRACEON)
    {
        ULONG key = mySuper(PowerPCBase);
        setMSR(getMSR() | PSL_BE);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_BRANCHTRACEOFF)
    {
        ULONG key = mySuper(PowerPCBase);
        setMSR(getMSR() & ~PSL_SE);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_FPEXCON)
    {
        ULONG key = mySuper(PowerPCBase);
        SetFPExc();
        setMSR(getMSR() | PSL_FE0 | PSL_FE1);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_FPEXCOFF)
    {
        ULONG key = mySuper(PowerPCBase);
        setMSR(getMSR() & ~(PSL_FE0 | PSL_FE1));
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_SETIBREAK)
    {
        ULONG address = (ULONG)param;
        address = (address & -4) | 3;
        ULONG key = mySuper(PowerPCBase);
        setIABR(address);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_CLEARIBREAK)
    {
        ULONG key = mySuper(PowerPCBase);
        setIABR(0);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_SETDBREAK)
    {
        ULONG address = (ULONG)param;
        address = (address & -8) | 7;
        ULONG key = mySuper(PowerPCBase);
        setDABR(address);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_CLEARDBREAK)
    {
        ULONG key = mySuper(PowerPCBase);
        setDABR(0);
        myUser(PowerPCBase, key);
    }
    else if (flags == HW_SETDEBUGMODE) //private
    {
        ULONG level = (ULONG)param;
        PowerPCBase->pp_DebugLevel = level;
    }
    else if (flags == HW_PPCSTATE) //private
    {
        status = PPCSTATEF_POWERSAVE;
        if (PowerPCBase->pp_WaitingTasks.mlh_Head->mln_Succ)
        {
            status = PPCSTATEF_APPACTIVE;
        }
        if (PowerPCBase->pp_ReadyTasks.mlh_Head->mln_Succ)
        {
            status = PPCSTATEF_APPACTIVE;
        }
    }
    else
    {
        status = HW_NOTAVAILABLE;
    }

    args.db_Arg[0] = status;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return status;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myModifyFPExc(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG fpflags)
{
    struct DebugArgs args;
    args.db_Function = 26 | (1<<8) | (1<<16);
    args.db_Arg[0] = fpflags;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    if (fpflags & FPF_EN_OVERFLOW)
    {
        FPE_Enable(1);
    }
    if (fpflags & FPF_DIS_OVERFLOW)
    {
        FPE_Disable(1);
    }
    if (fpflags & FPF_EN_UNDERFLOW)
    {
        FPE_Enable(2);
    }
    if (fpflags & FPF_DIS_UNDERFLOW)
    {
        FPE_Disable(2);
    }
    if (fpflags & FPF_EN_ZERODIVIDE)
    {
        FPE_Enable(4);
    }
    if (fpflags & FPF_DIS_ZERODIVIDE)
    {
        FPE_Disable(4);
    }
    if (fpflags & FPF_EN_INEXACT)
    {
        FPE_Enable(8);
    }
    if (fpflags & FPF_DIS_INEXACT)
    {
        FPE_Disable(8);
    }
    if (fpflags & FPF_EN_INVALID)
    {
        FPE_Enable(16);
    }
    if (fpflags & FPF_DIS_INVALID)
    {
        FPE_Disable(16);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG myWaitTime(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG signals, __reg("r5") ULONG time)
{
    struct DebugArgs args;
    args.db_Function = 27 | (2<<8) | (1<<16) | (1<<17);
    args.db_Arg[0] = signals;
    args.db_Arg[1] = time;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct UInt64 myInt64;
    struct WaitTime myWait;
    ULONG value1, value2, counter, tbu, tbl, recvdsig;

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemWaitList);

    /*
     * A PLAIN FLAG, AS THE ORIGINAL HAS IT: powerpc.p:11739 "stbi -1" here,
     * :11779 "stbi 0" after the ADDHEAD, :11787 "stbi -1" after WaitPPC
     * returns, :11802 "stbi 0" after PERMIT, read by the sweep at :4098.
     *
     * A NESTING COUNT WAS TRIED ON 22 OCT AND REVERTED. The argument for it
     * was that with two tasks inside WaitTime one task's store of 0 clears
     * the other's guard -- which is true, and still true. But the original
     * is identical, so it is a divergence rather than a repair.
     *
     * IT WAS REVERTED ON A FALSE NEGATIVE AND IS BACK. I reported that it
     * "changed nothing on hardware" -- but the run it was tested against was
     * tests/subdemo, not ScummVM: I had just shipped the build and asked for
     * subdemo, and the symptom reported was the whole Workbench and the mouse
     * dying, which is subdemo's signature, not ScummVM's (that locks the
     * screen and leaves the mouse alive). So the count was never tested
     * against the real hang, and I reverted it on the strength of a different
     * program's failure.
     */
    PowerPCBase->pp_FlagWait++;

    value2 = 60000000;

    mulInt64((struct UInt64*)&myInt64, PowerPCBase->pp_BusClock, value2);
    value1 = Calculator(myInt64.ui_High, myInt64.ui_Low, 4000000);            //Magic

    counter = time / value2;
    value2 = counter * value2;
    value2 = time - value2;

    mulInt64((struct UInt64*)&myInt64, PowerPCBase->pp_BusClock, value2);
    value2 = Calculator(myInt64.ui_High, myInt64.ui_Low, 4000000);            //Magic

    while (1)
    {
        tbu = getTBU();
        tbl = getTBL();
        if (tbu == getTBU())
        {
            break;
        }
    }

    FinalCalc(counter, tbu, tbl, value1, value2, (struct WaitTime*)&myWait);  //Magic

    myWait.wt_Task = PowerPCBase->pp_ThisPPCProc;
    myWait.wt_Node.ln_Pri = 0;
    myWait.wt_Node.ln_Type = NT_UNKNOWN;
    myWait.wt_Node.ln_Name = PowerPCBase->pp_ThisPPCProc->tp_Task.tc_Node.ln_Name;

    myAddHeadPPC(PowerPCBase, (struct List*)&PowerPCBase->pp_WaitTime, (struct Node*)&myWait);

    PowerPCBase->pp_FlagWait--;

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemWaitList);

    recvdsig = myWaitPPC(PowerPCBase, signals | SIGF_WAIT);

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    /* inside the mutex: these two are not serialised by pp_SemWaitList the
     * way the pair above is, and a lost decrement would disable the sweep
     * permanently -- the very symptom being chased */
    PowerPCBase->pp_FlagWait++;

    if (!((PowerPCBase->pp_ThisPPCProc->tp_Task.tc_SigRecvd | recvdsig) & SIGF_WAIT))
    {
        myRemovePPC(PowerPCBase, (struct Node*)&myWait);
    }
    else
    {
        PowerPCBase->pp_ThisPPCProc->tp_Task.tc_SigRecvd &= ~SIGF_WAIT;
    }

    PowerPCBase->pp_FlagWait--;

    FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

    recvdsig &= ~SIGF_WAIT;

    args.db_Arg[0] = recvdsig;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return recvdsig;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct TaskPtr* myLockTaskList(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);
	return ((struct TaskPtr*)PowerPCBase->pp_AllTasks.mlh_Head);
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myUnLockTaskList(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID mySetExcMMU(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    /*
     * A NO-OP, AS IN 17.13b (ppcfunctions.p:491-508, its MMU lines
     * commented out) and 17.15. WarpOS runs exception handlers untranslated
     * and SetExcMMU (powerpc.p:8053-8158) gives one its BAT set with
     * translation on. Here every handler already runs translated under the
     * library's BATs, segment registers and SDR1, installed by the exception
     * entry (kernel.s:151-267); pp_ExceptionBATs, the set a port would load,
     * holds the same BATs (setupppc.c:1166-1174). It compiles to a bare blr:
     * every register is kept (WarpOS promises all but r0 and r3). FIXES.md
     * "ITEM 6, SetExcMMU / ClearExcMMU", 31 Oct 2026.
     */
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myClearExcMMU(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    /*
     * A NO-OP, AS IN 17.13b (ppcfunctions.p:516-534, its MMU lines
     * commented out) and 17.15. WarpOS's ClearExcMMU (powerpc.p:8172-8236)
     * switches translation OFF and restores the BATs SetExcMMU saved. Here a
     * handler's code, stack and frame are 68k-view addresses, EA = PA +
     * id_MemBase (kernel.s:71-79): with translation off its next
     * instruction could not be fetched. The handler keeps the library's MMU
     * until the exception exit installs the task's (kernel.s:444 on).
     * FIXES.md "ITEM 6, SetExcMMU / ClearExcMMU", 31 Oct 2026.
     */
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION BOOL myChangeStack(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG NewStackSize)
{
    struct DebugArgs args;
    args.db_Function = 28 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)NewStackSize;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    BOOL result;
    APTR newStack;
    ULONG src, dst, siz, sup;
    struct MemList* stackMem;

    if (NewStackSize < PowerPCBase->pp_ThisPPCProc->tp_StackSize)
    {
        return FALSE;
    }

    if (!(newStack = AllocVec68K(PowerPCBase, NewStackSize, MEMF_PUBLIC|MEMF_CLEAR|MEMF_PPC)))
    {
        return FALSE;
    }

    if (stackMem = AllocVec68K(PowerPCBase, sizeof(struct MemList), MEMF_PUBLIC|MEMF_CLEAR|MEMF_PPC))
    {
        struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;
        stackMem->ml_NumEntries = 1;
        stackMem->ml_ME[0].me_Un.meu_Addr = newStack;
        stackMem->ml_ME[0].me_Length = NewStackSize;

        myAddHeadPPC(PowerPCBase, (struct List*)&myTask->tp_Task.tc_MemEntry, (struct Node*)stackMem);

        src = (ULONG)myTask->tp_Task.tc_SPLower;
        sup = (ULONG)myTask->tp_Task.tc_SPUpper;
        siz = sup - src;
        dst = (ULONG)newStack + NewStackSize - siz;

        myCopyMemPPC(PowerPCBase, (APTR)src, (APTR)dst, siz);

        myTask->tp_Task.tc_SPLower = (APTR)dst;
        myTask->tp_Task.tc_SPUpper = (APTR)((ULONG)newStack + NewStackSize);
        myTask->tp_StackSize = NewStackSize;

        SwapStack(sup, ((ULONG)newStack + NewStackSize));

        result = TRUE;
    }
    else
    {
        FreeVec68K(PowerPCBase, stackMem);
        result = FALSE;
    }

    args.db_Arg[0] = result;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);
    return result;
}
/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myChangeMMU(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG mode)
{
    struct DebugArgs args;
    args.db_Function = 29 | (1<<8) | (1<<16);
    args.db_Arg[0] = mode;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    /*
     * A STUB ON PURPOSE, as in 17.13b (ppcfunctions.p:8165 "#STUB", the
     * early epilog at :8169-8178). WarpOS's ChangeMMU (powerpc.p:11965-11994)
     * switches a task to PPC_BAT, a set that maps the Amiga space with
     * EA == PA (powerpc.p:529-578); a PCI card has no such set, and the
     * page table already maps a task's memory. The draft below is never
     * reached, and its two arms are the wrong way round against both
     * originals. FIXES.md "ITEM 14C", 31 Oct 2026.
     */
    return; //stub

    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    switch (mode)
    {
        case CHMMU_STANDARD:
        {
            myTask->tp_Flags &= ~MMUF_BAT;
            GetBATs(PowerPCBase, myTask);
            break;
        }
        case CHMMU_BAT:
        {
            myTask->tp_Flags |= MMUF_BAT;
            StoreBATs(PowerPCBase, myTask);
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

PPCFUNCTION VOID myGetInfo(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TagItem* taglist)
{
    struct DebugArgs args;
    args.db_Function = 30 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)taglist;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	struct TagItem* myTagItem = NULL;

    struct TagItemPtr tagPtr;
    tagPtr.tip_TagItem = taglist;

	while (myTagItem = myNextTagItemPPC(PowerPCBase, (struct TagItem**)&tagPtr))
    {
        if ((myTagItem->ti_Tag & 0xffffe000) == GETINFO_TAGS)
		{
            switch (myTagItem->ti_Tag)
            {
                case GETINFO_CPU:
                {
                    /* ENV:sonnet/ReportG4asG3 (LIB_FLAGPPC_RG3; forced on
                     * in the G4-as-G3 test build): every G4 is a G3.
                     * G4_GETINFO_G3: THIS answer alone says G3 -- the one
                     * SDL_HasAltiVec() asks on WarpOS -- while GetCPU and
                     * GETINFO_PVR still say G4. FIXES.md "THE GETINFO-G3
                     * VARIANT", 29 Oct 2026. */
#ifdef G4_GETINFO_G3
                    if (PVR_IS_G4(PowerPCBase->pp_CPUInfo))
#else
                    if ((PowerPCBase->pp_PowerPCBase.PPC_Flags & LIB_FLAGPPC_RG3) && PVR_IS_G4(PowerPCBase->pp_CPUInfo))
#endif
                    {
                        myTagItem->ti_Data = CPUF_G3;
                        break;
                    }
                    switch (PowerPCBase->pp_DeviceID)
                    {
                        case DEVICE_MPC8343E:
                        case DEVICE_MPC8314E:
                        {
                            myTagItem->ti_Data = CPUF_603E;
                            break;
                        }
                        /*
                         * FROM THE PVR, ON BOTH BOARDS, AND ALWAYS WRITTEN, as
                         * 17.13b answered it (ppcfunctions.p:1219-1252). The
                         * Harrier arm used to answer only a 750FX or a 7410
                         * and write nothing for any other CPU, so the caller
                         * got back whatever it had passed; the MPC107 arm
                         * called every unknown CPU, a 745x included, a G3.
                         * Unknown is 0, as in 17.13b and in WarpOS, whose
                         * GetInfo always writes PPCCPU (powerpc.p:12036-12042)
                         * and sets it 0 for a type it does not know
                         * (:1005-1038). Wider than 17.13b, which matched only
                         * the 750s at 0x00080xxx and 0x00088xxx and only the
                         * 7450 of the 745x family. FIXES.md "ITEM 5,
                         * GETINFO_CPU", 31 Oct 2026.
                         */
                        case DEVICE_HARRIER:
                        case DEVICE_MPC107:
                        {
                            ULONG version = PowerPCBase->pp_CPUInfo >> 16;

                            if ((version >> 8 == 0x70) || (version == 0x0008))
                            {
                                myTagItem->ti_Data = CPUF_G3;           /* 750FX/GX, 750/750CX/755 */
                            }
                            else if (version == 0x000C)
                            {
                                myTagItem->ti_Data = CPUF_7400;         /* 7400 */
                            }
                            else if (version == 0x800C)
                            {
                                myTagItem->ti_Data = CPUF_7410 | CPUF_7400;
                            }
                            else if ((version >= 0x8000) && (version <= 0x8004))
                            {
                                myTagItem->ti_Data = CPUF_7441 | CPUF_7400;  /* 745x/744x */
                            }
                            else
                            {
                                myTagItem->ti_Data = 0;
                            }
                            break;
                        }
                        default:
                        {
                            myTagItem->ti_Data = 0;
                            break;
                        }
                    }
                    break;
                }
                case GETINFO_PVR:
                {
                    if (PowerPCBase->pp_PowerPCBase.PPC_Flags & LIB_FLAGPPC_RG3)
                    {
                        /* ENV:sonnet/ReportG4asG3: a G4 answers a 750's PVR */
                        myTagItem->ti_Data = PVR_AS_G3(PowerPCBase->pp_CPUInfo);
                    }
                    else
                    {
                        myTagItem->ti_Data = PowerPCBase->pp_CPUInfo;
                    }
                    break;
                }
                case GETINFO_ICACHE:
                case GETINFO_DCACHE:
                {
                    ULONG shift;
                    if (myTagItem->ti_Tag == GETINFO_DCACHE)
                    {
                        shift = 12;
                    }
                    else
                    {
                        shift = 13;
                    }
                    ULONG state = (PowerPCBase->pp_CPUHID0 >> shift) & 0x5;
                    switch (state)
                    {
                       case 4:
                       {
                           myTagItem->ti_Data = CACHEF_ON_UNLOCKED;
                           break;
                       }
                       case 5:
                       {
                           myTagItem->ti_Data = CACHEF_ON_LOCKED;
                           break;
                       }
                       case 0:
                       {
                           myTagItem->ti_Data = CACHEF_OFF_UNLOCKED;
                           break;
                       }
                       default:
                       {
                           myTagItem->ti_Data = CACHEF_OFF_LOCKED;
                           break;
                       }
                    }
                    break;
                }
                case GETINFO_PAGETABLE:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_CPUSDR1 & 0xffff0000;
                    break;
                }
                case GETINFO_TABLESIZE:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_PageTableSize>>10;
                    break;
                }
                case GETINFO_BUSCLOCK:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_BusClock;
                    break;
                }
                case GETINFO_CPUCLOCK:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_CPUSpeed;
                    break;
                }
                case GETINFO_CPULOAD:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_CPULoad;
                    break;
                }
                case GETINFO_SYSTEMLOAD:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_SystemLoad;
                    break;
                }
                case GETINFO_L2STATE:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_L2State & L2CR_L2E;
                    break;
                }
                case GETINFO_L2SIZE:
                {
                    myTagItem->ti_Data = PowerPCBase->pp_L2Size;
                    break;
                }
            }
		}
    }

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct MsgPortPPC* myCreateMsgPortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    struct DebugArgs args;
    args.db_Function = 31 | (1<<16);
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	struct MsgPortPPC* myPort;
	LONG signal, result;

	if (myPort = AllocVec68K(PowerPCBase, sizeof(struct MsgPortPPC), MEMF_PUBLIC | MEMF_CLEAR | MEMF_PPC))
	{
        signal = myAllocSignalPPC(PowerPCBase, -1);
        if (signal != -1)
		{
			if (result = myInitSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&myPort->mp_Semaphore))
			{
				myNewListPPC(PowerPCBase, (struct List*)&myPort->mp_IntMsg);
				myNewListPPC(PowerPCBase, (struct List*)&myPort->mp_Port.mp_MsgList);
				myPort->mp_Port.mp_SigBit = signal;
				myPort->mp_Port.mp_SigTask = PowerPCBase->pp_ThisPPCProc;
				myPort->mp_Port.mp_Flags = PA_SIGNAL;
				myPort->mp_Port.mp_Node.ln_Type = NT_MSGPORTPPC;
			}
			else
			{
				myFreeSignalPPC(PowerPCBase, signal);
				FreeVec68K(PowerPCBase, myPort);
				myPort = NULL;
			}
		}
		else
		{
			FreeVec68K(PowerPCBase, myPort);
			myPort = NULL;
		}
	}

    /*
     * A FAILED CREATE GIVES BACK ALL IT TOOK AND RETURNS NULL. Until W1
     * (REVIEW-LIB.md) a failed AllocSignalPPC freed the port and still
     * returned it, and a failed InitSemaphorePPC returned the unfinished
     * port, leaked. WarpOS frees the signal, then the port, then returns 0
     * (.freesig, .free, .err: powerpc.p:12205-12212), and so does 17.13b
     * (.NoSemMem, .NoSigFree, .NoMsgMem: ppcfunctions.p:1772-1782): THIS
     * MATCHES. Test: tests/msgport.
     */
    args.db_Arg[0] = (ULONG)myPort;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return myPort;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myDeleteMsgPortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 32 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	if(port)
	{
		myFreeSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);
		myFreeSignalPPC(PowerPCBase, port->mp_Port.mp_SigBit);
		FreeVec68K(PowerPCBase, port);
	}
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myAddPortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 33 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	myNewListPPC(PowerPCBase, (struct List*)&port->mp_Port.mp_MsgList);

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

	myEnqueuePPC(PowerPCBase, (struct List*)&PowerPCBase->pp_Ports, (struct Node*)port);

	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myRemPortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 34 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

	myRemovePPC(PowerPCBase, (struct Node*)port);

	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct MsgPortPPC* myFindPortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") STRPTR name)
{
    struct DebugArgs args;
    args.db_Function = 35 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)name;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

	struct MsgPortPPC* myPort = (struct MsgPortPPC*)myFindNamePPC(PowerPCBase, (struct List*)&PowerPCBase->pp_Ports, name);

	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

    args.db_Arg[0] = (ULONG)myPort;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return myPort;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct Message* myWaitPortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 36 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);
    MergeIntMsgs(PowerPCBase, port);                    //a handler's messages first, as WarpOS (powerpc.p:12375-12399)

    volatile struct Message* topMsg = (struct Message*)port->mp_Port.mp_MsgList.lh_Head;

    while (!(topMsg->mn_Node.ln_Succ))
    {
        ULONG waitSig = 1 << (port->mp_Port.mp_SigBit);

        myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);

        myWaitPPC(PowerPCBase, waitSig);

        myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);
        MergeIntMsgs(PowerPCBase, port);                //and after the wait (powerpc.p:12418-12444)

        topMsg = (struct Message*)port->mp_Port.mp_MsgList.lh_Head;
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);

    args.db_Arg[0] = (ULONG)topMsg;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return (struct Message*)topMsg;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myPutMsgPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port, __reg("r5") struct Message* message)
{
    struct DebugArgs args;
    args.db_Function = 37 | (2<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)port;
    args.db_Arg[1] = (ULONG)message;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    if (PowerPCBase->pp_ExceptionMode)
    {
        /*
         * FROM AN EXCEPTION HANDLER, as WarpOS's .exc (powerpc.p:12487-12490,
         * :12523-12540): onto the port's internal list, without the semaphore
         * a handler cannot wait for. The next task to use the port has the
         * kernel move it onto mp_MsgList (MergeIntMsgs, HandleDeferred).
         * FIXES.md "#1 PART B DESIGNED", 1 Nov 2026.
         */
        message->mn_Node.ln_Type = NT_MESSAGE;
        myAddTailPPC(PowerPCBase, &port->mp_IntMsg, (struct Node*)message);

        if ((port->mp_Port.mp_SigTask) && (!(port->mp_Port.mp_Flags & PF_ACTION)))
        {
            mySignalPPC(PowerPCBase, port->mp_Port.mp_SigTask, (1 << port->mp_Port.mp_SigBit));
        }
        return;
    }

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);
    MergeIntMsgs(PowerPCBase, port);                    //a handler's messages first, as WarpOS

    message->mn_Node.ln_Type = NT_MESSAGE;

    myAddTailPPC(PowerPCBase, (struct List*)&port->mp_Port.mp_MsgList, (struct Node*)message);

    if ((port->mp_Port.mp_SigTask) && (!(port->mp_Port.mp_Flags & PF_ACTION)))
    {
         mySignalPPC(PowerPCBase, port->mp_Port.mp_SigTask, (1 << port->mp_Port.mp_SigBit));
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct Message* myGetMsgPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 38 | (1<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);
    MergeIntMsgs(PowerPCBase, port);                    //a handler's messages first, as WarpOS (powerpc.p:12579-12603)

    struct Message* myMsg = (struct Message*)myRemHeadPPC(PowerPCBase, (struct List*)&port->mp_Port.mp_MsgList);

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&port->mp_Semaphore);

    args.db_Arg[0] = (ULONG)myMsg;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return myMsg;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myReplyMsgPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct Message* message)
{
    struct DebugArgs args;
    args.db_Function = 39 | (1<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)message;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    if (!(message->mn_ReplyPort))
    {
        message->mn_Node.ln_Type = NT_FREEMSG;
        return;
    }

    /*
     * EVERY REPLY IS NT_REPLYMSG, as WarpOS (powerpc.p:12645-12646) and
     * 17.13b (ppcfunctions.p:7808-7810) set it before branching. This set it
     * only for an NT_XMSG68K message, so an ordinary reply kept its type.
     * FIXES.md "#1 (THE 11:03 TABLE), MESSAGE PORTS FROM EXCEPTION
     * HANDLERS", part A, 1 Nov 2026.
     */
    UBYTE oldType = message->mn_Node.ln_Type;
    message->mn_Node.ln_Type = NT_REPLYMSG;

    if (oldType == NT_XMSG68K)
    {
        struct MsgFrame* myFrame = CreateMsgFramePPC(PowerPCBase);
        myFrame->mf_Arg[0] = (ULONG)message;
        myFrame->mf_Identifier = ID_RX68;
        SendMsgFramePPC(PowerPCBase, myFrame);
        return;
    }
    else
    {
        struct MsgPortPPC* myPort = (struct MsgPortPPC*)message->mn_ReplyPort;

        if (PowerPCBase->pp_ExceptionMode)
        {
            /*
             * FROM AN EXCEPTION HANDLER, as WarpOS's .exc (powerpc.p:12647-12650,
             * :12701-12713): onto the reply port's internal list, without its
             * semaphore. WarpOS takes this branch before its 68k-message test,
             * which would put an NT_XMESSAGE reply on a 68k port's list; here
             * the 68k branch above goes first, its frames taking no lock.
             * FIXES.md "#1 PART B DESIGNED", 1 Nov 2026.
             */
            myAddTailPPC(PowerPCBase, &myPort->mp_IntMsg, (struct Node*)message);

            if ((myPort->mp_Port.mp_SigTask) && (!(myPort->mp_Port.mp_Flags & PF_ACTION)))
            {
                mySignalPPC(PowerPCBase, myPort->mp_Port.mp_SigTask, (1 << myPort->mp_Port.mp_SigBit));
            }
            return;
        }

        myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&myPort->mp_Semaphore);
        MergeIntMsgs(PowerPCBase, myPort);              //a handler's messages first, as WarpOS (powerpc.p:12672-12697)
        myAddTailPPC(PowerPCBase, (struct List*)&myPort->mp_Port.mp_MsgList, (struct Node*)message);

        if ((myPort->mp_Port.mp_SigTask) && (!(myPort->mp_Port.mp_Flags & PF_ACTION)))
        {
             mySignalPPC(PowerPCBase, myPort->mp_Port.mp_SigTask, (1 << myPort->mp_Port.mp_SigBit));
        }

        myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&myPort->mp_Semaphore);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myCopyMemPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") APTR source, __reg("r5") APTR dest, __reg("r6") ULONG size)
{
    struct DebugArgs args;
    args.db_Function = 40 | (3<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)source;
    args.db_Arg[1] = (ULONG)dest;
    args.db_Arg[2] = size;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

#ifdef G4_NO_VMX_COPY
    /* THE NO-VMX-COPY TEST BUILD (build.ps1 -G4VecOnNoVmx; FIXES.md "THE
     * VEC-ON NO-VMX-COPY VARIANT", 29 Oct 2026): the plain copy always, so
     * CopyMemQuickVMX -- whose only caller is below -- never runs. */
    CopyMemQuickPPC(PowerPCBase, source, dest, size);
    return;
#endif
    if (PowerPCBase->pp_EnAltivec)
    {
        if (PowerPCBase->pp_ExceptionMode)
        {
            CopyMemQuickPPC(PowerPCBase, source, dest, size);
        }
        else
        {
            //CopyMemAltivec(dest, source, size);
            CopyMemQuickVMX(PowerPCBase, source, dest, size);
            //CopyMemQuickPPC(PowerPCBase, source, dest, size);
        }
    }
    else
    {
        CopyMemQuickPPC(PowerPCBase, source, dest, size);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct Message* myAllocXMsgPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG length, __reg("r5") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 41 | (2<<8) | (1<<16);
    args.db_Arg[0] = length;
    args.db_Arg[1] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	length = (length + sizeof(struct Message) + 31) & -32;

	struct Message* myXMessage;

	if (myXMessage = myAllocVecPPC(PowerPCBase, length, MEMF_PUBLIC | MEMF_CLEAR | MEMF_PPC, 32L))
	{
		myXMessage->mn_ReplyPort = (struct MsgPort*)port;
		myXMessage->mn_Length = length;
		/*
		 * F6: THE 68k READS THIS BLOCK'S HEADER. myFreeXMsg (functions68k.c)
		 * tells a PPC XMsg by the MAGIC_COOKIE myAllocVecPPC put at msg-16,
		 * so that line goes to memory now, as PutXMsgPPC sends the body
		 * (below). Nothing writes the header again until the block is freed.
		 */
		mySetCache(PowerPCBase, CACHE_DCACHEFLUSH, (APTR)((ULONG)myXMessage - 16), 16);
	}

    args.db_Arg[0] = (ULONG)myXMessage;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return myXMessage;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myFreeXMsgPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct Message* message)
{
	struct DebugArgs args;
    args.db_Function = 42 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)message;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    /*
     * F6: EACH SIDE'S XMsg IS FREED ITS OWN WAY. A PPC XMsg is an AllocVecPPC
     * block, with MAGIC_COOKIE at msg-16 (myAllocVecPPC); a 68k one is
     * AllocXMsg's AllocVec32 block, with the 68k pointer at msg-4, where
     * myFreeVecPPC found no cookie and halted. A block without the cookie is
     * now freed by FreeVec68K, which is FreeVec32 by Run68K (supportppc.c).
     * WarpOS hands a 68k XMsg to the 68k's FreeXMsg (powerpc.p:12951-12958),
     * telling it by the type; the cookie is read here instead, as a reply
     * changes the type. 17.13b called FreeVecPPC only
     * (ppcfunctions.p:1706-1711).
     */
    if (message)
    {
        if (((ULONG*)message)[-4] == MAGIC_COOKIE)
        {
            myFreeVecPPC(PowerPCBase, message);
        }
        else
        {
            FreeVec68K(PowerPCBase, message);
        }
    }

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myPutXMsgPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPort* port, __reg("r5") struct Message* message)
{
    struct DebugArgs args;
    args.db_Function = 43 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)port;
    args.db_Arg[1] = (ULONG)message;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	message->mn_Node.ln_Type = NT_XMSGPPC;

	mySetCache(PowerPCBase, CACHE_DCACHEFLUSH, message, message->mn_Length);

	struct MsgFrame* myFrame = CreateMsgFramePPC(PowerPCBase);

	myFrame->mf_Identifier = ID_XMSG;
	myFrame->mf_Arg[0] = (ULONG)port;
	myFrame->mf_Arg[1] = (ULONG)message;
	myFrame->mf_Message.mn_Node.ln_Type = NT_MESSAGE;

	SendMsgFramePPC(PowerPCBase, myFrame);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myGetSysTimePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct timeval* timeval)
{
    struct DebugArgs args;
    args.db_Function = 44 | (1<<8) | (1<<16) | (1<<17);
    args.db_Arg[0] = (ULONG)timeval;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);
    struct UInt64 myInt64;
    ULONG tbu, tbl, secs, micros, temp;

    while (1)
    {
        tbu = getTBU();
        tbl = getTBL();
        if (tbu == getTBU())
        {
            break;
        }
    }

    ULONG busClockMod = PowerPCBase->pp_BusClock >> 2;

    secs = Calculator(tbu, tbl, busClockMod);
    timeval->tv_secs = secs;

    temp = secs * busClockMod;
    temp = tbl - temp;

    mulInt64((struct UInt64*)&myInt64, 1000000, temp);

    micros = Calculator(myInt64.ui_High, myInt64.ui_Low, busClockMod);

    timeval->tv_micro = micros;

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myAddTimePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct timeval* dest, __reg("r5") struct timeval* source)
{
    ULONG timeSecs = 0;
    LONG timeMicro = dest->tv_micro + source->tv_micro;
    if (1000000 <= timeMicro)
    {
        timeSecs = 1;
        timeMicro -= 1000000;
    }
    dest->tv_secs = dest->tv_secs + source->tv_secs + timeSecs;
    dest->tv_micro = timeMicro;

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID mySubTimePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct timeval* dest, __reg("r5") struct timeval* source)
{
    ULONG timeSecs = 0;
    LONG timeMicro = dest->tv_micro - source->tv_micro;
    if (timeMicro < 0)
    {
        timeSecs = 1;
        timeMicro += 1000000;
    }
    dest->tv_secs = dest->tv_secs - source->tv_secs - timeSecs;
    dest->tv_micro = timeMicro;

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myCmpTimePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct timeval* dest, __reg("r5") struct timeval* source)
{
    if (dest->tv_secs < source->tv_secs)
    {
        return CMP_DESTLESS;
    }
    else if (dest->tv_secs > source->tv_secs)
    {
        return CMP_DESTGREATER;
    }
    else
    {
        if (dest->tv_micro < source->tv_micro)
        {
            return CMP_DESTLESS;
        }
        else if (dest->tv_micro > source->tv_micro)
        {
            return CMP_DESTGREATER;
        }
        else
        {
            return CMP_EQUAL;
        }
    }
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct MsgPortPPC* mySetReplyPortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct Message* message, __reg("r5") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 45 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)message;
    args.db_Arg[1] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	struct MsgPortPPC* oldPort = (struct MsgPortPPC*)message->mn_ReplyPort;
	message->mn_ReplyPort = (struct MsgPort*)port;

    args.db_Arg[0] = (ULONG)oldPort;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return oldPort;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG mySnoopTask(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TagItem* taglist)
{
    struct DebugArgs args;
    args.db_Function = 46 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)taglist;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    ULONG value;

    struct SnoopData* mySnoop = AllocVec68K(PowerPCBase, sizeof(struct SnoopData), MEMF_PUBLIC | MEMF_CLEAR);

    if (mySnoop)
    {
        if (value = myGetTagDataPPC(PowerPCBase, SNOOP_CODE, 0, taglist))
        {
            mySnoop->sd_Code = (APTR)value;
            mySnoop->sd_Data = myGetTagDataPPC(PowerPCBase, SNOOP_DATA, 0, taglist);
            if(!(mySnoop->sd_Type = myGetTagDataPPC(PowerPCBase, SNOOP_TYPE, 0, taglist)))
            {
                FreeVec68K(PowerPCBase, mySnoop);
                mySnoop = 0;
            }
            else
            {
                myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

                myAddHeadPPC(PowerPCBase, (struct List*)&PowerPCBase->pp_Snoop, (struct Node*)mySnoop);

                myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);
            }
        }
        else
        {
            FreeVec68K(PowerPCBase, mySnoop);
            mySnoop = 0;
        }
    }

    args.db_Arg[0] = (ULONG)mySnoop;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return (ULONG)mySnoop;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myEndSnoopTask(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG id)
{
    struct DebugArgs args;
    args.db_Function = 47 | (1<<8) | (1<<16);
    args.db_Arg[0] = id;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    if (id)
    {
        myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

        myRemovePPC(PowerPCBase, (struct Node*)id);

        myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);

        FreeVec68K(PowerPCBase, (APTR)id);
    }

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myGetHALInfo(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TagItem* taglist)
{
    struct DebugArgs args;
    args.db_Function = 48 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)taglist;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct TagItem* myTagItem;

    if (myTagItem = myFindTagItemPPC(PowerPCBase,HINFO_ALEXC_HIGH, taglist))
    {
        myTagItem->ti_Data = PowerPCBase->pp_AlignmentExcHigh;
    }
    if (myTagItem = myFindTagItemPPC(PowerPCBase,HINFO_ALEXC_LOW, taglist))
    {
        myTagItem->ti_Data = PowerPCBase->pp_AlignmentExcLow;
    }
    if (myTagItem = myFindTagItemPPC(PowerPCBase,HINFO_DSEXC_HIGH, taglist))
    {
        myTagItem->ti_Data = PowerPCBase->pp_DataExcHigh;
    }
    if (myTagItem = myFindTagItemPPC(PowerPCBase,HINFO_DSEXC_LOW, taglist))
    {
        myTagItem->ti_Data = PowerPCBase->pp_DataExcLow;
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID mySetScheduling(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TagItem* taglist)
{
    struct DebugArgs args;
    args.db_Function = 49 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)taglist;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct TagItem* myTagItem;
    ULONG result;

    if (myTagItem = myFindTagItemPPC(PowerPCBase, SCHED_REACTION, taglist))
    {
        result = myTagItem->ti_Data;
        if (result < 1)
        {
            result = 1;
        }
        else if (result > 20)
        {
            result = 20;
        }
        PowerPCBase->pp_LowActivityPri = (result * 1000);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION struct TaskPPC* myFindTaskByID(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") LONG id)
{
    struct DebugArgs args;
    args.db_Function = 50 | (1<<8) | (1<<16);
    args.db_Arg[0] = id;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct TaskPtr* myPtr = myLockTaskList(PowerPCBase);
    struct TaskPPC* foundTask = NULL;

    while (myPtr->tptr_Node.ln_Succ)
    {
        struct TaskPPC* myTask = (struct TaskPPC*)myPtr->tptr_Task;
        if (id == myTask->tp_Id)
        {
            foundTask = myTask;
            break;
        }
        myPtr = (struct TaskPtr*)myPtr->tptr_Node.ln_Succ;
    }

    myUnLockTaskList(PowerPCBase);

    args.db_Arg[0] = (ULONG)foundTask;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return foundTask;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG mySetNiceValue(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct TaskPPC* task, __reg("r5") LONG nice)
{
    struct DebugArgs args;
    args.db_Function = 51 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)task;
    args.db_Arg[1] = nice;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    LONG oldNice;

    if (oldNice = (LONG)task)
    {
        if (nice < -20)
        {
            nice = -20;
        }
        else if (nice > 20)
        {
            nice = 20;
        }

        myLockTaskList(PowerPCBase);

        oldNice = task->tp_Nice;
        task->tp_Nice = nice;

        myUnLockTaskList(PowerPCBase);
    }

    args.db_Arg[0] = oldNice;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return oldNice;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myTrySemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC, __reg("r5") ULONG timeout)
{
    struct DebugArgs args;
    args.db_Function = 52 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    args.db_Arg[1] = timeout;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    LONG status;

    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    while (!(LockMutexPPC((volatile ULONG)SemaphorePPC->ssppc_reserved)));

    if (!(SemaphorePPC->ssppc_SS.ss_QueueCount += 1))
    {
        SemaphorePPC->ssppc_SS.ss_Owner = (struct Task*)myTask;
        SemaphorePPC->ssppc_SS.ss_NestCount += 1;   //W2: a grant counts a nest, as WarpOS's .success -> .end (powerpc.p:13534-13537) and AttemptSemaphorePPC here; without it the release halted the task. REVIEW-LIB.md W2
        FreeMutexPPC((ULONG)SemaphorePPC->ssppc_reserved);
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
        status = ATTEMPT_SUCCESS;
    }
    else
    {
        if (SemaphorePPC->ssppc_SS.ss_Owner == (struct Task*)myTask)
        {
            SemaphorePPC->ssppc_SS.ss_NestCount += 1;   //W2: the owner's nested grant too
            FreeMutexPPC((ULONG)SemaphorePPC->ssppc_reserved);
            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
            status = ATTEMPT_SUCCESS;
        }
        else
        {
            struct SemWait semWait;
            semWait.sw_Task = myTask;
            myTask->tp_Task.tc_SigRecvd &= ~SIGF_SINGLE;
            myAddTailPPC(PowerPCBase, (struct List*)&SemaphorePPC->ssppc_SS.ss_WaitQueue, (struct Node*)&semWait);

            FreeMutexPPC((ULONG)SemaphorePPC->ssppc_reserved);
            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

            ULONG signal = myWaitTime(PowerPCBase, SIGF_SINGLE, timeout);

            /*
             * THE OUTCOME IS READ FROM ss_Owner, UNDER pp_Mutex -- T4 (REVIEW-LIB.md),
             * a deliberate divergence from WarpOS and 17.13b. Both spin until the
             * releaser clears ssppc_lock, then decide by WaitTime's result
             * (powerpc.p:13500-13525; ppcfunctions.p:4039-4084). Here that spin
             * froze the PPC: the releaser's SignalPPC runs an atomic waiter at once
             * (kernel.c priShouldSwitch, the atomic rule), a running atomic task is
             * not switched out (SwitchPPC, R3), so the releaser never got back to
             * clear the lock. WarpOS needs the lock because its releaser writes the
             * new owner after PERMIT (powerpc.p:10079, :10097). Ours makes every
             * grant -- our node off the queue, the owner, the nest -- before it
             * frees pp_Mutex (R1, myReleaseSemaphorePPC), so ss_Owner alone says
             * whether we were granted; only the wake-up comes later.
             *
             * GRANTED: success. The releaser's SIGF_SINGLE may still be on its way;
             * it is waited for here, so that no later wait takes it for its own.
             * A timeout that raced the grant counts as the grant: the old code
             * removed the node a second time and returned failure while owning the
             * semaphore.
             *
             * NOT GRANTED: our node is still queued. It comes off, and the queue
             * count comes down by the one this call added, as myVacatePPC undoes a
             * withdrawn waiter (WarpOS the same, powerpc.p:13934-13936). WarpOS,
             * 17.13b and 17.15 add a nest instead (powerpc.p:13532-13537,
             * ppcfunctions.p:4092-4095), so the owner's last release kept the
             * semaphore and the next ObtainSemaphorePPC waited for ever.
             */
            while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

            ULONG sigMask = signal | myTask->tp_Task.tc_SigRecvd;
            ULONG gotSignal = sigMask & SIGF_SINGLE;
            sigMask &= ~SIGF_SINGLE;
            myTask->tp_Task.tc_SigRecvd = sigMask;

            if (SemaphorePPC->ssppc_SS.ss_Owner == (struct Task*)myTask)
            {
                FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

                if (!(gotSignal))
                {
                    myWaitPPC(PowerPCBase, SIGF_SINGLE);
                }
                status = ATTEMPT_SUCCESS;
            }
            else
            {
                myRemovePPC(PowerPCBase, (struct Node*)&semWait);
                SemaphorePPC->ssppc_SS.ss_QueueCount -= 1;
                FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
                status = ATTEMPT_FAILURE;
            }
        }
    }
    return status;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG mySetExceptPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG signals, __reg("r5") ULONG mask, __reg("r6") ULONG flag)
{
    struct DebugArgs args;
    args.db_Function = 53 | (3<<8) | (1<<16);
    args.db_Arg[0] = signals;
    args.db_Arg[1] = mask;
    args.db_Arg[2] = flag;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    ULONG OldSignals = myTask->tp_Task.tc_SigExcept;
    ULONG newSignals = (signals & mask) | (OldSignals & ~mask);
    myTask->tp_Task.tc_SigExcept = newSignals;
    if (flag)
    {
       myTask->tp_Task.tc_ExceptData = (APTR)getR2();
    }

    FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

    CheckExcSignal(PowerPCBase, myTask, 0);

    args.db_Arg[0] = OldSignals;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return OldSignals;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myObtainSemaphoreSharedPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 54 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct SemWait myWait;
    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    if (!(SemaphorePPC->ssppc_SS.ss_QueueCount += 1))
    {
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
        SemaphorePPC->ssppc_SS.ss_NestCount += 1;
    }
    else
    {
        if ((!(SemaphorePPC->ssppc_SS.ss_Owner)) || (SemaphorePPC->ssppc_SS.ss_Owner == (struct Task*)myTask))
        {
            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
            SemaphorePPC->ssppc_SS.ss_NestCount += 1;
        }
        else
        {
            myWait.sw_Task = (struct TaskPPC*)((ULONG)myTask | SM_SHARED);
            myTask->tp_Task.tc_SigRecvd &= ~(SIGF_SINGLE);
            myAddTailPPC(PowerPCBase, (struct List*)&SemaphorePPC->ssppc_SS.ss_WaitQueue, (struct Node*)&myWait);
            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
            myWaitPPC(PowerPCBase, SIGF_SINGLE);
        }
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myAttemptSemaphoreSharedPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 55 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    LONG result;
    struct TaskPPC* myTask = PowerPCBase->pp_ThisPPCProc;

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    if (!(SemaphorePPC->ssppc_SS.ss_QueueCount + 1) || !(SemaphorePPC->ssppc_SS.ss_Owner) || (SemaphorePPC->ssppc_SS.ss_Owner == (struct Task*)myTask))
    {
        SemaphorePPC->ssppc_SS.ss_QueueCount += 1;
        SemaphorePPC->ssppc_SS.ss_NestCount += 1;
        result = ATTEMPT_SUCCESS;
    }
    else
    {
        result = ATTEMPT_FAILURE;
    }

    FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);

    args.db_Arg[0] = result;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return result;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myProcurePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC, __reg("r5") struct SemaphoreMessage* SemaphoreMessage)
{
    struct DebugArgs args;
    args.db_Function = 56 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    args.db_Arg[1] = (ULONG)SemaphoreMessage;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    SemaphoreMessage->ssm_Semaphore = (struct SignalSemaphore*)PowerPCBase->pp_ThisPPCProc;

    struct TaskPPC* owner = PowerPCBase->pp_ThisPPCProc;

    ULONG myStore = (ULONG)&SemaphoreMessage->ssm_Message.mn_Node.ln_Type;

    if (*((ULONG*)(myStore)) = (ULONG)SemaphoreMessage->ssm_Message.mn_Node.ln_Name)
    {
        owner = NULL;
    }

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));
    //R7: ONE increment; granted if it was free or its owner already is ours (the task, or NULL for a shared procure), queued otherwise -- as WarpOS (powerpc.p:13867-13890) and 17.13b (ppcfunctions.p:5655-5686). FIXES.md "CODE REVIEW OF lib/"
    if ((SemaphorePPC->ssppc_SS.ss_QueueCount += 1) && (SemaphorePPC->ssppc_SS.ss_Owner != (struct Task*)owner))
    {
        myAddTailPPC(PowerPCBase, (struct List*)&SemaphorePPC->ssppc_SS.ss_WaitQueue, (struct Node*)SemaphoreMessage);
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
    }
    else
    {
        if (!(SemaphorePPC->ssppc_SS.ss_QueueCount))                       //it was free: WarpOS "stw r28,SS_OWNER" (:13872)
        {
            SemaphorePPC->ssppc_SS.ss_Owner = (struct Task*)owner;
        }
        SemaphoreMessage->ssm_Semaphore = (struct SignalSemaphore*)SemaphorePPC;
        *((ULONG*)(myStore)) = 0;                                           //the whole waiter word at +8: WarpOS "stw r0,SSR_WAITER" (:13876), 17.13b "stw r0,LN_TYPE" (:5663)
        SemaphorePPC->ssppc_SS.ss_NestCount += 1;
        FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
        myReplyMsgPPC(PowerPCBase, (struct Message*)SemaphoreMessage);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myVacatePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC, __reg("r5") struct SemaphoreMessage* SemaphoreMessage)
{
    struct DebugArgs args;
    args.db_Function = 57 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    args.db_Arg[1] = (ULONG)SemaphoreMessage;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    ULONG myStore = (ULONG)&SemaphoreMessage->ssm_Message.mn_Node.ln_Type;
    *((ULONG*)(myStore)) = 0;                                           //R8: the whole waiter word at +8, not the local -- WarpOS "stw r0,SSR_WAITER" (powerpc.p:13918), 17.13b "stw r0,LN_TYPE" (ppcfunctions.p:5722)
    SemaphoreMessage->ssm_Semaphore = 0;

    while (!(LockMutexPPC((volatile ULONG)&PowerPCBase->pp_Mutex)));

    struct Node* currMsg = (struct Node*)SemaphorePPC->ssppc_SS.ss_WaitQueue.mlh_Head;

    while (1)
    {
        if (currMsg == (struct Node*)SemaphoreMessage)
        {
            SemaphorePPC->ssppc_SS.ss_QueueCount -= 1;
            myRemovePPC(PowerPCBase, currMsg);
            FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
            myReplyMsgPPC(PowerPCBase, (struct Message*)SemaphoreMessage);
            break;
        }
        else
        {
            if(!(currMsg = currMsg->ln_Succ))
            {
                FreeMutexPPC((ULONG)&PowerPCBase->pp_Mutex);
                myReleaseSemaphorePPC(PowerPCBase, SemaphorePPC);
                break;
            }
        }
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION APTR myCreatePoolPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG flags, __reg("r5") ULONG puddle_size, __reg("r6") ULONG thres_size)
{
    struct DebugArgs args;
    args.db_Function = 58 | (3<<8) | (1<<16);
    args.db_Arg[0] = flags;
    args.db_Arg[1] = puddle_size;
    args.db_Arg[2] = thres_size;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	struct poolHeader* myPoolHeader = 0;

	if (puddle_size >= thres_size)
	{
        if (myPoolHeader = AllocVec68K(PowerPCBase, sizeof(struct poolHeader), flags))
		{
            puddle_size = (puddle_size + 31) & -32;
			myNewListPPC(PowerPCBase, (struct List*)&myPoolHeader->ph_PuddleList);
			myNewListPPC(PowerPCBase, (struct List*)&myPoolHeader->ph_BlockList);

			myPoolHeader->ph_Requirements = flags;
			myPoolHeader->ph_PuddleSize = puddle_size;
			myPoolHeader->ph_ThresholdSize = thres_size;

            myAddHeadPPC(PowerPCBase, &PowerPCBase->pp_ThisPPCProc->tp_TaskPools, (struct Node*)&myPoolHeader->ph_Node);
		}
	}

    args.db_Arg[0] = (ULONG)myPoolHeader;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return myPoolHeader;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myDeletePoolPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") APTR poolheader)
{
    struct DebugArgs args;
    args.db_Function = 59 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)poolheader;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	if (poolheader)
	{
		myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemMemory);
		myRemovePPC(PowerPCBase, (struct Node*)poolheader);

        struct poolHeader* myheader = (struct poolHeader*)poolheader;

        struct List* puddleList = (struct List*)(&(myheader->ph_PuddleList));
        struct List* blockList = (struct List*)(&(myheader->ph_BlockList));
		struct Node* currNode;

		while (currNode = myRemHeadPPC(PowerPCBase, puddleList))
		{
			FreeVec68K(PowerPCBase, currNode);
		}
		while (currNode = myRemHeadPPC(PowerPCBase, blockList))
		{
			FreeVec68K(PowerPCBase, currNode);
		}

		FreeVec68K(PowerPCBase, poolheader);

		myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemMemory);
	}
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION APTR myAllocPooledPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") APTR poolheader, __reg("r5") ULONG size)
{
    struct DebugArgs args;
    args.db_Function = 60 | (2<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)poolheader;
    args.db_Arg[1] = size;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct Node* memBlock;
    struct MemHeader* puddle;
    APTR mem = NULL;
    struct poolHeader* myHeader = (struct poolHeader*)poolheader;

    /*
     * F5: NO POOL OR NO SIZE GETS NULL, before pp_SemMemory is taken. A NULL
     * pool was read at once (ph_ThresholdSize, +0x10) with the semaphore
     * held: the task was killed holding it, and every later PPC allocation
     * waited for ever. A size of 0 returned a pointer just past its own 32
     * bytes, the start of the next block. MATCHES WarpOS
     * (warpos/warpos/mm/pool.c:101-102); 17.13b tests neither
     * (ppcfunctions.p:6872-6876). AllocVecPPC passes neither (:349-351).
     */
    if ((!(myHeader)) || (!(size)))
    {
        args.db_Arg[0] = 0;
        printDebug(PowerPCBase, (struct DebugArgs*)&args);
        return NULL;
    }

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemMemory);

    if (myHeader->ph_ThresholdSize < size)
    {
        if (memBlock = AllocVec68K(PowerPCBase, size+32, myHeader->ph_Requirements))
        {
            memBlock->ln_Type = NT_MEMORY;
            myAddHeadPPC(PowerPCBase, (struct List*)&myHeader->ph_BlockList, memBlock);
            mem = (APTR)((ULONG)memBlock + 32);
            (*((ULONG*)((ULONG)mem - 4))) = size + 32;
        }
    }
    else
    {
        if (!(puddle = (struct MemHeader*)myRemHeadPPC(PowerPCBase, (struct List*)&myHeader->ph_PuddleList)))
        {
            if (puddle = AllocVec68K(PowerPCBase, myHeader->ph_PuddleSize + sizeof(struct MemHeader), myHeader->ph_Requirements))
            {
                struct MemChunk* startPuddle = (struct MemChunk*)((ULONG)puddle + sizeof(struct MemHeader));
                puddle->mh_First = startPuddle;
                puddle->mh_Lower = startPuddle;
                puddle->mh_Node.ln_Type = NT_MEMORY;
                puddle->mh_Attributes = myHeader->ph_Requirements;
                startPuddle->mc_Next = NULL;
                startPuddle->mc_Bytes = myHeader->ph_PuddleSize;
                puddle->mh_Free = myHeader->ph_PuddleSize;
                puddle->mh_Upper = (APTR)((ULONG)startPuddle + myHeader->ph_PuddleSize);
            }
        }
        if (puddle)
        {
            if (!(mem = AllocatePPC(PowerPCBase, puddle, size + 32)))
            {
                myAddHeadPPC(PowerPCBase, (struct List*)&myHeader->ph_PuddleList, (struct Node*)puddle);

                if (puddle = AllocVec68K(PowerPCBase, myHeader->ph_PuddleSize + sizeof(struct MemHeader), myHeader->ph_Requirements))
                {
                    struct MemChunk* startPuddle = (struct MemChunk*)((ULONG)puddle + sizeof(struct MemHeader));
                    puddle->mh_First = startPuddle;
                    puddle->mh_Lower = startPuddle;
                    puddle->mh_Node.ln_Type = NT_MEMORY;
                    puddle->mh_Attributes = myHeader->ph_Requirements;
                    startPuddle->mc_Next = NULL;
                    startPuddle->mc_Bytes = myHeader->ph_PuddleSize;
                    puddle->mh_Free = myHeader->ph_PuddleSize;
                    puddle->mh_Upper = (APTR)((ULONG)startPuddle + myHeader->ph_PuddleSize);
                    mem = AllocatePPC(PowerPCBase, puddle, size + 32);
                 }
            }
            if (puddle)
            {
                ULONG divider = (myHeader->ph_PuddleSize >> 8) & 0xffffff;
                ULONG granularity = (puddle->mh_Free / divider) + 0x80;
                puddle->mh_Node.ln_Pri = (UBYTE)granularity;
                myEnqueuePPC(PowerPCBase, (struct List*)&myHeader->ph_PuddleList, (struct Node*)puddle);
            }
            if (mem)
            {
                mem = (APTR)((ULONG)mem + 32);
                (*((ULONG*)((ULONG)mem - 4))) = size + 32;
            }
        }
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemMemory);

    args.db_Arg[0] = (ULONG)mem;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return mem;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myFreePooledPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") APTR poolheader, __reg("r5") APTR ptr, __reg("r6") ULONG size)
{
    struct DebugArgs args;
    args.db_Function = 61 | (3<<8) | (1<<16) | (2<<17);
    args.db_Arg[0] = (ULONG)poolheader;
    args.db_Arg[1] = (ULONG)ptr;
    args.db_Arg[2] = size;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    struct poolHeader* myHeader = poolheader;

    /*
     * F5: NO POOL, NOTHING DONE, before pp_SemMemory is taken: a NULL pool
     * was read at once with the semaphore held, and the task was killed
     * holding it. WarpOS's FreePooledPPC never reads the pool; it frees from
     * the chunk's own header (warpos/warpos/mm/pool.c:178-246). Here the
     * pool is needed to find the block's puddle, so the block stays in its
     * pool until DeletePoolPPC frees it. 17.13b tests the pointer only
     * (ppcfunctions.p:7220-7221), as this did. FreeVecPPC passes the pool
     * it recorded (:391).
     */
    if ((!(ptr)) || (!(myHeader)))
    {
        return;
    }
    if (!(size))
    {
        size = (*((ULONG*)((ULONG)ptr - 4))) - 32;
    }

    ULONG mem = (ULONG)ptr - 32;

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemMemory);

    if (myHeader->ph_ThresholdSize < size)
    {
        myRemovePPC(PowerPCBase, (struct Node*)mem);
        FreeVec68K(PowerPCBase, (APTR)mem);
    }
    else
    {
        struct MemHeader* currPuddle = (struct MemHeader*)myHeader->ph_PuddleList.mlh_Head;
        struct MemHeader* nextPuddle;

        while (nextPuddle = (struct MemHeader*)currPuddle->mh_Node.ln_Succ)
        {
            if ((mem >= (ULONG)currPuddle->mh_Lower) && (mem < (ULONG)currPuddle->mh_Upper))
            {
                DeallocatePPC(PowerPCBase, currPuddle, (APTR)mem, size + 32);

                myRemovePPC(PowerPCBase, (struct Node*)currPuddle);

                if (currPuddle->mh_Free == myHeader->ph_PuddleSize)
                {
                    FreeVec68K(PowerPCBase, currPuddle);
                }
                else
                {
                    ULONG divider = (myHeader->ph_PuddleSize >> 8) & 0xffffff;
                    ULONG granularity = (currPuddle->mh_Free / divider) + 0x80;
                    currPuddle->mh_Node.ln_Pri = (UBYTE)granularity;
                    myEnqueuePPC(PowerPCBase, (struct List*)&myHeader->ph_PuddleList, (struct Node*)currPuddle);
                }
                break;
            }
            currPuddle = nextPuddle;
        }
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemMemory);
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION APTR myRawDoFmtPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") STRPTR formatstring, __reg("r5") APTR datastream, __reg("r6") APTR (*putchproc)(), __reg("r7") APTR putchdata)
{
    struct DebugArgs args;
    args.db_Function = 62 | (4<<8) | (1<<16) | (1<<17);
    args.db_Arg[0] = (ULONG)formatstring;
    args.db_Arg[1] = (ULONG)datastream;
    args.db_Arg[2] = (ULONG)putchproc;
    args.db_Arg[3] = (ULONG)putchdata;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    UBYTE sLength;
    STRPTR stringPtr;
    struct RDFData rdfData;

    if (putchproc == (APTR)1)
    {
        return PowerPCBase;   //emulate real WarpOS where r3 is not changed (and so is PowerPCBase)
    }

    rdfData.rd_DataStream = datastream;
    rdfData.rd_PutChData = putchdata;
    rdfData.rd_FormatString = formatstring;

    UBYTE currChar;

    while (currChar = rdfData.rd_FormatString[0])
    {
        rdfData.rd_FormatString += 1;
        if (currChar == '%')
        {
            rdfData.rd_BufPointer = (STRPTR)&rdfData.rd_Buffer;
            currChar = rdfData.rd_FormatString[0];
            ULONG flag = 0;
            if (currChar == '-')        //flag->left align
            {
                flag |= RDFF_JUSTIFY;
                rdfData.rd_FormatString += 1;
            }
            currChar = rdfData.rd_FormatString[0];
            if (currChar == '0')        //flag->prepends zeros
            {
                flag |= RDFF_PREPEND;
            }
            LONG prependNum = getNum(&rdfData);
            rdfData.rd_TruncateNum = 0;
            currChar = rdfData.rd_FormatString[0];
            if (currChar == '.')        //precision
            {
                rdfData.rd_FormatString += 1;
                rdfData.rd_TruncateNum = getNum(&rdfData);
            }
            currChar = rdfData.rd_FormatString[0];
            if (currChar == 'l')        //length->long
            {
                flag |= RDFF_LONG;
                rdfData.rd_FormatString += 1;
            }
            currChar = rdfData.rd_FormatString[0];
            rdfData.rd_FormatString += 1;
            switch (currChar)
            {
                case 'd':    //type->signed decimal number
                case 'D':
                {
                    MakeDecimal(&rdfData, TRUE, AdjustParam(&rdfData, flag));
                    goto FmtOutput;
                }
                case 'x':    //type->hexidecimal number
                case 'X':
                {
                    MakeHex(&rdfData, flag, AdjustParam(&rdfData, flag));
                    goto FmtOutput;
                }
                case 's':    //type->null-terminated string
                {
                    if (!(stringPtr = (STRPTR)AdjustParamInt(&rdfData)))
                    {
                        break;
                    }
                    rdfData.rd_BufPointer = stringPtr;
                    goto StrOutput;
                }
                case 'b':    //type->bcpl string
                {
                    if (!(stringPtr = (STRPTR)AdjustParamInt(&rdfData)))
                    {
                        break;
                    }
                    stringPtr = (STRPTR)((ULONG)stringPtr << 2);

                    sLength = stringPtr[0];

                    if (!(sLength))
                    {
                        break;
                    }

                    stringPtr += 1;
                    sLength -= 1;
                    if (stringPtr[sLength])
                    {
                        sLength += 1;
                    }
                    rdfData.rd_BufPointer = stringPtr;
                    goto BStrOutput;
                }
                case 'u':    //type->unsigned decimal number
                case 'U':
                {
                    MakeDecimal(&rdfData, FALSE, AdjustParam(&rdfData, flag));
                    goto FmtOutput;
                }
                case 'c':    //type->char
                {
                    rdfData.rd_BufPointer[0] = AdjustParam(&rdfData, flag);
                    rdfData.rd_BufPointer += 1;

          FmtOutput:
                    rdfData.rd_BufPointer[0] = 0;
                    rdfData.rd_BufPointer = (STRPTR)&rdfData.rd_Buffer;

          StrOutput:
                    sLength = -1;
                    STRPTR filler = rdfData.rd_BufPointer;
                    while (filler[0])
                    {
                        sLength -= 1;
                        filler += 1;
                    }
                    sLength = ~sLength;

         BStrOutput:
                    if ((!(rdfData.rd_TruncateNum)) || ((rdfData.rd_TruncateNum) && (sLength > rdfData.rd_TruncateNum)))
                    {
                        rdfData.rd_TruncateNum = sLength;
                    }

                    prependNum = prependNum - rdfData.rd_TruncateNum;
                    if (prependNum < 0)
                    {
                        prependNum = 0;
                    }

                    if (!(flag & RDFF_JUSTIFY))
                    {
                        PerformPad(&rdfData, flag, putchproc, prependNum);
                    }

                    if (rdfData.rd_TruncateNum)
                    {
                        for (int i = 0; i < rdfData.rd_TruncateNum; i++)
                        {
                            UBYTE currChar = rdfData.rd_BufPointer[0];
                            rdfData.rd_BufPointer += 1;

                            if (putchproc)
                            {
                                rdfData.rd_PutChData  = putchproc(rdfData.rd_PutChData, currChar);
                            }
                            else
                            {
                                STRPTR myData = (STRPTR)rdfData.rd_PutChData;
                                myData[0] = currChar;
                                rdfData.rd_PutChData = (APTR)((ULONG)rdfData.rd_PutChData + 1);
                            }
                        }
                    }

                    if (flag & RDFF_JUSTIFY)
                    {
                        PerformPad(&rdfData, flag, putchproc, prependNum);
                    }
                    break;
                }
                default:
                {
                    if (!(putchproc))
                    {
                        STRPTR myData = (STRPTR)rdfData.rd_PutChData;
                        myData[0] = currChar;
                        rdfData.rd_PutChData = (APTR)((ULONG)rdfData.rd_PutChData + 1);
                    }
                    else
                    {
                        rdfData.rd_PutChData = putchproc(rdfData.rd_PutChData, currChar);
                    }
                }
            }
        }
        else
        {
            if (!(putchproc))
            {
                STRPTR myData = (STRPTR)rdfData.rd_PutChData;
                myData[0] = currChar;
                rdfData.rd_PutChData = (APTR)((ULONG)rdfData.rd_PutChData + 1);
            }
            else
            {
                rdfData.rd_PutChData = putchproc(rdfData.rd_PutChData , currChar);
            }
        }
    }

    if (!(putchproc))
    {
        STRPTR myData = (STRPTR)rdfData.rd_PutChData;
        myData[0] = currChar;
        putchdata = (APTR)((ULONG)rdfData.rd_PutChData + 1);
    }
    else
    {
        rdfData.rd_PutChData = putchproc(rdfData.rd_PutChData , currChar);
    }

    return rdfData.rd_DataStream;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myPutPublicMsgPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") STRPTR portname, __reg("r5") struct Message* message)
{
    struct DebugArgs args;
    args.db_Function = 63 | (2<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)portname;
    args.db_Arg[1] = (ULONG)message;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    LONG status = PUBMSG_SUCCESS;

    myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

    struct MsgPortPPC* myPort = myFindPortPPC(PowerPCBase, portname);

    if (myPort)
    {
        myPutMsgPPC(PowerPCBase, myPort, message);
    }
    else
    {
        status = PUBMSG_NOPORT;
    }

    myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

    args.db_Arg[0] = status;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

    return status;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myAddUniquePortPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgPortPPC* port)
{
    struct DebugArgs args;
    args.db_Function = 64 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)port;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);

	struct MsgPortPPC* myPort;
	LONG result;

	if (myPort = myFindPortPPC(PowerPCBase, port->mp_Port.mp_Node.ln_Name))
	{
		result = UNIPORT_NOTUNIQUE;
	}
	else
	{
		result = UNIPORT_SUCCESS;
		myAddPortPPC(PowerPCBase, port);
	}
	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);
		
    args.db_Arg[0] = result;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return result;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION LONG myAddUniqueSemaphorePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    struct DebugArgs args;
    args.db_Function = 65 | (1<<8) | (1<<16);
    args.db_Arg[0] = (ULONG)SemaphorePPC;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	myObtainSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);

	struct SignalSemaphorePPC* mySem;
	LONG result;

	if (mySem = myFindSemaphorePPC(PowerPCBase, SemaphorePPC->ssppc_SS.ss_Link.ln_Name))
	{
		result = UNISEM_NOTUNIQUE;
	}
	else
	{
		result = UNISEM_SUCCESS;
		myAddSemaphorePPC(PowerPCBase, SemaphorePPC);
	}
	myReleaseSemaphorePPC(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);
	
    args.db_Arg[0] = result;
    printDebug(PowerPCBase, (struct DebugArgs*)&args);

	return result;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION BOOL myIsExceptionMode(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    BOOL status = PowerPCBase->pp_ExceptionMode;
    return status;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myNewListPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct List* list)
{
	list->lh_TailPred = (struct Node*)list;
	list->lh_Tail = 0;
	list->lh_Head = (struct Node*)((ULONG)list+4);

	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myCauseInterrupt(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    if (!(PowerPCBase->pp_ExceptionMode))
    {
        ULONG key = mySuper(PowerPCBase);

        PowerPCBase->pp_ExceptionMode = -1;

		setDEC(50);

        myUser(PowerPCBase, key);

        while (PowerPCBase->pp_ExceptionMode);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myAllocPrivateMem(void)
{
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myFreePrivateMem(void)
{
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myResetPPC(void)
{
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION VOID myFreeAllMem(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION ULONG myRun68KLowLevel(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG Code, __reg("r5") LONG Offset, __reg("r6") ULONG a0,
                                 __reg("r7") ULONG a1, __reg("r8") ULONG d0, __reg("r9") ULONG d1)
{
	struct MsgFrame* myFrame = CreateMsgFramePPC(PowerPCBase);

	myFrame->mf_Message.mn_Node.ln_Type = NT_MESSAGE;
	myFrame->mf_Message.mn_Length = sizeof(struct MsgFrame);
	myFrame->mf_Identifier = ID_LL68;
	myFrame->mf_PPCTask = PowerPCBase->pp_ThisPPCProc;
	myFrame->mf_PPCArgs.PP_Regs[0] = Code;
	myFrame->mf_PPCArgs.PP_Regs[1] = Offset;
	myFrame->mf_PPCArgs.PP_Regs[2] = a0;
	myFrame->mf_PPCArgs.PP_Regs[3] = a1;
	myFrame->mf_PPCArgs.PP_Regs[4] = d0;
	myFrame->mf_PPCArgs.PP_Regs[5] = d1;
	myFrame->mf_Arg[0] = 0;
	myFrame->mf_Arg[1] = 0;
	myFrame->mf_Arg[2] = 0;

	SendMsgFramePPC(PowerPCBase, myFrame);

	ULONG result = myWaitFor68K(PowerPCBase, (struct PPCArgs*)&myFrame->mf_PPCArgs);

	return result;
}


/********************************************************************************************
*
*	warp.library address-space functions.
*
*	These are warp.library LVOs, not powerpc.library ones: -276 CalcTableSize,
*	-282 AddAddrSpace, -384 RemoveAddrSpace and -402 ReadRC, as listed in
*	warpos/hal/warp_lib.i. iFUSION reaches them from PPC code with WarpBase in
*	r3, as "lwz r0,LVO+2(r3) / mtlr r0 / blrl", so they are PPC functions
*	installed in the dummy warp.library's jump table.
*
*	Reference implementations are in warpos/hal/warp.p.
*
*********************************************************************************************/

PPCFUNCTION ULONG myCalcTableSize(__reg("r3") struct WarpBase* WarpBase, __reg("r4") ULONG size)
{
	/* warp.p:1577. Round the requested address space up to a power of two,
	 * floor it at 8 MB, then divide by 128. The original gets the power of two
	 * with cntlzw and a rotate; doubling up from the floor gives the same
	 * answer without needing a builtin. */

	ULONG tableSize = 8 * 1024 * 1024;

	while ((tableSize < size) && (tableSize < 0x80000000))
	{
		tableSize <<= 1;
	}

	WarpBase->wb_CallCount[0] += 1;

	return (tableSize >> 7);
}

/********************************************************************************************/

PPCFUNCTION VOID myAddAddrSpace(__reg("r3") struct WarpBase* WarpBase, __reg("r4") APTR pageTable,
								__reg("r5") ULONG start, __reg("r6") ULONG end,
								__reg("r7") ULONG physAddr, __reg("r8") ULONG protMode,
								__reg("r9") ULONG cacheMode, __reg("r10") ULONG tableSize)
{
	/* NOT IMPLEMENTED YET. warp.p:1608 runs the real work in supervisor mode
	 * through RunSuperVisor, and AddAddressSpace (warp.p:2876) is close to four
	 * hundred lines of page table manipulation.
	 *
	 * The vector exists so a call lands here rather than in whatever an
	 * unextended jump table happened to hold, and the counter records that it
	 * happened. Static analysis of iFUSION found no reachable caller for this,
	 * while its author says it is required; this is what settles that. */

	WarpBase->wb_CallCount[1] += 1;

	return;
}

/********************************************************************************************/

PPCFUNCTION VOID myRemoveAddrSpace(__reg("r3") struct WarpBase* WarpBase, __reg("r4") APTR pageTable,
								   __reg("r5") ULONG start, __reg("r6") ULONG end,
								   __reg("r7") ULONG tableSize, __reg("r8") ULONG vSpace)
{
	/* NOT IMPLEMENTED YET. See myAddAddrSpace. The real one is warp.p:2415,
	 * with RemoveAddressSpace at warp.p:3267. */

	WarpBase->wb_CallCount[2] += 1;

	return;
}

/********************************************************************************************/

PPCFUNCTION ULONG myReadRC(__reg("r3") struct WarpBase* WarpBase, __reg("r4") APTR pageTable,
						   __reg("r5") ULONG tableSize, __reg("r6") APTR logicalAddr,
						   __reg("r7") ULONG vSpace)
{
	/* THE FOUR ARGUMENTS OF warp.p's In: block (:2533-2537): the page table,
	 * its size, the logical address and the virtual space id, -1 for the
	 * current one. Its signature line (:2529) says base + 3, which this used
	 * to follow; myRemoveAddrSpace takes its vSpace the same way. FIXES.md
	 * "#4 (WAS ITEM 12), ReadRC's ARGUMENTS", 1 Nov 2026.
	 *
	 * NOT IMPLEMENTED YET. The real one is warp.p:2542, calling ReadRCInfo at
	 * warp.p:3370. Returns zero for now. */

	WarpBase->wb_CallCount[3] += 1;

	return 0;
}
