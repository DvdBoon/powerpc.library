// Copyright (c) 2019-2021, 2024 Dennis van der Boon
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
#include <proto/powerpc.h>
#include <powerpc/powerpc.h>
#include "constants.h"
#include "libstructs.h"
#include "Internalsppc.h"
#include "prirule.h"
#pragma pack(pop)

/********************************************************************************************
*
*	The other half of EXCATTR_TIME, FIXES.md item 2.
*
*********************************************************************************************/

/*
 * SHORTEN THE DECREMENTER FOR THE SOONEST TIMED HANDLER, powerpc.p:5248-5308,
 * under the original's own comment:
 *
 *     ;  Check, if our DEC is too high for any
 *     ;  decrementer exception handler and correct the
 *     ;  DEC value
 *
 * excTimedDue() already defers a handler until its deadline has passed, so a
 * timed handler fires at the first decrementer tick AFTER its deadline. With
 * the slice at 20 ms that is up to 20 ms late, and under load with the quantum
 * at 1.5 ms it is less late but still arbitrary. This is what makes it fire
 * NEAR the deadline: the next decrementer is pulled forward to the earliest
 * deadline any handler is waiting on.
 *
 * The two halves share one field. pp_Quantum is PPC_DELAY, a running MINIMUM
 * of every pending deadline -- the scheduler already narrows it to the
 * incoming task's remaining slice (SwitchPPC, powerpc.p:4973-4980), and this
 * narrows it again for the decrementer list. Whichever is sooner wins.
 *
 * THE 64-BIT COMPARE IS THE ORIGINAL'S, INCLUDING ITS READ ORDER (:5292-5299):
 *
 *     mftbl  r7 / subfc  r0,r7,r0      ; low:  deadline_lo - now_lo, carry out
 *     mftbu  r7 / subfe. r3,r7,r3      ; high: deadline_hi - now_hi - borrow
 *     bgt .decloop                     ; high > 0  -- far off, leave the DEC
 *     beq .deceq                       ; high == 0 -- the low word is the wait
 *     li  r0,0                         ; high < 0  -- deadline passed, fire now
 *
 * TBL is read BEFORE TBU, so a TBL wrap between the two reads gives a high
 * word one too large and the handler is skipped for this one tick. That race
 * is the original's and is reproduced rather than fixed: at bus/4 the window
 * is a couple of instructions in 2^32 ticks, and "fires one tick later" is the
 * behaviour excTimedDue already has.
 *
 * MINDEC IS TAKEN RAW, unlike RESCH_DEFAULT and MIN_TICKS which item 18 had to
 * rescale by bus clock. Those two are durations the scheduling behaviour
 * depends on; MINDEC is a floor against programming the decrementer with
 * something that fires instantly or not at all -- 100 ticks is 6 us on a
 * 66 MHz bus and 4 us on a 100 MHz one, and nothing depends on which.
 *
 * A handler whose ed_TimeBaseLower already equals its deadline's low word is
 * skipped, exactly as at :5270-5272 and for the same reason excTimedDue skips
 * it: that deadline has been served and the handler is waiting for a new one.
 */
static ULONG decShorten(struct PrivatePPCBase* PowerPCBase, ULONG decValue)
{
    struct MinNode* node = PowerPCBase->pp_ExcDecrementer.mlh_Head;
    struct MinNode* next;

    while (next = node->mln_Succ)
    {
        struct ExcData* eData = (struct ExcData*)node;
        ULONG* deadline = (ULONG*)eData->ed_TimeBasePtr;

        if ((deadline) && (eData->ed_TimeBaseLower != deadline[1]))
        {
            ULONG nowLower = getTBL();
            ULONG waitLower = deadline[1] - nowLower;
            LONG  waitUpper = (LONG)(deadline[0] - getTBU()
                                     - ((deadline[1] < nowLower) ? 1 : 0));

            if (waitUpper <= 0)
            {
                if (waitUpper < 0)
                {
                    waitLower = 0;          /* the deadline has already passed */
                }
                if (waitLower < decValue)
                {
                    decValue = waitLower;
                }
            }
        }
        node = next;
    }

    if (decValue < MINDEC)
    {
        decValue = MINDEC;
    }

    return decValue;
}

/********************************************************************************************
*
*	Entry point after kernel.s. This directs to the correct exception code.
*
*********************************************************************************************/

PPCKERNEL void Exception_Entry(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct iframe* iframe)
{
    PowerPCBase->pp_ExceptionMode = -1;

    /*
     * pp_StdQuantum, NOT the running task's tp_Quantum. Reading
     * pp_ThisPPCProc->tp_Quantum here CRASHED THE LIBRARY DURING INIT: this
     * runs on every exception including the earliest ones in setup, where that
     * pointer can be stale or its structure not yet filled, and a fault inside
     * exception entry is fatal. The per-task quantum is applied in SwitchPPC
     * instead, where the incoming task is known good. FIXES.md item 18.
     */
    PowerPCBase->pp_Quantum = PowerPCBase->pp_StdQuantum;
    PowerPCBase->pp_CPUSDR1 = getSDR1();
    PowerPCBase->pp_CPUHID0 = getHID0();

    if ((PowerPCBase->pp_DeviceID != DEVICE_MPC8343E) && (PowerPCBase->pp_DeviceID != DEVICE_MPC8314E))
    {
       PowerPCBase->pp_L2State = getL2State();
    }

    ULONG ExceptionVector = iframe->if_Context.ec_ExcID - (PPC_VECLEN * OPCODE_LEN);
    iframe->if_ExcNum = ExceptionVector >> 8;
    iframe->if_Context.ec_ExcID = 1 << iframe->if_ExcNum;

    switch (ExceptionVector)
    {
        case VEC_EXTERNAL:
        {
            switch (PowerPCBase->pp_DeviceID)
	        {
		        case DEVICE_HARRIER:
		        {
			        if (readmemLongPPC(PPC_XCSR_BASE, XCSR_FEST) & XCSR_FEST_MIM0)
                    {
                        writememLongPPC(PPC_XCSR_BASE, XCSR_FECL, XCSR_FECL_MIM0);
                        CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcInterrupt);
                    }

                    if (readmemLongPPC(PowerPCBase->pp_BridgeMPIC, XMPI_P0IAC) == 0xff)
                    {
                        PowerPCBase->pp_ExceptionMode = 0;
                        setDEC(decShorten(PowerPCBase, PowerPCBase->pp_Quantum));
                        return;
                    }

                    struct MsgFrame* msgFrame;

                    while (msgFrame = KGetMsgFramePPC(PowerPCBase))
                    {
                        AddTailPPC((struct List*)&PowerPCBase->pp_MsgQueue , (struct Node*)msgFrame);
                    }
                    writememLongPPC(PowerPCBase->pp_BridgeMPIC, XMPI_P0EOI, 0);
                    break;
		        }

		        case DEVICE_MPC8343E:
		        {
                    if (loadPCI(IMMR_ADDR_DEFAULT, IMMR_IMISR) & IMMR_IMISR_IDI)
                    {
                        storePCI(IMMR_ADDR_DEFAULT, IMMR_IDR, 1);
                        CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcInterrupt);
                    }
                    struct MsgFrame* msgFrame;

                    while (msgFrame = KGetMsgFramePPC(PowerPCBase))
                    {
                        AddTailPPC((struct List*)&PowerPCBase->pp_MsgQueue, (struct Node*)msgFrame);
                    }

                    if (loadPCI(IMMR_ADDR_DEFAULT, IMMR_IMISR) & IMMR_IMISR_IM0I)
                    {
                        storePCI(IMMR_ADDR_DEFAULT, IMMR_IMISR, IMMR_IMISR_IM0I);
                    }
                    break;
		        }
                case DEVICE_MPC8314E: //CausePPCInterrupt should be handled here
                {
                    struct MsgFrame* msgFrame;

                    while (msgFrame = KGetMsgFramePPC(PowerPCBase))
                    {
                        AddTailPPC((struct List*)&PowerPCBase->pp_MsgQueue, (struct Node*)msgFrame);
                    }

                    writememLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_IMBCR, 0);
                    writememLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_CSMISR, readmemLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_CSMISR));
                    break;
                }
		        case DEVICE_MPC107:
		        {
                    if (loadPCI(PPC_EUMB_BASE, MPC107_IMISR) & MPC107_IMISR_IM0I)
                    {
                        storePCI(PPC_EUMB_BASE, MPC107_IMISR, (MPC107_IMISR_IM0I | MPC107_IMISR_IM1I));
                        CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcInterrupt);
                    }

                    if ((readmemLongPPC(PPC_EUMB_EPICPROC, EPIC_IACK) >> 24) == 0xff)
                    {
                        PowerPCBase->pp_ExceptionMode = 0;
                        setDEC(decShorten(PowerPCBase, PowerPCBase->pp_Quantum));
                        return;
                    }
                    struct MsgFrame* msgFrame;

                    while (msgFrame = KGetMsgFramePPC(PowerPCBase))
                    {
                        AddTailPPC((struct List*)&PowerPCBase->pp_MsgQueue, (struct Node*)msgFrame);
                    }
                    storePCI(PPC_EUMB_BASE, MPC107_IMISR, MPC107_IMISR_IPQI);
                    writememLongPPC(PPC_EUMB_EPICPROC, EPIC_EOI, 0);
			        break;
		        }
	        }

            CPUStats(PowerPCBase);

            ULONG currAddress = iframe->if_Context.ec_UPC.ec_SRR0;
            if ((PowerPCBase->pp_LowerLimit <= currAddress) && (currAddress < PowerPCBase->pp_UpperLimit))
            {
                PowerPCBase->pp_Quantum = 0x1000;
                break;
            }

            if ((PowerPCBase->pp_Mutex) || (PowerPCBase->pp_FlagForbid))
            {
                PowerPCBase->pp_Quantum = 0x1000;
                break;
            }

            struct TaskPPC* currTask = PowerPCBase->pp_ThisPPCProc;

            if (currTask)
            {
                if (currTask->tp_Task.tc_State == TS_ATOMIC)
                {
                    break;
                }
            }

            HandleMsgs(PowerPCBase);
            TaskCheck(PowerPCBase);
            SwitchPPC(PowerPCBase, iframe);
            break;
        }
        case VEC_DECREMENTER:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcDecrementer);

            struct ExcData* eData;

            while (eData = (struct ExcData*)RemHeadPPC((struct List*)&PowerPCBase->pp_ReadyExc))
            {
                if (eData->ed_ExcID & EXCF_MCHECK)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcMCheck, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_DACCESS)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcDAccess, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_IACCESS)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcIAccess, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_INTERRUPT)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcInterrupt, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_ALIGN)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcAlign, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_PROGRAM)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcProgram, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_FPUN)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcFPUn, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_DECREMENTER)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcDecrementer, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_SC)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcSystemCall, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_TRACE)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcTrace, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_PERFMON)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcPerfMon, (struct Node*)eData);
                if (eData->ed_ExcID & EXCF_IABR)
                    EnqueuePPC((struct List*)&PowerPCBase->pp_ExcIABR, (struct Node*)eData);
                eData->ed_Flags |= EXCF_ACTIVE;
            }

            while (eData = (struct ExcData*)RemHeadPPC((struct List*)&PowerPCBase->pp_RemovedExc))
            {
                ULONG nodeAddr = (ULONG)&eData->ed_LastExc;
                for (int i=0; i<12; i++)
                {
                    nodeAddr += 4;
                    ULONG actNode = *((ULONG*)(nodeAddr));
                    if (actNode)
                    {
                        RemovePPC((struct Node*)actNode);
                    }
                }
                struct ExcData* lastExc = eData->ed_LastExc;
                lastExc->ed_Flags &= ~EXCF_ACTIVE;
            }

            CPUStats(PowerPCBase);

            ULONG currAddress = iframe->if_Context.ec_UPC.ec_SRR0;
            if ((PowerPCBase->pp_LowerLimit <= currAddress) && (currAddress < PowerPCBase->pp_UpperLimit))
            {
                PowerPCBase->pp_Quantum = 0x1000;
                break;
            }

            if ((PowerPCBase->pp_Mutex) || (PowerPCBase->pp_FlagForbid))
            {
                PowerPCBase->pp_Quantum = 0x1000;
                break;
            }

            struct TaskPPC* currTask = PowerPCBase->pp_ThisPPCProc;

            if (currTask)
            {
                if (currTask->tp_Task.tc_State == TS_ATOMIC)
                {
                    break;
                }
            }

            HandleMsgs(PowerPCBase);
            TaskCheck(PowerPCBase);
            SwitchPPC(PowerPCBase, iframe);
            break;
        }
        case VEC_DATASTORAGE:
        {
            if (PowerPCBase->pp_EnDAccessExc)  //not standard WOS behaviour
            {
                CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcDAccess);
            }
            if ((iframe->if_Context.ec_DAR == 4) || ((iframe->if_Context.ec_DAR < 0xfffff800) && (iframe->if_Context.ec_DAR > 0x800)))
            {
                if (iframe->if_Context.ec_UPC.ec_SRR0 > 0x10000)
                {
                    if(!(PowerPCBase->pp_DataExcLow += 1))
                    {
                        PowerPCBase->pp_DataExcHigh += 1;
                    }
                    struct DataMsg myData;
                    ULONG Status = DoDataStore(iframe, iframe->if_Context.ec_UPC.ec_SRR0, &myData);
                    if (Status)
                    {
                        struct MsgFrame* myFrame = KCreateMsgFramePPC(PowerPCBase);
                        myFrame->mf_Identifier = myData.dm_Type;
                        myFrame->mf_Arg[1] = myData.dm_Address;
                        myFrame->mf_Arg[0] = myData.dm_Value;
                        KSendMsgFramePPC(PowerPCBase, myFrame);
                        if (!(myData.dm_LoadFlag))
                        {
                            while (myFrame->mf_Identifier != ID_DONE);
#ifdef DEBUG
                            if (myFrame->mf_Arg[0] == ERR_EMEM)
                            {
                                CommonExcError(PowerPCBase, iframe);
                                break;
                            }
#endif
                            if (!(FinDataStore(myFrame->mf_Arg[0], iframe, iframe->if_Context.ec_UPC.ec_SRR0, &myData)))
                            {
                                CommonExcError(PowerPCBase, iframe);
                                break;
                            }
                        }
                        iframe->if_Context.ec_UPC.ec_SRR0  += 4;
                        //PowerPCBase->pp_Quantum = getDEC() + 1000; //Do not start quantum all over
                        break;
                    }
                }
            }
            else if (PowerPCBase->pp_PowerPCBase.PPC_Flags & LIB_FLAGPPC_NZG)
            {
                if (iframe->if_Context.ec_UPC.ec_SRR0 > 0x10000)
                {
                    if(!(PowerPCBase->pp_DataExcLow += 1))
                    {
                        PowerPCBase->pp_DataExcHigh += 1;
                    }
                    struct DataMsg myData;
                    ULONG Status = DoDataStore(iframe, iframe->if_Context.ec_UPC.ec_SRR0, &myData);
                    if (Status)
                    {
                        myData.dm_Address = 0x3c1000;
                        myData.dm_Value   = 0;
                        if (!(myData.dm_LoadFlag))
                        {
                            if (!(FinDataStore(0, iframe, iframe->if_Context.ec_UPC.ec_SRR0, &myData)))
                            {
                                CommonExcError(PowerPCBase, iframe);
                                break;
                            }
                        }
                        iframe->if_Context.ec_UPC.ec_SRR0  += 4;
                        //PowerPCBase->pp_Quantum = getDEC() + 1000; //Do not start quantum all over
                        break;
                    }
                }
            }
            CommonExcError(PowerPCBase, iframe);
            break;
        }
        case VEC_PROGRAM:
        {
            if (iframe->if_Context.ec_GPR[4] == SUPERKEY)
            {
                iframe->if_Context.ec_UPC.ec_SRR0  += 4;
                iframe->if_Context.ec_SRR1         &= ~PSL_PR;
                iframe->if_Context.ec_GPR[3]        = 0;            //Set SuperKey
                //PowerPCBase->pp_Quantum = getDEC() + 1000;
            }
            else
            {
                CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcProgram);
            }
            break;
        }
        case VEC_ALIGNMENT:
        {

            if ((PowerPCBase->pp_EnAlignExc) || (iframe->if_Context.ec_DAR < 0x200000))
            {
                CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcAlign);
            }
            else
            {
                if (DoAlign(iframe, iframe->if_Context.ec_UPC.ec_SRR0))
                {
                    iframe->if_Context.ec_UPC.ec_SRR0  += 4;
                    if (!(PowerPCBase->pp_AlignmentExcLow += 1))
                    {
                        PowerPCBase->pp_AlignmentExcHigh += 1;
                    }
                    //PowerPCBase->pp_Quantum = getDEC() + 1000;
                }
                else
                {
                    CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcAlign);
                }
            }
            break;
        }
        case VEC_ALTIVECUNAV:
        {
            iframe->if_Context.ec_ExcID = EXCF_ALTIVECUNAV;
            iframe->if_ExcNum = EXCB_ALTIVECUNAV;
            if (PowerPCBase->pp_EnAltivec)
            {
                iframe->if_Context.ec_SRR1 |= PSL_VEC;
            }
            else
            {
                CommonExcError(PowerPCBase, iframe);
            }
            break;
        }
        case VEC_MACHINECHECK:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcMCheck);
            break;
        }
        case VEC_INSTSTORAGE:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcIAccess);
            break;
        }
        case VEC_FPUNAVAILABLE:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcFPUn);
            break;
        }
        case VEC_SYSTEMCALL:
        {
            ULONG scReg = iframe->if_Context.ec_GPR[3];

            /*
             * r3 is a struct SysCall* only for OUR callers. The sc instruction
             * is not ours alone: iFUSION issues it with 'HOST'/'SERV' in r3/r4
             * and a selector in r5, and dereferencing that as a pointer faults
             * inside the exception handler, which is fatal.
             *
             * So anything that is not plainly one of ours is handed to the
             * registered handlers untouched, without being read through. The
             * magic pair is recognised explicitly because it is a known
             * caller; the null and alignment tests catch the obvious rest.
             * This is not a full validity check -- an arbitrary aligned
             * non-null value would still be dereferenced below -- but it
             * removes the case that is known to occur.
             */
            if ((scReg == 0) || (scReg & 3) ||
                ((scReg == HOSTSERV_MAGIC_R3) &&
                 (iframe->if_Context.ec_GPR[4] == HOSTSERV_MAGIC_R4)))
            {
                CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcSystemCall);
                break;
            }

            struct SysCall* sc = (struct SysCall*)scReg;

            switch (sc->sc_Function)
            {
                case SC_CREATEMSG:
                {
                    iframe->if_Context.ec_GPR[3] = (ULONG)KCreateMsgFramePPC(PowerPCBase);
                    break;
                }
                case SC_GETMSG:
                {
                    iframe->if_Context.ec_GPR[3] = (ULONG)KGetMsgFramePPC(PowerPCBase);
                    break;
                }
                case SC_SENDMSG:
                {
                    KSendMsgFramePPC(PowerPCBase, (struct MsgFrame*)sc->sc_Arg[0]);
                    break;
                }
                case SC_FREEMSG:
                {
                    KFreeMsgFramePPC(PowerPCBase, (struct MsgFrame*)sc->sc_Arg[0]);
                    break;
                }
                case SC_SETCACHE:
                {
                    KSetCache(PowerPCBase, sc->sc_Arg[0], (APTR)sc->sc_Arg[1], sc->sc_Arg[2]);
                    break;
                }
                default:
                {
                    CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcSystemCall);
                    break;
                }
            }
            break;
        }
        case VEC_TRACE:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcTrace);
            break;
        }
        case VEC_PERFMONITOR:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcPerfMon);
            break;
        }
        case VEC_IBREAKPOINT:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcIABR);
            break;
        }
        default:
        {
            CommonExcError(PowerPCBase, iframe);
        }
    }
    PowerPCBase->pp_ExceptionMode = 0;
    setDEC(decShorten(PowerPCBase, PowerPCBase->pp_Quantum));
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCFUNCTION void TaskCheck(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    ULONG mask;
    struct TaskPPC* currTask = PowerPCBase->pp_ThisPPCProc;

    if(currTask)
    {
        if ((PowerPCBase->pp_TaskExcept) && (PowerPCBase->pp_TaskExcept == currTask) &&
            (currTask->tp_Task.tc_ExceptData) && (mask = currTask->tp_Task.tc_SigRecvd & currTask->tp_Task.tc_SigExcept))
        {
            currTask->tp_Task.tc_SigRecvd &= ~mask;
            currTask->tp_Task.tc_SigExcept &= ~mask;

            ULONG (*ExcHandler)(__reg("r2") ULONG, __reg("r3") ULONG) = currTask->tp_Task.tc_ExceptCode;
            ULONG tempR2 = getR2();
            ULONG signal = ExcHandler((ULONG)currTask->tp_Task.tc_ExceptData, mask);
            storeR2(tempR2);

            currTask->tp_Task.tc_SigExcept |= signal;
        }
    }

    if (!(PowerPCBase->pp_FlagWait))
    {
        struct WaitTime* currWait = (struct WaitTime*)PowerPCBase->pp_WaitTime.mlh_Head;
        struct WaitTime* nxtWait;

        while (nxtWait = (struct WaitTime*)currWait->wt_Node.ln_Succ)
        {
            ULONG tbu, tbl, lo;
            LONG hi;
            LONG expired = 0;

            /*
             * THE TIMEBASE IS READ WITH A RETRY, as powerpc.p:4118 does it:
             *
             *     .retry  mftbu r5 / mftbl r6 / mftbu r0
             *             cmplw r0,r5 / bne .retry
             *
             * This was "wt_TimeUpper < getTBU() || (wt_TimeUpper == getTBU()
             * && wt_TimeLower < getTBL())" -- three separate reads inside one
             * expression, with no retry, so a TBL wrap between them could
             * skip an entry for that pass. myWaitTime itself already reads
             * the timebase with a retry loop when it COMPUTES the deadline
             * (functionsppc.c:2254); only the comparison was careless.
             */
            while (1)
            {
                tbu = getTBU();
                tbl = getTBL();
                if (tbu == getTBU())
                {
                    break;
                }
            }

            /*
             * A REAL 64-BIT SUBTRACT, "now - deadline", as powerpc.p:4124
             * does with subfc/subfe. -- not two independent comparisons.
             */
            lo = tbl - currWait->wt_TimeLower;
            hi = (LONG)(tbu - currWait->wt_TimeUpper
                        - ((tbl < currWait->wt_TimeLower) ? 1 : 0));

            if (hi >= 0)
            {
                expired = 1;                    /* deadline is behind us */
            }
            else if ((hi == -1) && ((LONG)lo <= 0))
            {
                /*
                 * "(LONG)lo <= 0" IS THE ORIGINAL'S "tstw r6 / bgt .settimer",
                 * powerpc.p:4129-4130, and the first version of this rewrite
                 * left it out. Without it a deadline between 2^31 and 2^32
                 * ticks ahead -- hi == -1 with a positive low word, about
                 * 86 to 172 seconds here -- passed the DEVIATION test below
                 * as if it were marginally early and fired immediately.
                 */
                /*
                 * DEVIATION, powerpc.p:32 = 100 and :4131 "cmpwi r6,-DEVIATION
                 * / bge .signal". A deadline within 100 ticks counts as
                 * reached, so an entry that is marginally early is not left
                 * for another pass -- which is how a timer gets reported as
                 * never arriving.
                 */
                if ((LONG)lo >= -DEVIATION)
                {
                    expired = 1;
                }
            }

            if (expired)
            {
                if ((currTask) && (currTask == currWait->wt_Task))
                {
                    currWait->wt_Task->tp_Task.tc_State = TS_RUN;
                }
                else
                {
                    currWait->wt_Task->tp_Task.tc_State = TS_READY;
                }
                currWait->wt_Task->tp_Task.tc_SigRecvd |= SIGF_WAIT;
                RemovePPC((struct Node*)currWait);
            }
            else if (hi == -1)
            {
                /*
                 * .settimer, powerpc.p:4134: narrow PPC_DELAY to the time
                 * still to run, so the NEXT decrementer lands when this timer
                 * is due. Ours never did this, so an expiry was only noticed
                 * when an exception happened for some other reason.
                 *
                 * pp_Quantum IS PPC_DELAY (see the note at the top of this
                 * file) and Exception_Entry resets it to pp_StdQuantum each
                 * pass, so narrowing it here is per-pass and cannot
                 * accumulate. Only the hi == -1 case is used: that is the
                 * only one where the low word is a meaningful remaining
                 * count, and the original's path for a far-future deadline
                 * negates a value that is not one.
                 */
                ULONG remaining = (ULONG)(-((LONG)lo));

                if (remaining < PowerPCBase->pp_Quantum)
                {
                    PowerPCBase->pp_Quantum = remaining;
                }
            }
            currWait = nxtWait;
        }
    }

    struct TaskPPC* currWTask = (struct TaskPPC*)PowerPCBase->pp_WaitingTasks.mlh_Head;
    struct TaskPPC* nxtWTask;

    while (nxtWTask = (struct TaskPPC*)currWTask->tp_Task.tc_Node.ln_Succ)
    {
        if ((currWTask->tp_Task.tc_State == TS_READY) || (currWTask->tp_Task.tc_SigWait & currWTask->tp_Task.tc_SigRecvd))
        {
            currWTask->tp_Task.tc_State = TS_READY;
            RemovePPC((struct Node*)currWTask);
            //AddTailPPC((struct List*)&PowerPCBase->pp_ReadyTasks, (struct Node*)currWTask); //Enqueue on pri?
            EnqueuePPC((struct List*)&PowerPCBase->pp_ReadyTasks, (struct Node*)currWTask);
        }
        currWTask = nxtWTask;
    }
    return;
}

/********************************************************************************************
*
*	This function dispatches new PPC tasks, switches between tasks and redirect signals.
*
*********************************************************************************************/

PPCKERNEL void HandleMsgs(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    struct MsgFrame* currMsg = (struct MsgFrame*)PowerPCBase->pp_MsgQueue.mlh_Head;
    struct MsgFrame* nxtMsg;
    while (nxtMsg = (struct MsgFrame*)currMsg->mf_Message.mn_Node.ln_Succ)
    {
        switch (currMsg->mf_Identifier)
        {
            case ID_TPPC:
            case ID_DONE:
            case ID_END:
            case ID_DNLL:
            {
                struct TaskPPC* myTask = currMsg->mf_PPCTask;
                if ((currMsg->mf_Identifier == ID_TPPC) && (!(myTask)))
                {
                    RemovePPC((struct Node*)currMsg);
                    AddTailPPC((struct List*)&PowerPCBase->pp_NewTasks, (struct Node*)currMsg);
                }
                else
                {
                    struct MsgPortPPC* myPort = myTask->tp_Msgport;
                    if (myPort->mp_Semaphore.ssppc_SS.ss_QueueCount == -1)
                    {
                        RemovePPC((struct Node*)currMsg);

                        ULONG signal = 1 << (myPort->mp_Port.mp_SigBit);

                        /* queue first, signal second -- the same order as the
                         * ID_RXPPC and ID_XPPC handlers below, FIXES.md item
                         * 20; this site had been left the other way round */
                        AddTailPPC(&myPort->mp_Port.mp_MsgList, (struct Node*)currMsg);
                        myTask->tp_Task.tc_SigRecvd |= signal;

                        if (currMsg->mf_Identifier == ID_DONE)
                        {
                            /*
                             * ASSIGN the mirror's mask over this task's, as
                             * the original does at powerpc.p:14975-14976 and
                             * :15711-15712 ("lwz XPP_SIGALLOC / stw
                             * TC_SIGALLOC") and Sonnet at ppcfunctions.p:
                             * 10334-10335. The 68k side assigns the other way
                             * on every T68K, keeping its port bit; see
                             * functions68k.c. An OR here ratcheted; nothing
                             * here collided.
                             */
                            myTask->tp_Task.tc_SigAlloc = currMsg->mf_Arg[1];
                        }
                        if (myTask == PowerPCBase->pp_ThisPPCProc)
                        {
                            myTask->tp_Task.tc_State = TS_RUN;
                        }
                        else
                        {
                            myTask->tp_Task.tc_State = TS_READY;
                        }
                    }
                }
                break;
            }
            case ID_XMSG:
            {
                struct MsgPortPPC* myPort = (struct MsgPortPPC*)currMsg->mf_Message.mn_ReplyPort;
                if (myPort->mp_Semaphore.ssppc_SS.ss_QueueCount != -1)
                {
                    break;
                }

                struct MsgFrame* oldMsg = (struct MsgFrame*)currMsg->mf_Arg[0];

                RemovePPC((struct Node*)currMsg);
                KFreeMsgFramePPC(PowerPCBase, currMsg);

                myPort = (struct MsgPortPPC*)oldMsg->mf_Message.mn_ReplyPort;
                struct TaskPPC* sigTask = (struct TaskPPC*)myPort->mp_Port.mp_SigTask;

                if (!(sigTask))
                {
                    break;
                }

                ULONG signal = 1 << (myPort->mp_Port.mp_SigBit);

                /*
                 * QUEUE FIRST, SIGNAL SECOND -- FIXES.md item 20.
                 *
                 * This was the other way round. exec's PutMsg queues the
                 * message and then signals mp_SigTask, for the reason that
                 * makes this order mandatory rather than tidy: a woken task
                 * does GetMsg, and if the signal arrives before the message
                 * is linked it finds the port empty, consumes the signal and
                 * waits again. The message then sits on the port with
                 * nothing left to announce it.
                 *
                 * The 22 Oct sigdump ring caught exactly that state on both
                 * sides of a hung ScummVM: "1 MESSAGE(S) QUEUED, OWNER WAITS
                 * ON BIT n, AND THAT BIT IS NOT SET".
                 */
                AddTailPPC(&myPort->mp_Port.mp_MsgList, (struct Node*)oldMsg);
                sigTask->tp_Task.tc_SigRecvd |= signal;

                if (sigTask == PowerPCBase->pp_ThisPPCProc)
                {
                    sigTask->tp_Task.tc_State = TS_RUN;
                }
                else
                {
                    sigTask->tp_Task.tc_State = TS_READY;
                }
                break;
            }
            case ID_XPPC:
            {
                struct MsgPortPPC* myPort = (struct MsgPortPPC*)currMsg->mf_Arg[0];
                if (myPort->mp_Semaphore.ssppc_SS.ss_QueueCount != -1)
                {
                    break;
                }

                struct Node* xMsg = (struct Node*)currMsg->mf_Arg[1];

                RemovePPC((struct Node*)currMsg);
                KFreeMsgFramePPC(PowerPCBase, currMsg);

                struct TaskPPC* sigTask = (struct TaskPPC*)myPort->mp_Port.mp_SigTask;

                if (!(sigTask))
                {
                    break;
                }

                ULONG signal = 1 << (myPort->mp_Port.mp_SigBit);

                /* queue first, signal second -- FIXES.md item 20, same as
                 * the ID_RXPPC handler above, which carries the reasoning */
                AddTailPPC(&myPort->mp_Port.mp_MsgList, xMsg);
                sigTask->tp_Task.tc_SigRecvd |= signal;

                if (sigTask == PowerPCBase->pp_ThisPPCProc)
                {
                    sigTask->tp_Task.tc_State = TS_RUN;
                }
                else
                {
                    sigTask->tp_Task.tc_State = TS_READY;
                }
                break;
            }
            case ID_LLPP:
            {
                struct PrivateTask* myTask = (struct PrivateTask*)PowerPCBase->pp_ThisPPCProc;

                if ((myTask) && (myTask->pt_Mirror68K == (struct Task*)currMsg->mf_Arg[0]))
                {
                    myTask->pt_Task.tp_Task.tc_State = TS_RUN;
                    myTask->pt_Task.tp_Task.tc_SigRecvd |= currMsg->mf_Signals;
                }
                else
                {
                    struct PrivateTask* nxtTask;
                    struct PrivateTask* currTask = (struct PrivateTask*)PowerPCBase->pp_WaitingTasks.mlh_Head;
                    while (nxtTask = (struct PrivateTask*)currTask->pt_Task.tp_Task.tc_Node.ln_Succ)
                    {
                        if (currTask->pt_Mirror68K == (struct Task*)currMsg->mf_Arg[0])
                        {
                            currTask->pt_Task.tp_Task.tc_State = TS_READY;
                            currTask->pt_Task.tp_Task.tc_SigRecvd |= currMsg->mf_Signals;
                            break;
                        }
                        currTask = nxtTask;
                    }
                    if (!(nxtTask))
                    {
                        currTask = (struct PrivateTask*)PowerPCBase->pp_ReadyTasks.mlh_Head;
                        while (nxtTask = (struct PrivateTask*)currTask->pt_Task.tp_Task.tc_Node.ln_Succ)
                        {
                            if (currTask->pt_Mirror68K == (struct Task*)currMsg->mf_Arg[0])
                            {
                                currTask->pt_Task.tp_Task.tc_SigRecvd |= currMsg->mf_Signals;
                                break;
                            }
                            currTask = nxtTask;
                        }
                    }
                }
                RemovePPC((struct Node*)currMsg);
                KFreeMsgFramePPC(PowerPCBase, currMsg);
                break;
            }
            default:
            {
                RemovePPC((struct Node*)currMsg);
                KFreeMsgFramePPC(PowerPCBase, currMsg);
            }
        }
        currMsg = nxtMsg;
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

/*
 * tc_Switch and tc_Launch, FIXES.md item 10.
 *
 * struct TASKPPC embeds a 68k struct Task (the original declares it
 * "STRUCTURE TASKPPC,TC_SIZE"), so these are exec fields -- tc_Flags 0x0E,
 * tc_Switch 0x42, tc_Launch 0x46 -- driven by a PPC mechanism: WarpOS's own
 * PPC scheduler calls them from its own run queue, nothing on the 68k side
 * does. iFUSION installs a tc_Launch on its PPC task at 0x01BF64 and clears
 * TF_LAUNCH at 0x01F2F8, and until now this library called neither.
 *
 * The original gates them on TP_SWITCH and TP_LAUNCH (powerpc.p:4740, :5133),
 * names defined nowhere in the sources we have. They are bit POSITIONS, not
 * masks -- WarpOS spells positions _P and masks _F -- and iFUSION's
 * "li r5,0x80 / andc" at 0x01F2F8 pins TP_LAUNCH to exec's TF_LAUNCH, bit 7.
 * So these are exec's bits 6 and 7, and TF_SWITCH/TF_LAUNCH below are the
 * right masks. See the note beside them in constantsppc.i.
 *
 * THE r2 EACH ONE IS GIVEN IS THE ORIGINAL'S, and they differ:
 *
 *   tc_Switch   powerpc.p:4749 "mfsprg r2,3". The original keeps the kernel's
 *               own base in SPRG3 across the scheduler, so the hook runs with
 *               the LIBRARY's TOC. This library does not maintain SPRG3 that
 *               way -- _ExcCommon uses it for r0 and the LR -- so the
 *               equivalent is simply our own r2, which is what it already is.
 *               Restored afterwards in case the hook clobbers it.
 *   tc_Launch   powerpc.p:5141 "lwz r2,CONTEXT_GPR+__r2(r4)". The INCOMING
 *               TASK's saved r2, so the hook runs with the TOC of the task
 *               being launched. Taken from that task's own context memory.
 *
 * The __reg("r2") parameter plus getR2/storeR2 is the same pattern
 * CommonExcHandler uses for an EXCF_LARGECONTEXT handler below: declaring the
 * TOC as the first argument is what puts it in r2 across the call. Neither
 * hook takes arguments of its own -- the original sets up none, it just
 * "blrl".
 *
 * PLACEMENT IS ADAPTED, NOT IDENTICAL, AND THIS IS WHY. The original embeds
 * each call inside its own context save and restore: tc_Switch after the FPRs
 * are saved and before the non-volatile GPRs (powerpc.p:4736), tc_Launch
 * after the GPRs are restored and before SRR0/SRR1 (powerpc.p:5130). This
 * library splits those apart -- _ExcCommon and _StoreFrame have already saved
 * the outgoing context before any C runs, and _LoadFrame restores the
 * incoming one after it returns -- so neither of those points exists here.
 *
 * What is available, and what preserves the contract, is the end of SwitchPPC:
 * after the scheduling decision and before _LoadFrame installs the new
 * context. The ORDER is the original's, out before in. The gate is "the
 * running task actually changed", which is exact: DispatchPPC is called only
 * from inside SwitchPPC (:742 and :779) and both it and SwitchPPC assign
 * pp_ThisPPCProc, so one test catches every switch, including a dispatch,
 * exactly once -- and crucially does NOT fire on the paths where SwitchPPC
 * decides not to switch at all.
 */
static void callTaskHook(APTR code, ULONG toc)
{
    void (*hook)(__reg("r2") ULONG) = (void (*)(__reg("r2") ULONG))code;
    ULONG tempR2 = getR2();

    hook(toc);

    storeR2(tempR2);
}

/*
 * WHEN A TASK BEGINS RUNNING, powerpc.p:4969-4972 in the original's .switch:
 * "mftbl r5 / stw r5,TASKPPC_TIMESTAMP(r3)". tp_Timestamp is how the scheduler
 * knows how much CPU to charge the task when it next leaves -- the accounting
 * is per task and exact, not a share of the interval between passes.
 *
 * tp_Timestamp and tp_Timestamp2 ARE the scheduler's fields, and the comment
 * this library used to carry calling them "free space for debuggers" was wrong
 * -- tasksPPC.h says that of tp_DebugData at :39 only. The original reads and
 * writes both: tp_Timestamp at :4606, :4630, :4854, :4972 and :5417/:5425, and
 * tp_Timestamp2 at :4518, :4933 and :5434/:5439.
 */
static void priMarkRunning(struct TaskPPC* task)
{
    task->tp_Timestamp = getTBL();
}

/*
 * WHEN A TASK LEAVES THE CPU, powerpc.p:4853-4877 plus the NEWPRIRULE block at
 * :4884-4908.
 *
 *     lwz r3,TASKPPC_TIMESTAMP(r5) / mftbl r4 / subf r4,r3,r4   ; the delta
 *     tp_Elapsed      += delta
 *     tp_Totalelapsed += delta
 *     cmpw tp_Quantum,tp_Elapsed / bgt .notover / tp_Elapsed = 0
 *
 * so the slice counter is reset only when the quantum is actually used up --
 * a task that yields early keeps its remaining slice, which is what the launch
 * path's "quantum - elapsed" then programs the decrementer with.
 */
static void priLeaveRun(struct PrivatePPCBase* PowerPCBase, struct TaskPPC* task)
{
    ULONG delta = getTBL() - task->tp_Timestamp;

    task->tp_Elapsed += delta;
    task->tp_Totalelapsed += delta;

    if (task->tp_Quantum <= task->tp_Elapsed)
    {
        task->tp_Elapsed = 0;
    }

    priDecay(task);
    priLowerMinimum(PowerPCBase, task);
}

/*
 * THE PREEMPTION DECISION, powerpc.p:4596-4656. This library had none: on
 * every path where the running task was still runnable it took the ready head
 * unconditionally, so the ready list was a round-robin and tp_Priority,
 * tp_Prioffset and tp_Desired decided nothing. The original asks three
 * questions in this order.
 *
 *   1. IS THE READY HEAD ATOMIC (:4597-4600)? Then switch, whatever the
 *      priorities say.
 *   2. IS OUR QUANTUM SPENT (:4605-4612)? "elapsed + delta > quantum". If so
 *      switch when anybody is ready; and if NOBODY is ready, renew our own
 *      slice and take the decay anyway (:4616-4631) -- the task keeps the CPU
 *      but still pays for what it used, which is what stops a lone busy task
 *      from holding a high effective priority for ever.
 *   3. OTHERWISE switch only if the ready head's effective priority is
 *      STRICTLY greater than ours (:4643-4656, "cmpw r4,r5 / bgt
 *      .savecontext"). Equal priorities do not preempt; the running task
 *      keeps the CPU until its quantum expires.
 */
static LONG priShouldSwitch(struct PrivatePPCBase* PowerPCBase, struct TaskPPC* curr)
{
    struct TaskPPC* head = (struct TaskPPC*)PowerPCBase->pp_ReadyTasks.mlh_Head;
    LONG  ready = !priListEmpty(&PowerPCBase->pp_ReadyTasks);
    ULONG now   = getTBL();
    ULONG delta = now - curr->tp_Timestamp;

    if ((ready) && (head->tp_Flags & TASKPPCF_ATOMIC))
    {
        return 1;
    }

    if ((curr->tp_Elapsed + delta) > curr->tp_Quantum)
    {
        if (ready)
        {
            return 1;
        }

        curr->tp_Totalelapsed += delta;
        priDecay(curr);
        curr->tp_Timestamp = now;
        curr->tp_Elapsed = 0;
        return 0;
    }

    if (!ready)
    {
        return 0;
    }

    return (priEffective(head) > priEffective(curr));
}

PPCKERNEL void SwitchPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct iframe* iframe)
{
    struct TaskPPC* outTask = PowerPCBase->pp_ThisPPCProc;
    struct TaskPPC* currTask = PowerPCBase->pp_ThisPPCProc;
    while (1)
    {
        if (!(currTask))
        {
            if (currTask = (struct TaskPPC*)RemHeadPPC((struct List*)&PowerPCBase->pp_NewTasks))
            {
                DispatchPPC(PowerPCBase, (struct MsgFrame*)currTask);
                break;
            }
            if (currTask = (struct TaskPPC*)RemHeadPPC((struct List*)&PowerPCBase->pp_ReadyTasks))
            {
                currTask->tp_Task.tc_State = TS_RUN;
                priMarkRunning(currTask);
                PowerPCBase->pp_ThisPPCProc = currTask;
                iframe = (struct iframe*)currTask->tp_ContextMem;
                currTask->tp_Task.tc_SPReg = (APTR)iframe->if_Context.ec_GPR[1];
                break;
            }
            break;
        }
        if (currTask->tp_Task.tc_State == TS_REMOVED)
        {
            AddTailPPC((struct List*)&PowerPCBase->pp_RemovedTasks, (struct Node*)currTask);
            currTask = NULL;
            PowerPCBase->pp_ThisPPCProc = currTask;
            iframe->if_Context.ec_SRR1 = MACHINESTATE_DEFAULT;
            iframe->if_Context.ec_UPC.ec_SRR0 = (PowerPCBase->pp_PPCMemBase) + OFFSET_SYSMEM;
        }
        else if (currTask->tp_Task.tc_State == TS_CHANGING)
        {
            currTask->tp_Task.tc_State = TS_WAIT;
            /*
             * WHEN THE WAIT BEGINS, powerpc.p:4930-4933 "mftbl r4 /
             * stw r4,TASKPPC_TIMESTAMP2(r5)". ReSchedule charges the time
             * since this stamp to tp_Elapsed2 (:5434-5439), which is the
             * denominator of tp_Activity -- without it a task that waits
             * looks as though it never waited.
             *
             * The WAIT list stays on plain EnqueuePPC: the original uses
             * EnqueuePPC here (:4934-4936) and EnqueueTask only for the READY
             * list, so the priority rule applies to what competes for the CPU,
             * not to what is parked.
             */
            currTask->tp_Timestamp2 = getTBL();
            //AddTailPPC((struct List*)&PowerPCBase->pp_WaitingTasks, (struct Node*)currTask); //fair
            EnqueuePPC((struct List*)&PowerPCBase->pp_WaitingTasks, (struct Node*)currTask);   //priority
            currTask = NULL;
            PowerPCBase->pp_ThisPPCProc = currTask;
        }
        else
        {
            /*
             * A NEW TASK PREEMPTS UNCONDITIONALLY. The original tests the
             * priorities only against the READY head (:4643); a task arriving
             * from the 68k side goes through DispatchPPC either way.
             */
            if (currTask = (struct TaskPPC*)RemHeadPPC((struct List*)&PowerPCBase->pp_NewTasks))
            {
                struct TaskPPC* oldTask = PowerPCBase->pp_ThisPPCProc;
                priLeaveRun(PowerPCBase, oldTask);
                oldTask->tp_Task.tc_State = TS_READY;
                //AddTailPPC((struct List*)&PowerPCBase->pp_ReadyTasks, (struct Node*)oldTask);
                priInsert(PowerPCBase, &PowerPCBase->pp_ReadyTasks, oldTask);
                DispatchPPC(PowerPCBase, (struct MsgFrame*)currTask);
            }
            /*
             * OTHERWISE THE PRIORITY RULE DECIDES, and the ready head is only
             * REMOVED once it has won -- the old code called RemHeadPPC to
             * find out whether anything was there, which commits to the switch
             * before the question is asked.
             *
             * The ready list is enqueued with priInsert, not EnqueuePPC: the
             * original's Schedule uses EnqueueTask here under NEWSCHEDULER
             * (:4918-4921) and EnqueuePPC only in the #else, so the outgoing
             * task is placed by effective priority and capped at the ceiling.
             * EnqueuePPC sorts on ln_Pri, which nothing in the V14 rule
             * writes.
             */
            else if (priShouldSwitch(PowerPCBase, PowerPCBase->pp_ThisPPCProc))
            {
                struct TaskPPC* oldTask = PowerPCBase->pp_ThisPPCProc;

                priLeaveRun(PowerPCBase, oldTask);
                currTask = (struct TaskPPC*)RemHeadPPC((struct List*)&PowerPCBase->pp_ReadyTasks);
                oldTask->tp_Task.tc_State = TS_READY;
                currTask->tp_Task.tc_State = TS_RUN;
                priMarkRunning(currTask);
                //AddTailPPC((struct List*)&PowerPCBase->pp_ReadyTasks, (struct Node*)oldTask);
                priInsert(PowerPCBase, &PowerPCBase->pp_ReadyTasks, oldTask);
                PowerPCBase->pp_ThisPPCProc = currTask;
                iframe = (struct iframe*)currTask->tp_ContextMem;
                currTask->tp_Task.tc_SPReg = (APTR)iframe->if_Context.ec_GPR[1];
            }
            break;
        }
    }

    {
        struct TaskPPC* inTask = PowerPCBase->pp_ThisPPCProc;

        /*
         * THE INSTRUMENTATION THAT SETTLED THIS IS GONE, and this note is
         * what it established. Between 20 and 21 Oct 2026 this block also
         * wrote three fields that tasksPPC.h calls free space for debuggers
         * -- tp_DebugData at :39 (SwitchPPC entries in the high half, actual
         * task changes in the low half), tp_Timestamp at :42 (the library's
         * own tc_Switch count, with the tc_Flags byte it read in the top 8
         * bits) and tp_Timestamp2 at :43 (the outTask pointer) -- so that
         * tests/taskhook could compare the library's view against its own.
         *
         * It proved the gate and the hooks below correct: 5 calls per task
         * over the spin, flags 0xc0 and 0xc1, and outTask pointers matching
         * the test's own me and other exactly. The apparent "one hook call"
         * that prompted it was tests/taskhook.s counting with "addi r0,r0,1",
         * where an rA of r0 is the literal zero, so it stored 1 every time.
         *
         * Removed with taskhook at 6/6 PASS (commit 57984e9); its checks 1-6
         * never read these fields. The library writes none of the three now
         * and accounts from pp_CurrentTBL, where the original keeps per-task
         * timestamps -- a remaining difference, not a defect.
         */
        if (inTask != outTask)
        {
            /*
             * tc_Switch's r2 IS A DELIBERATE DIVERGENCE, FIXES.md item 3, and
             * this comment is here so it is not "fixed" later.
             *
             * The original passes THE KERNEL'S OWN BASE: "mfsprg r2,3" at
             * powerpc.p:4749, where SPRG3 holds the base the kernel addresses
             * itself through -- it uses r2 as that base everywhere
             * ("lwz r28,PPC_REACTION(r2)") and stashes it in SPRG3 across the
             * sleep transition (:4585, :4950). This library passes getR2(),
             * which is the INTERRUPTED TASK'S r2: kernel.s:84-89 never reloads
             * r2 before "bl _Exception_Entry", so the whole exception path
             * runs with it still live.
             *
             * THE LIBRARY ITSELF DOES NOT CARE. None of the four PPC C files
             * has a single file-scope variable -- every function takes
             * PowerPCBase in r3 and reaches everything through it, and the
             * only tables are the PC-relative assembler ones in setupppc.s --
             * so there is no data for r2 to address. The PPC options carry no
             * -sc either (build.ps1, "+warpos -c99 -O2 -speed").
             *
             * AND NO CONFORMING HOOK CAN CARE, which is what makes this safe
             * rather than merely untested. The original hands tc_Switch the
             * kernel's base, which is not a usable TOC for client code, so any
             * hook must establish its own addressing. iFUSION demonstrably
             * does: recon/asm/tocrecover.s records that "powerpc.library's
             * task switcher enters tc_Launch with r2 UNDEFINED", and
             * ifusion_tc_Launch's FIRST act is to recover its TOC from a data
             * word embedded in its own code through a bl/mflr trick. That
             * reading came from iFUSION's binary, independently of this work.
             *
             * SO THIS IS NOT CHANGED, and the reason is a cost one. Matching
             * the original would mean keeping a kernel base in SPRG3 -- and
             * SPRG3 is in use here as the r0 scratch during exception entry
             * (kernel.s:40, :80), so it would mean re-allocating a register in
             * the path items 9 and 11 just stabilised and that all four tests
             * cover, for no observable gain. The value passed is a live task's
             * r2, never garbage, so there is no crash risk either.
             */
            if ((outTask) && (outTask->tp_Task.tc_Flags & TF_SWITCH) && (outTask->tp_Task.tc_Switch))
            {
                callTaskHook((APTR)outTask->tp_Task.tc_Switch, getR2());
            }

            if ((inTask) && (inTask->tp_Task.tc_Flags & TF_LAUNCH) && (inTask->tp_Task.tc_Launch))
            {
                struct iframe* inFrame = (struct iframe*)inTask->tp_ContextMem;

                callTaskHook((APTR)inTask->tp_Task.tc_Launch,
                             inFrame->if_Context.ec_GPR[2]);
            }

            /*
             * THE PER-TASK QUANTUM, FIXES.md item 18, applied here rather than
             * in Exception_Entry. This point is safe where that one was not:
             * the scheduler has just taken inTask off a list and set it
             * TS_RUN, so the structure is known good, whereas Exception_Entry
             * runs on every exception including init's and crashed when it
             * dereferenced pp_ThisPPCProc there.
             *
             * CPUStats derives tp_Quantum from system load. Until it first
             * runs, tp_Quantum holds the pp_StdQuantum CreateTaskPPC gave it
             * (functionsppc.c:551), so the untouched path is the old
             * behaviour. A zero is ignored rather than armed.
             */
            /*
             * THE DECREMENTER GETS THE REMAINING SLICE, NOT THE WHOLE
             * QUANTUM, and it is a MINIMUM. powerpc.p:4973-4980 on the launch
             * path:
             *
             *     lwz   r4,TASKPPC_QUANTUM(r3)
             *     lwz   r6,TASKPPC_ELAPSED(r3)
             *     subf  r4,r6,r4            ; remaining = quantum - elapsed
             *     lwz   r6,PPC_DELAY(r2)
             *     cmplw r4,r6 / bgt .timeok
             *     stw   r4,PPC_DELAY(r2)    ; PPC_DELAY = min(PPC_DELAY, rem)
             *
             * This library assigned the full tp_Quantum, so a task that had
             * already used part of its slice got a whole fresh one every time
             * it was rescheduled -- the slice was never actually enforced.
             *
             * pp_Quantum is PPC_DELAY and Exception_Entry resets it to
             * pp_StdQuantum on entry (:47), so taking the minimum here is what
             * makes it the earliest pending deadline. FIXES.md item 2 is the
             * other half of that: the original also walks the decrementer list
             * and shortens the same field for a timed exception handler, which
             * this library still does not do.
             */
            if ((inTask) && (inTask->tp_Quantum))
            {
                ULONG remaining = inTask->tp_Quantum;

                if (remaining > inTask->tp_Elapsed)
                {
                    remaining -= inTask->tp_Elapsed;
                }

                if (remaining < PowerPCBase->pp_Quantum)
                {
                    PowerPCBase->pp_Quantum = remaining;
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

PPCKERNEL void DispatchPPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* myFrame)
{
    struct NewTask* newTask = (struct NewTask*)myFrame->mf_Arg[0];

    PowerPCBase->pp_IdUsrTasks += 1;

    newTask->nt_Task.pt_Task.tp_Id = PowerPCBase->pp_IdUsrTasks;
    newTask->nt_Task.pt_Task.tp_PowerPCBase = PowerPCBase;
    newTask->nt_Task.pt_Task.tp_Task.tc_Node.ln_Type = NT_PPCTASK;
    newTask->nt_Task.pt_Task.tp_Task.tc_State = TS_RUN;
    newTask->nt_Task.pt_Task.tp_ContextMem = &newTask->nt_Context;
    newTask->nt_Task.pt_Task.tp_BATStorage = &newTask->nt_BatStore;
    newTask->nt_Task.pt_Task.tp_Link.tl_Task = newTask;
    newTask->nt_Task.pt_Task.tp_Link.tl_Sig = 0xfff;
    newTask->nt_Task.pt_Task.tp_StackMem = (APTR)(myFrame->mf_Arg[0] + 2048);
    newTask->nt_Task.pt_Mirror68K = (struct Task*)myFrame->mf_Arg[2];
    newTask->nt_Task.pt_MirrorPort = myFrame->mf_MirrorPort;
    newTask->nt_Task.pt_Task.tp_Task.tc_Node.ln_Name = (APTR)&newTask->nt_Name;
    newTask->nt_Task.pt_Task.tp_StackSize = (ULONG)myFrame->mf_Message.mn_Node.ln_Name;
    newTask->nt_Task.pt_Task.tp_Task.tc_SPLower = newTask->nt_Task.pt_Task.tp_StackMem;
    newTask->nt_Task.pt_Task.tp_Task.tc_SPUpper = (APTR)((ULONG)newTask->nt_Task.pt_Task.tp_StackMem + newTask->nt_Task.pt_Task.tp_StackSize);
    newTask->nt_Task.pt_Task.tp_Task.tc_SPReg   = (APTR)((ULONG)newTask->nt_Task.pt_Task.tp_Task.tc_SPUpper - 32);

    NewListPPC((struct List*)&newTask->nt_Task.pt_Task.tp_Task.tc_MemEntry);
    newTask->nt_Task.pt_Task.tp_Task.tc_SigAlloc = myFrame->mf_Arg[1];
    PowerPCBase->pp_ThisPPCProc = (struct TaskPPC*)newTask;

    /*
     * STAMP IT AS RUNNING HERE TOO. This is the other place a task becomes
     * TS_RUN, and priLeaveRun charges it "now - tp_Timestamp" when it next
     * leaves the CPU. CreateTaskPPC leaves tp_Timestamp zero, so without this
     * the first task dispatched from the 68k side is charged the whole
     * timebase as CPU time, which drives its tp_Priority hugely negative and
     * takes the running minimum -- and so the ceiling, and every ready task
     * pulled down onto it -- with it. The relief term unwinds that over the
     * next pass, but the distortion is real and avoidable.
     *
     * The original does not need an equivalent because every task it runs has
     * passed through .switch (powerpc.p:4969-4972) first, which stamps it.
     */
    priMarkRunning((struct TaskPPC*)newTask);

    newTask->nt_Task.pt_Task.tp_TaskPtr = &newTask->nt_TaskPtr;
    newTask->nt_TaskPtr.tptr_Task = (struct TaskPPC*)newTask;
    newTask->nt_TaskPtr.tptr_Node.ln_Name = newTask->nt_Task.pt_Task.tp_Task.tc_Node.ln_Name;
    AddTailPPC((struct List*)&PowerPCBase->pp_AllTasks, (struct Node*)&newTask->nt_TaskPtr);

    PowerPCBase->pp_NumAllTasks += 1;

    NewListPPC((struct List*)&newTask->nt_Task.pt_Task.tp_TaskPools);
    NewListPPC((struct List*)&newTask->nt_Port.mp_IntMsg);
    NewListPPC((struct List*)&newTask->nt_Port.mp_Port.mp_MsgList);
    newTask->nt_Port.mp_Port.mp_SigBit = SIGB_DOS;
    NewListPPC((struct List*)&newTask->nt_Port.mp_Semaphore.ssppc_SS.ss_WaitQueue);
    newTask->nt_Port.mp_Semaphore.ssppc_SS.ss_Owner = NULL;
    newTask->nt_Port.mp_Semaphore.ssppc_SS.ss_NestCount = 0;
    newTask->nt_Port.mp_Semaphore.ssppc_SS.ss_QueueCount = -1;
    newTask->nt_Port.mp_Semaphore.ssppc_reserved = &newTask->nt_SSReserved1;
    newTask->nt_Port.mp_Port.mp_SigTask = newTask;
    newTask->nt_Port.mp_Port.mp_Flags = PA_SIGNAL;
    newTask->nt_Port.mp_Port.mp_Node.ln_Type = NT_MSGPORTPPC;
    newTask->nt_Task.pt_Task.tp_Msgport = &newTask->nt_Port;

    struct iframe* newFrame = (struct iframe*)&newTask->nt_Context;

    newFrame->if_Context.ec_SRR1 = MACHINESTATE_DEFAULT;
    newFrame->if_Context.ec_UPC.ec_SRR0 = *((ULONG*)((ULONG)PowerPCBase + 2 + _LVOStartTask));
    newFrame->if_Context.ec_GPR[1] = (ULONG)newTask->nt_Task.pt_Task.tp_Task.tc_SPReg;
    newFrame->if_Context.ec_GPR[2] = NULL;
    newFrame->if_Context.ec_GPR[3] = (ULONG)PowerPCBase;
    newFrame->if_Context.ec_GPR[4] = (ULONG)myFrame;

    ULONG* EndofStack = (APTR)newFrame->if_Context.ec_GPR[1];
    EndofStack[0] = 0; //Clear end of stack chain for SwapStack

    CopyMemPPC(&PowerPCBase->pp_SystemBATs, &newFrame->if_BATs, sizeof(struct BATArray) * 4);
    CopyMemPPC(&PowerPCBase->pp_SystemBATs, newTask->nt_Task.pt_Task.tp_BATStorage, sizeof(struct BATArray) * 4);

    for (int i = 0; i < 16; i++)
    {
        newFrame->if_Segments[i] = PowerPCBase->pp_SystemSegs[i];
    }

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

/*
 * EXCATTR_TIMEDREMOVAL: is this handler due to be called yet?
 *
 * ed_TimeBasePtr points at the client's two-word deadline, [high, low]. A
 * DECREMENTER handler carrying one is NOT called on every decrementer
 * exception -- it is skipped until the timebase reaches that deadline. This is
 * the whole of what the tag does, and it is why the field is a POINTER: the
 * client can re-arm the deadline at any time without touching the library.
 *
 * ed_TimeBaseUpper/ed_TimeBaseLower are not a time, they are a CACHE of the
 * deadline last honoured. Three consequences, all of them the original's:
 *
 *   - they start at zero (mySetExcHandler allocates MEMF_CLEAR, and
 *     powerpc.p:11264-11265 zeroes them explicitly), so the first armed
 *     deadline is always compared properly.
 *   - once a deadline has been honoured the cache equals it, and the first
 *     test below then returns TRUE on every subsequent tick. So the tag makes
 *     a ONE-SHOT timer that reverts to per-tick until the client re-arms.
 *     powerpc.p:6892 "cmplw r0,r4 / beq .notdec" is that branch.
 *   - a handler on any other exception list is unaffected, because the
 *     original guards the whole block with "cmplw r0,EXCF_DEC / bne .notdec"
 *     at powerpc.p:6874.
 *
 * THE COMPARISON IS THE ORIGINAL'S, INCLUDING ITS QUIRK. powerpc.p:6898-6904
 * is
 *
 *       lwz r4,0(r6) / mftbu r0 / cmplw r0,r4 / bgt .decok
 *       lwz r4,4(r6) / mftbl r0 / cmplw r0,r4 / bge .decok
 *       b .next
 *
 * so due = (now_hi > deadline_hi) || (now_lo >= deadline_lo). That is not a
 * 64-bit compare: when now_hi < deadline_hi it still consults the low words
 * and can fire early. Reproduced as written rather than corrected -- a client
 * arming a deadline more than a timebase period out is the only case that
 * differs, and changing it would be a behaviour change this library has no
 * evidence for. See FIXES.md item 8.
 */
static BOOL excTimedDue(struct ExcData* excData, struct iframe* iframe)
{
    ULONG* deadline = (ULONG*)excData->ed_TimeBasePtr;

    if ((iframe->if_Context.ec_ExcID != EXCF_DECREMENTER) || (!deadline))
    {
        return TRUE;
    }

    if (excData->ed_TimeBaseLower == deadline[1])
    {
        return TRUE;
    }

    if ((getTBU() <= deadline[0]) && (getTBL() < deadline[1]))
    {
        return FALSE;
    }

    excData->ed_TimeBaseUpper = deadline[0];
    excData->ed_TimeBaseLower = deadline[1];

    return TRUE;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL void CommonExcHandler(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct iframe* iframe, __reg("r5") struct List* excList)
{
    struct ExcData* nxtNode;
    struct ExcData* currNode = (struct ExcData*)excList->lh_Head;
    ULONG status = EXCRETURN_NORMAL;

    while (nxtNode = (struct ExcData*)currNode->ed_Node.ln_Succ)
    {
        if (currNode->ed_ExcID & iframe->if_Context.ec_ExcID)
        {
            if ((currNode->ed_Flags & EXCF_GLOBAL) || ((currNode->ed_Flags & EXCF_LOCAL) && (currNode->ed_Task) && (currNode->ed_Task == PowerPCBase->pp_ThisPPCProc)))
            {
                /*
                 * After the task test and before the context branch, which is
                 * where the original puts it -- powerpc.p:6870 ".notaskcheck"
                 * then the DECEXC block, then "bittst r7,EXCP_LARGECONTEXT".
                 * Skipping is the original's "b .next": the handler keeps its
                 * place in the list and is reconsidered on the next tick.
                 */
                if (!excTimedDue(currNode, iframe))
                {
                    currNode = nxtNode;
                    continue;
                }

                if (currNode->ed_Flags & EXCF_LARGECONTEXT)
                {
                    ULONG (*ExcHandler)(__reg("r2") ULONG, __reg("r3") struct EXCContext*) = currNode->ed_Code;
                    ULONG tempR2 = getR2();
                    status = ExcHandler(currNode->ed_Data, &iframe->if_Context);
                    storeR2(tempR2);
                }
                else if (currNode->ed_Flags & EXCF_SMALLCONTEXT)
                {
                    status = SmallExcHandler(currNode, iframe);
                }
                if (status == EXCRETURN_ABORT)
                {
                    break;
                }
            }
        }
        currNode = nxtNode;
    }

    if ((status == EXCRETURN_NORMAL) && (iframe->if_Context.ec_ExcID != EXCF_DACCESS))
    {
        CommonExcError(PowerPCBase, iframe);
    }
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL VOID CPUStats(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    ULONG currentTBL = getTBL();
    ULONG prevTBL = PowerPCBase->pp_CurrentTBL;
    PowerPCBase->pp_CurrentTBL = currentTBL;

    /*
     * pp_FlagReschedule, FIXES.md item 17: four sites set it when the run
     * queue changes materially -- CreateTaskPPC inserting a ready task,
     * DeleteTaskPPC, SignalPPC waking a task that becomes top of the ready
     * list, and supportppc.c's task halt. It means "recompute now, do not wait
     * for the period", so it collapses the counter and is cleared once
     * consumed.
     *
     * This is the reader the field never had. It corresponds to the original
     * running its rescheduler on the shorter RESCH_DEFAULT deadline, about
     * 6 ms, rather than the usual RESCH_TICKS of about 150 ms.
     */
    if ((PowerPCBase->pp_FlagReschedule) && (PowerPCBase->pp_ThisPPCProc))
    {
        /*
         * ONLY ONCE THERE IS A RUNNING TASK. pp_BusyCounter == 0 is what gates
         * the expensive block below -- the walk of pp_AllTasks -- and before
         * this reader existed that block ran once every 25 ticks, so init
         * finished before it first fired. Consuming the flag during init
         * forced it immediately and the library CRASHED DURING INIT, reported
         * as Task 0x0 at _CommonExcHandler+0x10C. The figures describe tasks
         * competing for the CPU, so there is nothing to compute before one is
         * running. FIXES.md item 17.
         */
        PowerPCBase->pp_FlagReschedule = 0;
        PowerPCBase->pp_BusyCounter = 0;
    }

    if (prevTBL)
    {
        ULONG systemLoad = 0;
        ULONG cpuLoad = 0;

        /*
         * THE STATISTICS INTERVAL IS NOT THE GAP BETWEEN EXCEPTIONS, and
         * conflating the two made every ratio below saturate.
         *
         * pp_CurrentTBL is updated at the top of this function, which runs on
         * EVERY exception, while tp_Elapsed, tp_Elapsed2 and tp_Totalelapsed
         * are zeroed only inside the "pp_BusyCounter == 0" block -- many
         * exceptions later. So the counters accumulated over a long period and
         * were divided by a short one: total >> interval, and tp_Busy,
         * tp_CPUusage and tp_Activity all pinned at their 10000 ceiling.
         * prisched's first hardware run showed it plainly -- BOTH tasks
         * reporting cpuusage 10000 and busy 10000, which cannot be true of two
         * tasks at once -- and tp_Desired was then computed from a saturated
         * activity, so the whole priority mechanism was running on inputs that
         * carried no information.
         *
         * The original has no such split: PPC_RESCH_TBL_OLD is written once
         * per ReSchedule call (powerpc.p:5386-5391) and ReSchedule is the
         * function that both measures and zeroes, so its r5 spans exactly the
         * period its counters cover.
         *
         * pp_StartTBL is PPC_RESCH_TBL's slot and had no reader or writer
         * anywhere in lib/ before this, so it is the field for the job.
         */
        ULONG statsInterval = 0;

        if (!(PowerPCBase->pp_BusyCounter))
        {
            if (PowerPCBase->pp_StartTBL)
            {
                statsInterval = currentTBL - PowerPCBase->pp_StartTBL;
            }
            PowerPCBase->pp_StartTBL = currentTBL;
        }

        /*
         * THE PER-PASS RELIEF, powerpc.p:5396-5403, computed ONCE before the
         * walk:
         *
         *     lwz   r7,PPC_MINPRI(r2)
         *     neg.  r0,r7
         *     lwz   r28,PPC_REACTION(r2)
         *     subf. r28,r28,r0        ; r28 = (-MinPri) - Reaction
         *     bge   .noclamp1 / li r28,0
         *
         * so relief = max(0, -MinPri - Reaction). It is zero while the minimum
         * is within Reaction of zero and becomes positive only once the
         * minimum has sunk further than that -- at which point it is added to
         * EVERY task's accumulated penalty and to the minimum itself, dragging
         * the whole scale back up by the same amount. That is what keeps the
         * numbers bounded without changing any task's position relative to
         * another.
         */
        LONG relief = -(PowerPCBase->pp_LowActivityPriOffset)
                      - PowerPCBase->pp_LowActivityPri;

        if (relief < 0)
        {
            relief = 0;
        }

        struct TaskPtr* currTask = (struct TaskPtr*)PowerPCBase->pp_AllTasks.mlh_Head;
        struct TaskPtr* nextTask;

        while (nextTask = (struct TaskPtr*)currTask->tptr_Node.ln_Succ)
        {
            struct TaskPPC* ppcTask = (struct TaskPPC*)currTask->tptr_Task;

            /*
             * THE ACCOUNTING IS PER TASK, FROM ITS OWN TIMESTAMP, and this
             * block used to add the whole inter-pass interval to whichever
             * bucket matched the task's state AT THE END of it
             * (powerpc.p:5414-5440):
             *
             *     .running   delta = now - tp_Timestamp
             *                tp_Elapsed += delta; tp_Totalelapsed += delta
             *                tp_Timestamp = now
             *     TS_WAIT    tp_Elapsed2 += now - tp_Timestamp2
             *                tp_Timestamp2 = now
             *
             * A TS_READY task gets NOTHING -- it is neither running nor
             * waiting, and charging it the interval is what made these figures
             * meaningless. The old code charged every task the full interval,
             * so a task that ran for a tenth of it and was ready for the rest
             * was recorded as having run for all of it, while the one that
             * actually ran got the same. That is FIXES.md's open item 4: with
             * two tasks spinning flat out the summed load read 1666 against a
             * threshold of 5000, so the load-scaled quantum never engaged.
             */
            if ((ppcTask->tp_Task.tc_State == TS_RUN) || (ppcTask->tp_Task.tc_State == TS_CHANGING))
            {
                ULONG delta = currentTBL - ppcTask->tp_Timestamp;

                ppcTask->tp_Elapsed += delta;
                ppcTask->tp_Totalelapsed += delta;
                ppcTask->tp_Timestamp = currentTBL;
            }
            else if (ppcTask->tp_Task.tc_State == TS_WAIT)
            {
                ppcTask->tp_Elapsed2 += currentTBL - ppcTask->tp_Timestamp2;
                ppcTask->tp_Timestamp2 = currentTBL;
            }

            if (!(PowerPCBase->pp_BusyCounter))
            {
                ULONG elapsed2 = ppcTask->tp_Elapsed2;
                ULONG total = ppcTask->tp_Totalelapsed;

                /*
                 * ALL THREE RATIOS ARE BUILT FROM tp_Totalelapsed, NOT
                 * tp_Elapsed, powerpc.p:5428-5452. tp_Elapsed is the quantum
                 * counter and the original uses it in none of them:
                 *
                 *     .cont   mr r3,r7              ; r7 = TOTALELAPSED
                 *     .cont1  add r0,r11,r7         ; elapsed2 + TOTALELAPSED
                 *             srawi r31,r7,10       ; TOTALELAPSED >> 10
                 *             srawi r0,r0,10
                 *             mulli r31,r31,10000
                 *             tstw r0 / beq .zero / divwu r0,r31,r0
                 *
                 * so activity is run-time over (run-time + wait-time). This
                 * library divided tp_Elapsed by (tp_Elapsed + tp_Elapsed2),
                 * which is the same shape over a counter that the scheduler
                 * resets on every quantum -- a ratio of whatever happened
                 * since the last reset, not of the task's life.
                 */
                ULONG activity = (elapsed2 + total) >> 10;
                ULONG totelps = (total >> 10) * 10000;

                if (activity)
                {
                    activity = totelps / activity;
                }

                ppcTask->tp_Activity = activity;

                /*
                 * tp_Desired, FIXES.md item 18, per powerpc.p:5460-5468:
                 *
                 *     addi  r29,r29,PRI_OFFSET
                 *     lwz   r7,TASKPPC_NICE(r4)
                 *     addi  r31,r7,NICE_RANGE/2
                 *     slwi  r31,r31,2
                 *     lwzx  r31,r7,r31
                 *     srwi  r31,r31,8
                 *     slwi  r7,r29,16
                 *     divwu r0,r7,r31
                 *
                 * It was "= 0; //from NICE table TBI" at
                 * functionsppc.c:553. The nice value is clamped to the
                 * table's range here as well as in mySetNiceValue, because
                 * tp_Nice is also inherited from the parent task at
                 * functionsppc.c:417 and indexing this table out of range
                 * would read whatever follows it.
                 */
                if ((PowerPCBase->pp_NICETable) && (PowerPCBase->pp_ThisPPCProc))
                {
                    const ULONG* niceTab = (const ULONG*)PowerPCBase->pp_NICETable;
                    LONG  nice = (LONG)ppcTask->tp_Nice;
                    ULONG weight;

                    if (nice < -(NICE_RANGE / 2))
                    {
                        nice = -(NICE_RANGE / 2);
                    }
                    else if (nice > (NICE_RANGE / 2))
                    {
                        nice = (NICE_RANGE / 2);
                    }

                    weight = niceTab[nice + (NICE_RANGE / 2)] >> 8;

                    if (weight)
                    {
                        ppcTask->tp_Desired = ((activity + PRI_OFFSET) << 16) / weight;
                    }
                }

                /*
                 * THE DENOMINATOR OF BOTH OF THESE IS THE INTERVAL BETWEEN
                 * PASSES, NOT THE TASK'S OWN TOTAL, powerpc.p:5471-5504. r5 is
                 * "now - PPC_RESCH_TBL_OLD", computed at the top of ReSchedule
                 * (:5386-5391), which is cpuTime here:
                 *
                 *     srawi r0,r5,10        ; interval >> 10
                 *     srawi r11,r11,10      ; elapsed2 >> 10
                 *     subf. r11,r11,r0      ; interval - wait
                 *     bgt .notneg2 / li r11,0
                 *     mulli r11,r11,10000
                 *     divwu r11,r11,r0      ; busy  = (iv - wait) / iv
                 *   ...
                 *     srawi r11,r5,10
                 *     srawi r0,r3,10        ; TOTALELAPSED >> 10
                 *     mulli r0,r0,10000
                 *     divwu r7,r0,r11       ; usage = run-time / iv
                 *
                 * so tp_Busy is the fraction of the INTERVAL the task was not
                 * waiting and tp_CPUusage the fraction it actually ran. Both
                 * are per-interval and both are therefore bounded and
                 * comparable between tasks, which is what makes summing them
                 * into pp_SystemLoad and pp_CPULoad meaningful. This library
                 * divided by the task's own tp_Totalelapsed, so each task's
                 * figure was scaled by its own history: that is why two tasks
                 * spinning flat out summed to 1666 instead of near 10000.
                 *
                 * The guard stays. cpuTime can be under the 1024-tick
                 * granularity of these shifts, and divwu by zero does not trap
                 * on PowerPC -- it returns an undefined result, which is how
                 * the unguarded tp_Busy division produced garbage in silence
                 * before 57984e9. cpuUsage is also declared INSIDE this loop:
                 * it was outside it, assigned only when the divisor was
                 * non-zero and written unconditionally, so a task with nothing
                 * to measure had the previous task's figure written into its
                 * tp_CPUusage and cpuLoad counted it twice.
                 */
                ULONG interval10 = statsInterval >> 10;
                ULONG cpuUsage = 0;
                LONG  busy = (LONG)interval10 - (LONG)(elapsed2 >> 10);

                if (busy < 0)
                {
                    busy = 0;
                }

                if (interval10)
                {
                    busy = (busy * 10000) / (LONG)interval10;
                    cpuUsage = totelps / interval10;
                }
                else
                {
                    busy = 0;
                }

                ppcTask->tp_Busy = busy;

                systemLoad += busy;

                if (cpuUsage > 10000)
                {
                   cpuUsage = 10000;
                }

                ppcTask->tp_CPUusage = cpuUsage;

                cpuLoad += cpuUsage;

                ppcTask->tp_Elapsed = 0;
                ppcTask->tp_Elapsed2 = 0;
                ppcTask->tp_Totalelapsed = 0;
            }
            currTask = nextTask;
        }

        /*
         * THE QUANTUM, FIXES.md item 18, per powerpc.p:5550-5569:
         *
         *     liw   r3,RESCH_DEFAULT
         *     li    r0,100
         *     divwu r0,r12,r0          ; r12 = the summed tp_Busy
         *     add   r0,r0,r0
         *     cmpwi r0,100 / bgt / li r0,100
         *     divwu r3,r3,r0
         *     mulli r0,r3,100
         *     cmplw r0,MIN_TICKS / bgt / liw r0,MIN_TICKS
         *     stw   r0,TASKPPC_QUANTUM(r4)
         *
         * So the slice is UNIFORM across tasks and scaled by SYSTEM load, not
         * by the task's own share -- nice is loaded in that loop but only for
         * debug output. Under load the quantum shrinks towards a floor.
         *
         * THE BASE AND FLOOR ARE SCALED BY BUS CLOCK, NOT TAKEN RAW. The
         * original's RESCH_DEFAULT and MIN_TICKS are tick COUNTS and the
         * decrementer runs at bus/4, so its own comments -- "about 6 ms" and
         * "at least 1.5 ms" -- hold only for a 66 MHz bus:
         *
         *                     ticks    66 bus   100 bus   E300
         *     RESCH_DEFAULT  100000    6.00ms   4.00ms    1.50ms
         *     MIN_TICKS       25000    1.50ms   1.00ms    0.38ms
         *
         * which fits WarpOS having run only on Phase5 603e/604e boards. This
         * library supports three bus clocks and already normalises for them:
         * QUANTUM_100, QUANTUM_66 and QUANTUM_E300 are 500000, 333333 and
         * 1333333 ticks, and every one of those is exactly 20.00 ms.
         *
         * So the base is pp_StdQuantum -- 20 ms on any board, the library's
         * own calibration -- and the floor is 3/40 of it, which is the
         * original's 1.5 ms expressed as a ratio. Taking the raw counts gave
         * 4 ms on a 100 MHz board and a 0.38 ms floor on an E300, about 2600
         * preemptions a second.
         *
         * Before item 18 every task got the fixed pp_StdQuantum and it never
         * varied (functionsppc.c:551). The slice now runs from 20 ms idle down
         * to 1.5 ms under load.
         */
        if ((!(PowerPCBase->pp_BusyCounter)) && (PowerPCBase->pp_ThisPPCProc))
        {
            ULONG base = PowerPCBase->pp_StdQuantum;
            ULONG floorQuantum = (base / 40) * 3;
            ULONG divisor = (systemLoad / 100) * 2;
            ULONG quantum;

            if (divisor < 100)
            {
                divisor = 100;
            }
            quantum = (base / divisor) * 100;
            if (quantum < floorQuantum)
            {
                quantum = floorQuantum;
            }

            currTask = (struct TaskPtr*)PowerPCBase->pp_AllTasks.mlh_Head;
            while (nextTask = (struct TaskPtr*)currTask->tptr_Node.ln_Succ)
            {
                struct TaskPPC* ppcTask = (struct TaskPPC*)currTask->tptr_Task;

                /*
                 * FOLD THIS PASS'S PENALTY INTO tp_Prioffset AND CLEAR
                 * tp_Priority, powerpc.p:5537-5547:
                 *
                 *     lwz  r0,TASKPPC_PRIORITY(r4)
                 *     lwz  r3,TASKPPC_PRIOFFSET(r4)
                 *     add  r0,r0,r3
                 *     add. r7,r0,r28         ; + the relief
                 *     blt  .notov            ; NEGATIVE: keep it
                 *     li   r7,0              ; non-negative: clamp to zero
                 *     .notov
                 *     stw  r7,TASKPPC_PRIOFFSET(r4)
                 *     ...
                 *     stwi 0,TASKPPC_PRIORITY(r4)
                 *
                 * NOTE WHICH WAY THE CLAMP GOES: blt SKIPS the "li r7,0", so a
                 * negative sum is kept and a non-negative one becomes zero.
                 * tp_Prioffset is therefore an accumulated NEGATIVE penalty
                 * with an upper bound of zero -- it can never become a bonus.
                 * The sites that cap a task at the ceiling (prirule.h's
                 * priPullToCeiling) may leave it positive in between, and this
                 * is what normalises it again.
                 *
                 * tp_Priority is a per-pass snapshot, not a running total:
                 * priDecay assigns it from tp_Totalelapsed on every switch and
                 * this folds it away and zeroes it.
                 */
                LONG offset = priEffective(ppcTask) + relief;

                if (offset >= 0)
                {
                    offset = 0;
                }
                ppcTask->tp_Prioffset = (ULONG)offset;
                ppcTask->tp_Priority = 0;

                ppcTask->tp_Quantum = quantum;
                currTask = nextTask;
            }

            /*
             * AND THE MINIMUM ITSELF MOVES UP BY THE SAME RELIEF,
             * powerpc.p:5574-5579:
             *
             *     lwz r3,PPC_MINPRI(r2) / add r3,r3,r28 / stw r3,PPC_MINPRI(r2)
             *
             * which drives pp_LowActivityPriOffset back towards
             * -pp_LowActivityPri and holds it there: once the minimum is
             * exactly that far below zero the relief computes as zero and the
             * scale stops moving. Every task's penalty was relieved by this
             * same amount in the loop above, so no task's position relative to
             * another changes -- only the absolute numbers shrink.
             */
            PowerPCBase->pp_LowActivityPriOffset += relief;
        }

        if (!(PowerPCBase->pp_BusyCounter))
        {
            PowerPCBase->pp_BusyCounter = 25;
            PowerPCBase->pp_SystemLoad = systemLoad;
            PowerPCBase->pp_CPULoad = cpuLoad;
        }

        PowerPCBase->pp_BusyCounter -= 1;

    }
    return;
}

#if 0
/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL VOID writememLongPPC(__reg("r3") ULONG Base, __reg("r4") ULONG offset, __reg("r5") ULONG value)
{
	*((ULONG*)(Base + offset)) = value;
	return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL ULONG readmemLongPPC(__reg("r3") ULONG Base, __reg("r4") ULONG offset)
{
    ULONG res;
    res = *((ULONG*)(Base + offset));
    return res;
}
#endif
/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL struct MsgFrame* KCreateMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
	ULONG msgFrame = 0;

    switch (PowerPCBase->pp_DeviceID)
	{
		case DEVICE_HARRIER:
		{
			ULONG msgOffset = readmemLongPPC(PPC_XCSR_BASE, XCSR_MIOFT);
            msgFrame = readmemLongPPC(msgOffset, 0);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIOFT, (msgOffset + 4) & 0xffff3fff);
            break;
		}

		case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
		{
			struct killFIFO* myFIFO = (struct killFIFO*)((ULONG)(PowerPCBase->pp_PPCMemBase + FIFO_END));
			msgFrame = *((ULONG*)(myFIFO->kf_MIOFT));
			myFIFO->kf_MIOFT = (myFIFO->kf_MIOFT + 4) & 0xffff3fff;
			break;
		}

		case DEVICE_MPC107:
		{
			ULONG msgOffset = loadPCI(PPC_EUMB_BASE, MPC107_OFTPR);
            msgFrame = readmemLongPPC(msgOffset, 0);
            storePCI(PPC_EUMB_BASE, MPC107_OFTPR, (((msgOffset + 4) | 0xc000) & 0xffff));
            break;
		}

		default:
		{
			break;
		}
	}
    return (struct MsgFrame*)msgFrame;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL struct MsgFrame* KGetMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    ULONG msgFrame = 0;

    switch (PowerPCBase->pp_DeviceID)
	{
		case DEVICE_HARRIER:
		{
			ULONG msgOffset1 = readmemLongPPC(PPC_XCSR_BASE, XCSR_MIIPH);
            ULONG msgOffset2 = readmemLongPPC(PPC_XCSR_BASE, XCSR_MIIPT);
            if (msgOffset1 != msgOffset2)
            {
                writememLongPPC(PPC_XCSR_BASE, XCSR_MIIPT, (msgOffset2 + 4) & 0xffff3fff);
                msgFrame = readmemLongPPC(msgOffset2, 0);
            }
            break;
		}

		case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
		{
			struct killFIFO* myFIFO = (struct killFIFO*)((ULONG)(PowerPCBase->pp_PPCMemBase + FIFO_END));
			if (myFIFO->kf_MIIPH != myFIFO->kf_MIIPT)
            {
                msgFrame = *((ULONG*)(myFIFO->kf_MIIPT));
			    myFIFO->kf_MIIPT = (myFIFO->kf_MIIPT + 4) & 0xffff3fff;
            }
			break;
		}

		case DEVICE_MPC107:
		{
            ULONG msgOffset1 = loadPCI(PPC_EUMB_BASE, MPC107_IPHPR);
            ULONG msgOffset2 = loadPCI(PPC_EUMB_BASE, MPC107_IPTPR);
            if (msgOffset1 != msgOffset2)
            {
                storePCI(PPC_EUMB_BASE, MPC107_IPTPR, ((msgOffset2 + 4) | 0x4000) & 0xffff7fff);
                msgFrame = readmemLongPPC(msgOffset2, 0);
            }
			break;
		}

		default:
		{
			break;
		}
	}
	return (struct MsgFrame*)msgFrame;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL VOID KSendMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* msgFrame)
{
	switch (PowerPCBase->pp_DeviceID)
	{
		case DEVICE_HARRIER:
		{
            ULONG msgOffset = readmemLongPPC(PPC_XCSR_BASE, XCSR_MIOPH);
            writememLongPPC(msgOffset, 0, (ULONG)msgFrame);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIOPH, (msgOffset + 4) & 0xffff3fff);
			break;
		}

		case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
		{
            struct killFIFO* myFIFO = (struct killFIFO*)((ULONG)(PowerPCBase->pp_PPCMemBase + FIFO_END));
			*((ULONG*)(myFIFO->kf_MIOPH)) = (ULONG)msgFrame;
			myFIFO->kf_MIOPH = (myFIFO->kf_MIOPH + 4) & 0xffff3fff;
            if (PowerPCBase->pp_DeviceID == DEVICE_MPC8343E)
            {
			    storePCI(IMMR_ADDR_DEFAULT, IMMR_OMR0, (ULONG)msgFrame);
            }
            else
            {
                storePCI(IMMR_ADDR_DEFAULT, IMMR_PEX_OMBCR, PEX_OMBCR_READY);
            }
            break;
		}

		case DEVICE_MPC107:
		{
            ULONG msgOffset = loadPCI(PPC_EUMB_BASE, MPC107_OPHPR);
            writememLongPPC(msgOffset, 0, (ULONG)msgFrame);
            storePCI(PPC_EUMB_BASE, MPC107_OPHPR, ((msgOffset + 4) & 0xbfff));
			break;
		}

		default:
		{
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

PPCKERNEL VOID KFreeMsgFramePPC(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct MsgFrame* msgFrame)
{
    //msgFrame->mf_Identifier = ID_FREE;

	switch (PowerPCBase->pp_DeviceID)
	{
		case DEVICE_HARRIER:
		{
            ULONG msgOffset = readmemLongPPC(PPC_XCSR_BASE, XCSR_MIIFH);
            writememLongPPC(msgOffset, 0, (ULONG)msgFrame);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIIFH, (msgOffset + 4) & 0xffff3fff);
            break;
		}

		case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
		{
			struct killFIFO* myFIFO = (struct killFIFO*)((ULONG)(PowerPCBase->pp_PPCMemBase + FIFO_END));
			*((ULONG*)(myFIFO->kf_MIIFH)) = (ULONG)msgFrame;
			myFIFO->kf_MIIFH = (myFIFO->kf_MIIFH + 4) & 0xffff3fff;
			break;
		}

		case DEVICE_MPC107:
		{
            ULONG msgOffset = loadPCI(PPC_EUMB_BASE, MPC107_IFHPR);
            writememLongPPC(msgOffset , 0, (ULONG)msgFrame);
            storePCI(PPC_EUMB_BASE, MPC107_IFHPR, (msgOffset + 4) & 0x3fff);
			break;
		}

		default:
		{
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

PPCKERNEL VOID FlushDCache(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    ULONG cacheSize;

    if (PowerPCBase->pp_CacheDisDFlushAll)
    {
        cacheSize = 0;
    }
    else
    {
        cacheSize = PowerPCBase->pp_CurrentL2Size;
    }

    cacheSize = (cacheSize >> 5) + CACHE_L1SIZE;
    //ULONG mem = ((PowerPCBase->pp_PPCMemSize - 0x400000) + PowerPCBase->pp_PPCMemBase);

    ULONG mem = 0;

    ULONG mem2 = mem;

    for (int i = 0; i < cacheSize; i++)
    {
        loadWord(mem);
        mem += CACHELINE_SIZE;
    }

    for (int i = 0; i < cacheSize; i++)
    {
        dFlush(mem2);
        mem2 += CACHELINE_SIZE;
    }

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL VOID KSetCache(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG flags, __reg("r5") APTR start, __reg("r6") ULONG length)
{
    ULONG value;

    switch (flags)
    {
        case CACHE_DCACHEOFF:
        {
            if (!(PowerPCBase->pp_CacheDState))
            {
                FlushDCache(PowerPCBase);

                value = getHID0();
                value &= ~HID0_DCE;
                setHID0(value);
                PowerPCBase->pp_CacheDState = -1;
            }
            break;
        }
        case CACHE_DCACHEON:
        {
            PowerPCBase->pp_CacheDState = 0;
            value = getHID0();
            value |= HID0_DCE;
            setHID0(value);
            break;
        }
        case CACHE_DCACHELOCK:
        {
            if ((start) && (length) && !(PowerPCBase->pp_CacheDLockState))
            {
                FlushDCache(PowerPCBase);
                ULONG iterations = (ULONG)start + length;
                ULONG mask = -32;
                start = (APTR)((ULONG)start & mask);
                ULONG mem = (ULONG)start;
                iterations = (((iterations + 31) & mask) - (ULONG)start) >> 5;

                for (int i = 0; i < iterations; i++)
                {
                    loadWord(mem);
                    mem += CACHELINE_SIZE;
                }

                value = getHID0();
                value |= HID0_DLOCK;
                setHID0(value);
                PowerPCBase->pp_CacheDLockState = -1;
            }
            break;
        }
        case CACHE_DCACHEUNLOCK:
        {
            PowerPCBase->pp_CacheDLockState = 0;
            value = getHID0();
            value &= ~HID0_DLOCK;
            setHID0(value);
            break;
        }
        case CACHE_DCACHEFLUSH:
        {
            if (!(PowerPCBase->pp_CacheDState) && !(PowerPCBase->pp_CacheDLockState))
            {
                if (PowerPCBase->pp_L2Size)
                {
                    if ((start) && (length))
                    {
                        ULONG iterations = (ULONG)start + length;
                        ULONG mask = -32;
                        start = (APTR)((ULONG)start & mask);
                        ULONG mem = (ULONG)start;
                        iterations = (((iterations + 31) & mask) - (ULONG)start) >> 5;

                        for (int i = 0; i < iterations; i++)
                        {
                            dFlush(mem);
                            mem += CACHELINE_SIZE;
                        }
                        sync();
                    }
                    else
                    {
                        FlushDCache(PowerPCBase);
                    }
                }
                else
                {
                    ULONG mem = 0;
                    for (int i = 0; i < CACHE_L1SIZE; i++)
                    {
                        loadWord(mem);
                        mem += CACHELINE_SIZE;
                    }
                }
            }
            break;
        }
        case CACHE_ICACHEOFF:
        {
            value = getHID0();
            value &= ~HID0_ICE;
            setHID0(value);
            break;
        }
        case CACHE_ICACHEON:
        {
            value = getHID0();
            value |= HID0_ICE;
            setHID0(value);
            break;
        }
        case CACHE_ICACHELOCK:
        {
            value = getHID0();
            value |= HID0_ILOCK;
            setHID0(value);
            break;
        }
        case CACHE_ICACHEUNLOCK:
        {
            value = getHID0();
            value &= ~HID0_ILOCK;
            setHID0(value);
            break;
        }
        case CACHE_DCACHEINV:
        {
            if ((start) && (length))
            {
                if (PowerPCBase->pp_L2Size)
                {
                    ULONG iterations = (ULONG)start + length;
                    ULONG mask = -32;
                    start = (APTR)((ULONG)start & mask);
                    ULONG mem = (ULONG)start;
                    iterations = (((iterations + 31) & mask) - (ULONG)start) >> 5;

                    for (int i = 0; i < iterations; i++)
                    {
                        dInval(mem);
                        mem += CACHELINE_SIZE;
                    }
                    sync();
                }
                else
                {
                    ULONG mem = 0;
                    for (int i = 0; i < CACHE_L1SIZE; i++)
                    {
                        loadWord(mem);
                        mem += CACHELINE_SIZE;
                    }
                }
            }
            break;
        }
        case CACHE_ICACHEINV:
        {
            if ((start) && (length))
            {
                ULONG iterations = (ULONG)start + length;
                ULONG mask = -32;
                start = (APTR)((ULONG)start & mask);
                ULONG mem = (ULONG)start;
                iterations = (((iterations + 31) & mask) - (ULONG)start) >> 5;

                for (int i = 0; i < iterations; i++)
                {
                    iInval(mem);
                    mem += CACHELINE_SIZE;
                }
                isync();
            }
            else
            {
                FlushICache();
            }
            break;
        }
    }
    if ((PowerPCBase->pp_DeviceID != DEVICE_MPC8343E) && (PowerPCBase->pp_DeviceID != DEVICE_MPC8314E))
    {
        switch (flags) //To prevent SDA_BASE
        {
            case  CACHE_L2CACHEON:
            {
                value = getL2State();
                value |= L2CR_L2E;
                setL2State(value);
                PowerPCBase->pp_CurrentL2Size = PowerPCBase->pp_L2Size;
                break;
            }
            case CACHE_L2CACHEOFF:
            {
                FlushDCache(PowerPCBase);
                value = getL2State();
                value &= ~L2CR_L2E;
                setL2State(value);
                PowerPCBase->pp_CurrentL2Size = 0;
                break;
            }
            case CACHE_L2WTON:
            {
                value = getL2State();
                value |= L2CR_L2WT;
                setL2State(value);
                break;
            }
            case CACHE_L2WTOFF:
            {
                value = getL2State();
                value &= ~L2CR_L2WT;
                setL2State(value);
                break;
            }
            case CACHE_TOGGLEDFLUSH:
            {
                PowerPCBase->pp_CacheDisDFlushAll = !PowerPCBase->pp_CacheDisDFlushAll;
                break;
            }
        }
    }
    return;
}

/********************************************************************************************
*
*     Entry point to print an exception error in a window.
*
*********************************************************************************************/

PPCKERNEL void CommonExcError(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct iframe* iframe)
{
    if ((iframe->if_Context.ec_ExcID == EXCF_INTERRUPT) || (iframe->if_Context.ec_ExcID == EXCF_DECREMENTER)
     || (iframe->if_Context.ec_ExcID == EXCF_TRACE)     || (iframe->if_Context.ec_ExcID == EXCF_PERFMON))
    {
        return;
    }

    ULONG* errorData = (ULONG*)(PowerPCBase->pp_PPCMemBase + (ULONG)(FIFO_END + 0x100));
    errorData[0]  = (ULONG)PowerPCBase->pp_ThisPPCProc->tp_Task.tc_Node.ln_Name;
    errorData[1]  = (ULONG)PowerPCBase->pp_ThisPPCProc;
    errorData[2]  = iframe->if_ExcNum;
    errorData[3]  = iframe->if_Context.ec_UPC.ec_SRR0;
    errorData[4]  = iframe->if_Context.ec_SRR1;
    errorData[5]  = getMSR();
    errorData[6]  = getHID0();
    errorData[7]  = getPVR();
    errorData[8]  = iframe->if_Context.ec_DAR;
    errorData[9]  = iframe->if_Context.ec_DSISR;
    errorData[10] = getSDR1();
    errorData[11] = getDEC();
    errorData[12] = getTBU();
    errorData[13] = getTBL();
    errorData[14] = iframe->if_Context.ec_XER;
    errorData[15] = iframe->if_Context.ec_CR;
    errorData[16] = iframe->if_Context.ec_FPSCR;
    errorData[17] = iframe->if_Context.ec_LR;
    errorData[18] = iframe->if_Context.ec_CTR;
    errorData[19] = iframe->if_Context.ec_GPR[0];
    errorData[20] = iframe->if_Context.ec_GPR[1];
    errorData[21] = iframe->if_Context.ec_GPR[2];
    errorData[22] = iframe->if_Context.ec_GPR[3];
    errorData[23] = iframe->if_BATs[0].ba_ibatu;;
    errorData[24] = iframe->if_BATs[0].ba_ibatl;
    errorData[25] = iframe->if_Context.ec_GPR[4];
    errorData[26] = iframe->if_Context.ec_GPR[5];
    errorData[27] = iframe->if_Context.ec_GPR[6];
    errorData[28] = iframe->if_Context.ec_GPR[7];
    errorData[29] = iframe->if_BATs[1].ba_ibatu;
    errorData[30] = iframe->if_BATs[1].ba_ibatl;
    errorData[31] = iframe->if_Context.ec_GPR[8];
    errorData[32] = iframe->if_Context.ec_GPR[9];
    errorData[33] = iframe->if_Context.ec_GPR[10];
    errorData[34] = iframe->if_Context.ec_GPR[11];
    errorData[35] = iframe->if_BATs[2].ba_ibatu;
    errorData[36] = iframe->if_BATs[2].ba_ibatl;
    errorData[37] = iframe->if_Context.ec_GPR[12];
    errorData[38] = iframe->if_Context.ec_GPR[13];
    errorData[39] = iframe->if_Context.ec_GPR[14];
    errorData[40] = iframe->if_Context.ec_GPR[15];
    errorData[41] = iframe->if_BATs[3].ba_ibatu;
    errorData[42] = iframe->if_BATs[3].ba_ibatl;
    errorData[43] = iframe->if_Context.ec_GPR[16];
    errorData[44] = iframe->if_Context.ec_GPR[17];
    errorData[45] = iframe->if_Context.ec_GPR[18];
    errorData[46] = iframe->if_Context.ec_GPR[19];
    errorData[47] = iframe->if_BATs[0].ba_dbatu;
    errorData[48] = iframe->if_BATs[0].ba_dbatl;
    errorData[49] = iframe->if_Context.ec_GPR[20];
    errorData[50] = iframe->if_Context.ec_GPR[21];
    errorData[51] = iframe->if_Context.ec_GPR[22];
    errorData[52] = iframe->if_Context.ec_GPR[23];
    errorData[53] = iframe->if_BATs[1].ba_dbatu;
    errorData[54] = iframe->if_BATs[1].ba_dbatl;
    errorData[55] = iframe->if_Context.ec_GPR[24];
    errorData[56] = iframe->if_Context.ec_GPR[25];
    errorData[57] = iframe->if_Context.ec_GPR[26];
    errorData[58] = iframe->if_Context.ec_GPR[27];
    errorData[59] = iframe->if_BATs[2].ba_dbatu;
    errorData[60] = iframe->if_BATs[2].ba_dbatl;
    errorData[61] = iframe->if_Context.ec_GPR[28];
    errorData[62] = iframe->if_Context.ec_GPR[29];
    errorData[63] = iframe->if_Context.ec_GPR[30];
    errorData[64] = iframe->if_Context.ec_GPR[31];
    errorData[65] = iframe->if_BATs[3].ba_dbatu;
    errorData[66] = iframe->if_BATs[3].ba_dbatl;

    struct MsgFrame* myFrame = KCreateMsgFramePPC(PowerPCBase);
    myFrame->mf_Identifier = ID_CRSH;
    KSendMsgFramePPC(PowerPCBase, myFrame);

    PowerPCBase->pp_ThisPPCProc->tp_Task.tc_State = TS_REMOVED;
    PowerPCBase->pp_ThisPPCProc->tp_Flags |= TASKPPCF_CRASHED;
    RemovePPC((struct Node*)PowerPCBase->pp_ThisPPCProc->tp_TaskPtr);
    SwitchPPC(PowerPCBase, iframe);
}
