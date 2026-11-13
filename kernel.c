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

/*
 * T1: AN EXTERNAL INTERRUPT DOES NOT RESTART THE TIME SLICE. Every exception
 * entry puts pp_Quantum back to pp_StdQuantum, and the exits of the decrementer
 * and external-interrupt exceptions wrote setDEC(decShorten(pp_Quantum)). For an
 * external interrupt that switched no task that restarted the whole slice, so
 * under 68k-to-PPC interrupts faster than a quantum the decrementer never
 * expired, and what only its arm does never ran: the decrementer handler list,
 * the pp_ReadyExc and pp_RemovedExc moves that SetExcHandler and RemExcHandler
 * wait for, and the pp_ExternalInt test that CauseInterrupt waits for. It also
 * threw away the short decrementers the library arms to get that arm quickly
 * (CauseDECInterrupt's 30, CauseInterrupt's 50) and a handler's own mtdec.
 *
 * NOW: when the task that was running is still running, the decrementer is
 * written only if the new value is SHORTER than what is left, and one that is
 * already past zero is left alone: its exception is delayed while MSR[EE] is 0
 * and comes with the rfi (MPC7410UM.pdf p. 98, p. 281). When a task was switched
 * the incoming task's slice is written, as before. The minima pp_Quantum
 * collects stay: the 0x1000 retry for interrupted library code or pp_Mutex or
 * Forbid, the WaitTime deadlines (TaskCheck), the incoming task's remaining
 * slice (SwitchPPC) and decShorten's timed handlers.
 *
 * THE ORIGINALS. 17.13b restarts the slice here, as this library did: its
 * external interrupt ends in .SlowReturn, "lwz r0,Quantum(r0) / mtdec r0"
 * (setupppc.p:3413-3414), or .QuickReturn, 0x1000, for an atomic task or
 * interrupted library code (:3409-3410, :2768-2780). WarpOS does not: its
 * external interrupt ends in "mtdec 0" with PPC_SCHEDULE set (powerpc.p:5767-5771,
 * :5800-5804), and its decrementer handler, finding the flag clear and the
 * planned time PPC_TBL not reached, only re-arms the remainder (:4416-4432), so
 * no external interrupt moves the next decrementer out. This gets that without
 * forcing the decrementer, and DIVERGES from WarpOS in that WarpOS also runs its
 * decrementer handler list after every external interrupt; from 17.13b rightly.
 * A flood that SWITCHES a task at every interrupt still writes the incoming
 * slice each time, at most one quantum. Test: tests/decstarve.
 */
static void decAfterExternal(struct PrivatePPCBase* PowerPCBase, struct TaskPPC* taskAtEntry)
{
    ULONG want = decShorten(PowerPCBase, PowerPCBase->pp_Quantum);

    if (PowerPCBase->pp_ThisPPCProc == taskAtEntry)
    {
        LONG left = (LONG)getDEC();

        if ((left <= 0) || (want >= (ULONG)left))
        {
            return;
        }
    }
    setDEC(want);
}

/********************************************************************************************
*
*	Entry point after kernel.s. This directs to the correct exception code.
*
*********************************************************************************************/

/*
 * THE FRAME CONTRACT, CHECKED. A LARGECONTEXT handler receives &if_Context,
 * which is the frame (IF_CONTEXT is 0), and what the exit installs is the
 * frame: these are the offsets the rebuild duplicates (recon stands alone),
 * libstructs.h at struct iframe. A wrong number here is a compile error.
 */
typedef char iframe_contract_holds[
    (__offsetof(struct iframe, if_BATs)     ==  964 &&
     __offsetof(struct iframe, if_Segments) == 1028 &&
     __offsetof(struct iframe, if_ExcNum)   == 1092 &&
     __offsetof(struct iframe, if_SDR1)     == 1096 &&
     sizeof(struct iframe)                == 1100) ? 1 : -1];

#ifdef EXC_TIMING
/*
 * THE TIMING BUILD (constants.h MMU_ZERO_TIME_*). Called at every C entry,
 * once the exception number is known: the PREVIOUS exception's stamps are
 * complete by now -- its T0..T2 were copied to TIME_PREV at its own entry,
 * its T3..T8 are still in place because this exception has not reached
 * them -- so its eight phase lengths are added up when its number is the
 * one asked for and its handler stamps were written. Then this one's
 * T0..T2 and number are kept and its handler stamps cleared.
 */
#define TIME_STAMP(n)   (*((volatile ULONG*)(MMU_ZERO_TIME_T0 + 4 * (n))) = getTBL())
static void excTimingAccount(ULONG excNum)
{
    volatile ULONG* t = (volatile ULONG*)MMU_ZERO_TIME_T0;
    volatile ULONG* p = (volatile ULONG*)MMU_ZERO_TIME_PREV;
    volatile ULONG* s = (volatile ULONG*)MMU_ZERO_TIME_SUMS;
    ULONG want = *((volatile ULONG*)MMU_ZERO_TIME_WANT);
    if (want && (p[3] == want) && t[3] && t[4])
    {
        ULONG st[9];
        st[0] = p[0];
        st[1] = p[1];
        st[2] = p[2];
        for (int i = 3; i < 9; i++)
        {
            st[i] = t[i];
        }
        for (int i = 0; i < 8; i++)
        {
            s[i] += st[i + 1] - st[i];
        }
        *((volatile ULONG*)MMU_ZERO_TIME_COUNT) += 1;
    }
    p[0] = t[0];
    p[1] = t[1];
    p[2] = t[2];
    p[3] = excNum;
    t[3] = 0;
    t[4] = 0;
}
#else
#define TIME_STAMP(n)
#endif

PPCKERNEL void Exception_Entry(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct iframe* iframe)
{
    PowerPCBase->pp_ExceptionMode = -1;

    /* the task running at entry, so that the exit of an external interrupt can tell whether one was switched in (T1); only the pointer is read */
    struct TaskPPC* taskAtEntry = PowerPCBase->pp_ThisPPCProc;

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
#ifdef EXC_TIMING
    excTimingAccount(iframe->if_ExcNum);
#endif

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
                        decAfterExternal(PowerPCBase, taskAtEntry);     //T1: no switch happened here, so it only shortens
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
                    /*
                     * Q5: THE MESSAGE INTERRUPT IS ACKNOWLEDGED BEFORE THE QUEUE IS READ.
                     * The write of IM0I came after the drain, so a frame the 68k posted
                     * after the drain's last read and before that write had its IM0I
                     * wiped while its slot stayed unread: it waited for the next frame's
                     * interrupt. Acknowledged first, a frame posted before the write is
                     * in the queue the drain reads (the 68k stores the slot and the head
                     * before it writes IMR0, which sets IM0I), and one posted after it
                     * sets IM0I again and interrupts again. IMISR is read once and the
                     * write clears IM0I alone. The EXCF_INTERRUPT handlers that
                     * CommonExcHandler runs below now see IM0I acknowledged
                     * (tests/killack looks at that). 17.13b acknowledges after the drain
                     * (setupppc.p:2707-2711) and not at all when the queue was empty at
                     * entry (:2682-2683): THIS DIVERGES, rightly. WarpOS has no such
                     * unit (a search of warpos/ for IM0I, IMISR and MPC8343 finds no
                     * line, 12 Nov 2026).
                     */
                    ULONG imisr = loadPCI(IMMR_ADDR_DEFAULT, IMMR_IMISR);

                    if (imisr & IMMR_IMISR_IM0I)
                    {
                        storePCI(IMMR_ADDR_DEFAULT, IMMR_IMISR, IMMR_IMISR_IM0I);
                    }
                    if (imisr & IMMR_IMISR_IDI)
                    {
                        storePCI(IMMR_ADDR_DEFAULT, IMMR_IDR, 1);
                        CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcInterrupt);
                    }
                    struct MsgFrame* msgFrame;

                    while (msgFrame = KGetMsgFramePPC(PowerPCBase))
                    {
                        AddTailPPC((struct List*)&PowerPCBase->pp_MsgQueue, (struct Node*)msgFrame);
                    }
                    break;
		        }
                case DEVICE_MPC8314E: //the 68k's CausePPCInterrupt: kf_Doorbell, below (E9)
                {
                    /*
                     * Q5, the PCIe M1's half: the order of the MPC8343E's arm above. The
                     * write-back of CSMISR clears the bits that were set when it was read,
                     * and a frame posted after it sets its bit again. 17.13b has no PCIe M1
                     * (a search of src_173b for MPC8314 and PEX_ finds no line) and WarpOS
                     * none (warpos/, 12 Nov 2026): NO COUNTERPART. No PCIe M1 has run this
                     * order.
                     */
                    writememLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_IMBCR, 0);
                    writememLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_CSMISR, readmemLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_CSMISR));

                    /*
                     * E9: THE PCIe M1's CausePPCInterrupt. The 68k adds 1 to kf_Doorbell and then
                     * sets IMBCR[READY], the interrupt a frame post raises; a count that differs
                     * from kf_DoorbellSeen, which only this code writes, runs the EXCF_INTERRUPT
                     * handlers, as the MPC8343E's doorbell does. It is read AFTER the clears
                     * above: a doorbell whose READY came before them had its count written before
                     * them too, and one that comes later raises the interrupt again. A count and
                     * not a flag: this code never stores into the 68k's word, so no doorbell is
                     * wiped, and two close together may share one call, as on the other cards.
                     * 17.13b has no PCIe M1 (powerpc.s:4411-4433: K1, Harrier, MPC107) and WarpOS's
                     * CausePPCInterrupt is Phase5's InvokeInterrupt (powerpc.s:1446-1458): NO
                     * COUNTERPART.
                     */
                    struct killFIFO* bellFIFO = (struct killFIFO*)((ULONG)(PowerPCBase->pp_PPCMemBase + FIFO_END));
                    ULONG bell = bellFIFO->kf_Doorbell;

                    if (bell != bellFIFO->kf_DoorbellSeen)
                    {
                        bellFIFO->kf_DoorbellSeen = bell;
                        CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcInterrupt);
                    }

                    struct MsgFrame* msgFrame;

                    while (msgFrame = KGetMsgFramePPC(PowerPCBase))
                    {
                        AddTailPPC((struct List*)&PowerPCBase->pp_MsgQueue, (struct Node*)msgFrame);
                    }
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
                        decAfterExternal(PowerPCBase, taskAtEntry);     //T1: no switch happened here, so it only shortens
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
                if (currTask->tp_Task.tc_State == TS_ATOMIC)            //never true: no tc_State write in lib/ stores TS_ATOMIC; a running atomic task is kept in SwitchPPC, as WarpOS (R3)
                {
                    break;
                }
            }

            HandleDeferred(PowerPCBase);    //what a handler left: FIXES.md "#1 PART B DESIGNED"
            HandleMsgs(PowerPCBase);
            TaskCheck(PowerPCBase);
            SwitchPPC(PowerPCBase, iframe);
            break;
        }
        case VEC_DECREMENTER:
        {
            CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcDecrementer);

            /*
             * CauseInterrupt (E5): THE EXCF_INTERRUPT HANDLERS, AFTER THE
             * DECREMENTER LIST. Only the external arms above ran
             * pp_ExcInterrupt, so a handler set with EXCF_INTERRUPT was never
             * called for CauseInterrupt, which arms this decrementer. The flag is
             * cleared before the list runs, as WarpOS (powerpc.p:3949-3962) and
             * 17.13b (setupppc.p:2528-2533), and the handlers see the exception
             * ID they asked for: CommonExcHandler matches them by
             * "ed_ExcID & ec_ExcID" (below) and _SmallExcHandler hands the
             * handler that ID, so it is EXCF_INTERRUPT for the call and the
             * decrementer's again after it. WarpOS runs the same list from
             * Exception5 with XC_EXCID = EXCF_INTERRUPT (powerpc.p:6136-6168).
             * CommonExcError returns for both IDs, so a list nobody is on adds
             * no error window. CauseInterrupt waits for the flag.
             */
            if (PowerPCBase->pp_ExternalInt)
            {
                PowerPCBase->pp_ExternalInt = 0;
                iframe->if_Context.ec_ExcID = EXCF_INTERRUPT;
                CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcInterrupt);
                iframe->if_Context.ec_ExcID = EXCF_DECREMENTER;
            }

            CPUStats(PowerPCBase);

            ULONG currAddress = iframe->if_Context.ec_UPC.ec_SRR0;
            if ((PowerPCBase->pp_LowerLimit <= currAddress) && (currAddress < PowerPCBase->pp_UpperLimit))
            {
                PowerPCBase->pp_Quantum = 0x1000;
                break;
            }

            /*
             * T6: THE TWO MOVES BELOW COME AFTER THE WINDOW TEST ABOVE. The
             * moves are not interrupted themselves (an exception runs with
             * EE off); what was not protected is the code they interrupt.
             * LinkExcList (supportppc.c, one insert per entry of a
             * SetExcHandler) and RemExcHandler (functionsppc.c) put an entry
             * on pp_ReadyExc or pp_RemovedExc with myAddHeadPPC, in user mode
             * with EE on, under pp_Mutex. As written, myAddHeadPPC points the
             * list head at the new node BEFORE it sets the node's own links
             * (functionsppc.c). pp_Mutex is a flag and not an interrupt mask:
             * this arm tests it below, to keep a task switch away, after these
             * loops. So a decrementer that landed between those stores ran the
             * loops on a list whose head was a node with no links yet, and
             * RemHeadPPC followed them. The window above is the code between
             * the jump-table targets of InsertPPC and FindNamePPC
             * (pp_LowerLimit and pp_UpperLimit, setupppc.c), which holds
             * myAddHeadPPC: it is how this arm tells that the interrupted
             * code is inside a list function, and the loops ran ahead of it.
             * A caller outside the window has its entries moved at this tick,
             * as before; one inside it at the next, 0x1000 timebase ticks on
             * (the quantum set above). The moves stay AHEAD of the mutex and
             * Forbid test below, so that a SetExcHandler or RemExcHandler that
             * holds pp_Mutex, or runs under Forbid, still has its entries
             * moved and its wait for EXCF_ACTIVE ends.
             * 17.13b has the same order and the same exposure: its AddHeadPPC
             * stores the head first (ppcfunctions.p:598-603) and its moves
             * (setupppc.p:4316 and :4423) come before .StartQ (:2766-2780), which
             * loads the atomic flag (:2768) and the window limits (:2774, :2778);
             * a search of the whole 17.13b tree finds no other load of either,
             * and every other use of the flag is a lock or unlock call
             * (ppcfunctions.p, 90 sites, AtomicTest and AtomicDone). WarpOS
             * tests FORBID first (powerpc.p:4187-4220) and inserts under FORBID
             * (powerpc.p:11425-11428, :11480-11484). tests/excstage.
             */
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

            if ((PowerPCBase->pp_Mutex) || (PowerPCBase->pp_FlagForbid))
            {
                PowerPCBase->pp_Quantum = 0x1000;
                break;
            }

            struct TaskPPC* currTask = PowerPCBase->pp_ThisPPCProc;

            if (currTask)
            {
                if (currTask->tp_Task.tc_State == TS_ATOMIC)            //never true: no tc_State write in lib/ stores TS_ATOMIC; a running atomic task is kept in SwitchPPC, as WarpOS (R3)
                {
                    break;
                }
            }

            HandleDeferred(PowerPCBase);    //what a handler left: FIXES.md "#1 PART B DESIGNED"
            HandleMsgs(PowerPCBase);
            TaskCheck(PowerPCBase);
            SwitchPPC(PowerPCBase, iframe);
            break;
        }
        case VEC_DATASTORAGE:
        {
            /*
             * ENV:sonnet/EnMemExc DECIDES WHO SEES THE FAULT FIRST (the user's,
             * 31 Oct 2026; it replaces sonnet/EnDAccessExc). A DSI here is a
             * real fault or the PPC reaching Amiga memory, which it cannot do
             * itself and the 68k proxy below serves. WarpOS reaches Amiga
             * memory directly, so all its DSIs are real faults and its
             * handlers see every one (Exception3, powerpc.p:5934-6010).
             * - 1 (pp_EnDAccessExc set): the task's DACCESS handlers first, as
             *   WarpOS; EXCRETURN_ABORT claims the fault, NORMAL leaves it to
             *   the proxy. ReFused needs this for its guest's device accesses.
             * - 0, the default: the proxy first, and the handlers only for
             *   what it refuses, at the end of this arm -- 17.13b's default
             *   (src_173b/setupppc.p:5370-5372, .HaltDSI :5760-5765).
             * 17.15 called them only under EnDAccessExc and ignored their
             * answer; from 24 to 31 Oct they ran first on every DSI.
             * FIXES.md "EnMemExc".
             */
            if (PowerPCBase->pp_EnDAccessExc)
            {
                if (CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcDAccess) == EXCRETURN_ABORT)
                {
                    break;
                }
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

                        ULONG answered = 1;

                        if (!(myData.dm_LoadFlag))
                        {
                            /*
                             * Q3: THE WAIT FOR THE 68k'S ANSWER TO A LOAD ENDS. This was
                             * the bare loop "while (mf_Identifier != ID_DONE);", so a 68k
                             * that did not answer -- inside Disable() for seconds, or with
                             * the message lost -- held this CPU here with EE off: no
                             * decrementer, no other task, and nothing to say why. It is now
                             * bounded by the timebase, PROXY_WAIT_SECONDS (constants.h),
                             * the same time on every card; 17.13b bounds it by a count,
                             * 0x400000 polls, a different time on every CPU
                             * (setupppc.p:5869-5881). On expiry the load is refused as
                             * 17.13b refuses it: 'ETIM' (0x4554494D) goes in the saved r0,
                             * which the crash window prints, and the code falls out of this
                             * block to the tail below -- the task's DACCESS handlers when
                             * EnMemExc is 0, then CommonExcError -- where it used to add 4
                             * to SRR0 and go on. The message frame stays with the 68k,
                             * which answers it and frees it whenever it gets to the message
                             * (support68k.c, ID_GETV). A frame the 68k has already freed
                             * (ID_FREE written over ID_DONE) before the poll saw ID_DONE
                             * ends here the same way; tests/proxytime floods loads to see
                             * whether a healthy machine ever gets here. WarpOS has no
                             * proxy: it reaches Amiga memory itself.
                             */
                            ULONG waitStart = getTBL();
                            ULONG waitTicks = (PowerPCBase->pp_BusClock >> 2) * PROXY_WAIT_SECONDS;

                            while ((myFrame->mf_Identifier != ID_DONE) && ((getTBL() - waitStart) < waitTicks))
                            {
                            }

                            if (myFrame->mf_Identifier != ID_DONE)
                            {
                                iframe->if_Context.ec_GPR[0] = 0x4554494D;      //'ETIM', as 17.13b
                                answered = 0;
                            }
                            else
                            {
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
                        }
                        if (answered)
                        {
                            iframe->if_Context.ec_UPC.ec_SRR0  += 4;
                            //PowerPCBase->pp_Quantum = getDEC() + 1000; //Do not start quantum all over
                            break;
                        }
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
            if (!(PowerPCBase->pp_EnDAccessExc))
            {
                /* EnMemExc 0: what the proxy refused -- outside its range, from
                 * an SRR0 at or below 0x10000, or an instruction DoDataStore
                 * cannot emulate -- reaches the handlers before the crash. */
                if (CommonExcHandler(PowerPCBase, iframe, (struct List*)&PowerPCBase->pp_ExcDAccess) == EXCRETURN_ABORT)
                {
                    break;
                }
            }
            CommonExcError(PowerPCBase, iframe);
            break;
        }
        case VEC_PROGRAM:
        {
            /*
             * SUPER() IS KNOWN BY ITS ADDRESS: the one mfpvr at SuperLocation
             * (supportppc.s), as WarpOS's Exc7 compares SRR0 with SuperLocation
             * (warp.p:4588-4602) and 17.13b's .Private with ViolationAddress
             * (setupppc.p:6302-6317). The address comes from the base, where
             * libinit.c stored it: this hunk cannot take it itself. r0 = 0 is
             * the status SuperTrap returns through "mr r3,r0". Any other
             * program exception, whatever r4 holds, goes to the handlers. It
             * used to test GPR[4] == SUPERKEY. FIXES.md "ITEM 3, Super()",
             * 31 Oct 2026.
             */
            if ((PowerPCBase->pp_SuperLocation) && (iframe->if_Context.ec_UPC.ec_SRR0 == PowerPCBase->pp_SuperLocation))
            {
                iframe->if_Context.ec_UPC.ec_SRR0  += 4;
                iframe->if_Context.ec_SRR1         &= ~PSL_PR;
                iframe->if_Context.ec_GPR[0]        = 0;            //the SuperKey, returned by SuperTrap
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

                /*
                 * T8: A NEW TASK'S VECTOR UNIT STARTS IN NON-JAVA MODE, as the
                 * CPU does. The exit loads VSCR from this frame (kernel.s,
                 * _LoadFrame), and a task's frame is zero, so every task
                 * began with VSCR[NJ] = 0, Java mode, where the MPC7410 takes
                 * the AltiVec assist exception (vector 0x1600) for a denormal
                 * input or output of vaddfp, vsubfp, vmaddfp and vnmsubfp
                 * (MPC7410UM Table 7-7, PDF p. 387; 4.6.17, p. 285) -- and
                 * this kernel has no 0x1600 case, so the task was ended. The
                 * 7410 resets to VSCR = 0x0001_0000 and "defaults to non-Java
                 * mode" (Table 2-21, PDF p. 127; 7.1.3, p. 386). 17.13b's tasks
                 * kept that: its .VMXUnav only sets the VEC bit and touches no
                 * VSCR (setupppc.p:5253-5335), though it saves and restores
                 * VSCR per task (:3808-3909, :3981-3992); 17.15 lost it. THIS
                 * MATCHES 17.13b in effect, not in mechanism. WarpOS has no
                 * AltiVec: NO COUNTERPART. Word 3 is the one mtvscr reads and
                 * mfvscr writes. Test: tests/ctxswitch.
                 */
                iframe->if_VSCR[3] = VSCR_NJ;
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

            /* the six calls are KSystemCall's, which a handler calls
             * directly (REVIEW-LIB.md K1); r3 comes back as it did: the frame
             * for CREATEMSG and GETMSG, untouched for the other four */
            switch (sc->sc_Function)
            {
                case SC_CREATEMSG:
                case SC_GETMSG:
                {
                    iframe->if_Context.ec_GPR[3] = KSystemCall(PowerPCBase, sc);
                    break;
                }
                case SC_SENDMSG:
                case SC_FREEMSG:
                case SC_SETCACHE:
                case SC_GETCPUSTATE:
                {
                    KSystemCall(PowerPCBase, sc);
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
    /*
     * THE DECREMENTER IS PROGRAMMED WHERE WarpOS PROGRAMS IT AND NOWHERE ELSE:
     * at the end of the decrementer exception, after the scheduler and the
     * DEC handler list (powerpc.p:5315), and in the external interrupt, where
     * WarpOS forces a reschedule with "mtdec 0" (powerpc.p:5769, :5802) and
     * this library switches tasks itself (SwitchPPC above). Those are the
     * only two cases that switch tasks. Every other exception leaves DEC
     * counting. Until 24 Oct 2026 it was set here after ALL of them, from the
     * pp_StdQuantum Exception_Entry puts back first, so every exception
     * restarted the time slice: a task taking exceptions faster than its
     * quantum was never preempted and no timed handler ran -- iFUSION's guest
     * traps on every privileged instruction -- and a handler's own DEC write
     * was thrown away: iFUSION's mtdec emulation sets DEC to 100 at 0x01DDAC
     * to be called back. FIXES.md "STAGE 5".
     *
     * T1: the external interrupt no longer restarts the slice. A decrementer
     * exception writes the next slice as ever; an external interrupt that
     * left the same task running only SHORTENS what is left (decAfterExternal,
     * above), and writes the incoming task's slice when one was switched in.
     */
    if (ExceptionVector == VEC_DECREMENTER)
    {
        setDEC(decShorten(PowerPCBase, PowerPCBase->pp_Quantum));
    }
    else if (ExceptionVector == VEC_EXTERNAL)
    {
        decAfterExternal(PowerPCBase, taskAtEntry);
    }
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
        /*
         * E6 (REVIEW-LIB.md): THE GATE IS THE CODE, NOT THE DATA. This test read
         * tc_ExceptData and then called tc_ExceptCode without a look at it. A task
         * whose handler was in tc_ExceptCode and whose tc_ExceptData was NULL
         * (SetExceptPPC with flag 0 hands the structure's own value to r2) had its
         * armed signals left pending and its handler unrun; a task with
         * tc_ExceptData set (SetExceptPPC with flag 1 stores r2) and tc_ExceptCode
         * NULL had the kernel call address 0 in supervisor mode at its first armed
         * signal. WarpOS tests the code (powerpc.p:4011-4013) and loads r2 from
         * tc_ExceptData whatever it holds (:4031), and its autodoc calls
         * tc_ExceptCode the handler: THIS MATCHES WarpOS and DIVERGES from 17.13b
         * and 17.15, which test the data (setupppc.p:3637-3639, src_175/kernel.c:475).
         * A NULL code leaves the signals pending as ordinary ones, and the
         * waiting-task scan at the end of this function wakes a sleeper on them in
         * this pass. Test: tests/excgate.
         */
        if ((PowerPCBase->pp_TaskExcept) && (PowerPCBase->pp_TaskExcept == currTask) &&
            (currTask->tp_Task.tc_ExceptCode) && (mask = currTask->tp_Task.tc_SigRecvd & currTask->tp_Task.tc_SigExcept))
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

    /*
     * pp_TaskExcept IS CLEARED ON EVERY PASS, as 17.13b's kernel does
     * (setupppc.p:3631-3634), whether or not it named the current task.
     * CheckExcSignal waits for it to clear (supportppc.c:379), and nothing
     * cleared it, so a SignalPPC that hit a task's exception signals hung its
     * sender. FIXES.md "#1 PART B DESIGNED, AND A THIRD DEFECT (C)", 1 Nov 2026.
     */
    PowerPCBase->pp_TaskExcept = 0;

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

            /*
             * T9, REVIEW-LIB.md: THE WOKEN TASK GOES IN BY EFFECTIVE PRIORITY, as every other
             * insertion into pp_ReadyTasks does: SwitchPPC with priInsert (below) and CreateTaskPPC,
             * SignalPPC and SetTaskPriPPC through InsertOnPri. EnqueuePPC sorts on ln_Pri, which the
             * priority rule does not write, so a task that had used little CPU went behind every
             * ready task of its ln_Pri, and the scheduler, which looks at the head only, ran those
             * first: the delay was a quantum for each ready task ahead of it. WarpOS wakes with
             * EnqueueTask under NEWSCHEDULER (powerpc.p:4515-4526), which orders by dynamic priority
             * and applies the ceiling (:3047-3109); 17.13b appends to the tail of the list
             * (setupppc.p:3256-3264).
             */
            priInsert(PowerPCBase, &PowerPCBase->pp_ReadyTasks, currWTask);
        }
        currWTask = nxtWTask;
    }
    return;
}

/********************************************************************************************
*
*	WHAT AN EXCEPTION HANDLER LEFT FOR THE KERNEL (WarpOS: powerpc.p:4050-4068 and
*	:4467-4514). Called from the external and decrementer arms after their pp_Mutex /
*	pp_FlagForbid guard, so no task is inside the lists it touches.
*	1. The port a task handed over (supportppc.c's MergeIntMsgs): its internal list
*	   onto mp_MsgList, then the flag cleared, which releases the task.
*	2. The signals a handler's SignalPPC queued: each task's tp_Link off
*	   pp_TaskLinks, tl_Used cleared; exception signals marked for TaskCheck as
*	   CheckExcSignal marks them (pp_TaskExcept); the rest ORed into tc_SigRecvd,
*	   where TaskCheck wakes a waiting task whose tc_SigWait they hit, and a task
*	   between its Wait and the switch (TS_CHANGING) kept running, as mySignalPPC
*	   does. A task already TS_REMOVED is only taken off.
*	FIXES.md "#1 PART B DESIGNED", 1 Nov 2026.
*
*********************************************************************************************/

PPCKERNEL void HandleDeferred(__reg("r3") struct PrivatePPCBase* PowerPCBase)
{
    if (PowerPCBase->pp_FlagPortInUse)
    {
        struct MsgPortPPC* port = PowerPCBase->pp_CurrentPort;
        struct Node* msg;

        while (msg = RemHeadPPC(&port->mp_IntMsg))
        {
            AddTailPPC((struct List*)&port->mp_Port.mp_MsgList, msg);
        }
        PowerPCBase->pp_FlagPortInUse = 0;
    }

    struct TaskLink* link;

    while (link = (struct TaskLink*)RemHeadPPC((struct List*)&PowerPCBase->pp_TaskLinks))
    {
        struct TaskPPC* task = (struct TaskPPC*)link->tl_Task;
        ULONG signals = link->tl_Sig;

        link->tl_Used = 0;

        if (task->tp_Task.tc_State == TS_REMOVED)
        {
            continue;
        }

        ULONG sigmask = (task->tp_Task.tc_SigRecvd | signals) & task->tp_Task.tc_SigExcept;

        if (sigmask)
        {
            task->tp_Task.tc_SigRecvd |= sigmask;
            signals &= ~sigmask;
            PowerPCBase->pp_TaskExcept = task;
        }

        task->tp_Task.tc_SigRecvd |= signals;

        if (task->tp_Task.tc_State == TS_CHANGING)
        {
            task->tp_Task.tc_State = TS_RUN;
        }
    }
    return;
}

/********************************************************************************************
*
*	A TASK THAT IS BEING REMOVED KEEPS ITS STATE (T3, REVIEW-LIB.md). HandleMsgs
*	wakes the task a frame is for: RUN if it is the running task, READY if not.
*	DeleteTaskPPC on the running task, and KillTask, set TS_REMOVED, ask for a
*	decrementer interrupt and spin in TaskHalt; the pass that follows runs
*	HandleMsgs BEFORE SwitchPPC. A frame for the task handled there (the 68k's
*	ID_END, an ID_DONE for an async call, an ID_XPPC) turned TS_REMOVED back into
*	RUN: SwitchPPC, which files a task on pp_RemovedTasks by the state it finds,
*	then left it running -- a CPU hog off pp_AllTasks that Kryten never frees,
*	and with TASKATTR_ATOMIC one that keeps the CPU for ever (the R3 rule comes
*	before the TS_REMOVED test in SwitchPPC). The same overwrite on a task
*	already on pp_RemovedTasks breaks the test SetTaskPriPPC and SignalPPC make.
*
*	WarpOS wakes a task only from TS_WAIT (to READY) and TS_CHANGING (to RUN), so
*	a removed task is left as it is (powerpc.p:4484-4514, :10804-10828).
*	HandleDeferred has the test above (TS_REMOVED, "continue"). 17.13b stores the
*	state without one (setupppc.p:3003-3008, :3109-3115): THIS DIVERGES from it.
*
*	A MACRO, NOT A FUNCTION: kernel.c's statics carry no section, and a call
*	out of the kernel hunk would land where the callee used to be.
*
*********************************************************************************************/

#define MSG_SETSTATE(task, state)                                   \
    do                                                              \
    {                                                               \
        if ((task)->tp_Task.tc_State != TS_REMOVED)                 \
        {                                                           \
            (task)->tp_Task.tc_State = (state);                     \
        }                                                           \
    } while (0)

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
                        MSG_SETSTATE(myTask, (myTask == PowerPCBase->pp_ThisPPCProc) ? TS_RUN : TS_READY);
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

                MSG_SETSTATE(sigTask, (sigTask == PowerPCBase->pp_ThisPPCProc) ? TS_RUN : TS_READY);
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

                MSG_SETSTATE(sigTask, (sigTask == PowerPCBase->pp_ThisPPCProc) ? TS_RUN : TS_READY);
                break;
            }
            case ID_LLPP:
            {
                struct PrivateTask* myTask = (struct PrivateTask*)PowerPCBase->pp_ThisPPCProc;

                if ((myTask) && (myTask->pt_Mirror68K == (struct Task*)currMsg->mf_Arg[0]))
                {
                    MSG_SETSTATE(&myTask->pt_Task, TS_RUN);
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
                            MSG_SETSTATE(&currTask->pt_Task, TS_READY);
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
        if ((currTask->tp_Task.tc_State == TS_RUN) && (currTask->tp_Flags & TASKPPCF_ATOMIC))
        {
            break;      //R3: a RUNNING atomic task keeps the CPU until it waits or ends -- no switch, not to a new task either -- as WarpOS "bne .done" (powerpc.p:4559-4566), before TS_REMOVED as there (:4569). FIXES.md "CODE REVIEW OF lib/"
        }
        if (currTask->tp_Task.tc_State == TS_REMOVED)
        {
            AddTailPPC((struct List*)&PowerPCBase->pp_RemovedTasks, (struct Node*)currTask);
            if (currTask->tp_Link.tl_Used)  //a handler's queued signal, off before its memory goes
            {
                RemovePPC((struct Node*)&currTask->tp_Link);
                currTask->tp_Link.tl_Used = 0;
            }
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

    /*
     * T12, REVIEW-LIB.md: THE NEW TASK GETS A QUANTUM. The 68k side zeroes this block
     * (myCRunPPC, functions68k.c) and CreateTaskPPC stores pp_StdQuantum in the field
     * (functionsppc.c), but this path stored none, so a task started by a 68k program's first
     * RunPPC kept 0 until CPUStats' next pass that sets quanta, up to 24 passes: the decrementer
     * arm skips a task whose quantum is 0 and priShouldSwitch reads its slice as used at once.
     * WarpOS stores RESCH_NEWTASK in every task it makes (powerpc.p:9328) and 17.13b stores
     * sonnet_Quantum (ppcfunctions.p:5284-5285), both in the task's own creation.
     */
    newTask->nt_Task.pt_Task.tp_Quantum = PowerPCBase->pp_StdQuantum;

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
    newFrame->if_SDR1 = 0;                  /* the library's table, kernel.s exit */

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

PPCKERNEL ULONG CommonExcHandler(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct iframe* iframe, __reg("r5") struct List* excList)
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
                    TIME_STAMP(3);
                    status = ExcHandler(currNode->ed_Data, &iframe->if_Context);
                    TIME_STAMP(4);
                    storeR2(tempR2);
                }
                else if (currNode->ed_Flags & EXCF_SMALLCONTEXT)
                {
                    TIME_STAMP(3);
                    status = SmallExcHandler(currNode, iframe);
                    TIME_STAMP(4);
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
    return status;              /* the DSI case asks: ABORT claimed the fault */
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
    /*
     * A FRAME ONLY WHEN THE OUTBOUND FREE LIST HAS ONE (Q4, REVIEW-LIB.md).
     * This popped the tail with no head test, on every bridge. The DSI
     * proxy's stores post a frame each and do not wait, and debug output's
     * frames stay on MasterControl's port until printed, so the list could
     * run empty; the next pop then returned a frame from 4096 pops earlier,
     * perhaps still queued for the 68k, and it was overwritten and posted
     * twice. Now it waits while head == tail -- the 68k frees frames on its
     * own, so the wait ends -- then syncs before reading the slot, as
     * 17.13b's .ContCreateH (ppcfunctions.p:3109-3110). 17.13b has no empty
     * test (:3074-3110): THIS DIVERGES, rightly. WarpOS has no fixed ring:
     * its jobs are allocated per call (AllocJob, powerpc.s:4395; PPCAllocJob,
     * powerpc.p:15289), NO COUNTERPART. Test: tests/storeflood.
     */
	ULONG msgFrame = 0;

    switch (PowerPCBase->pp_DeviceID)
	{
		case DEVICE_HARRIER:
		{
			ULONG msgOffset = readmemLongPPC(PPC_XCSR_BASE, XCSR_MIOFT);
            while (readmemLongPPC(PPC_XCSR_BASE, XCSR_MIOFH) == msgOffset)
            {
                //Q4: the list is empty until the 68k frees a frame
            }
            sync();
            msgFrame = readmemLongPPC(msgOffset, 0);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIOFT, (msgOffset + 4) & 0xffff3fff);
            break;
		}

		case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
		{
			struct killFIFO* myFIFO = (struct killFIFO*)((ULONG)(PowerPCBase->pp_PPCMemBase + FIFO_END));
			ULONG msgOffset = myFIFO->kf_MIOFT;
			while (*((volatile ULONG*)&myFIFO->kf_MIOFH) == msgOffset)
			{
				//Q4: as above; the 68k writes kf_MIOFH, so it is read anew each pass
			}
			sync();
			msgFrame = *((ULONG*)msgOffset);
			myFIFO->kf_MIOFT = (myFIFO->kf_MIOFT + 4) & 0xffff3fff;
			break;
		}

		case DEVICE_MPC107:
		{
			ULONG msgOffset = loadPCI(PPC_EUMB_BASE, MPC107_OFTPR);
            while (loadPCI(PPC_EUMB_BASE, MPC107_OFHPR) == msgOffset)
            {
                //Q4: as above
            }
            sync();
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

PPCKERNEL ULONG KSystemCall(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SysCall* sc)
{
    /*
     * THE SIX SYSTEM CALLS, in one place. A task reaches them by "sc", through
     * the VEC_SYSTEMCALL arm of Exception_Entry. An exception handler calls
     * this directly, by the address libinit.c stores in pp_KSystemCall
     * (supportppc.c's CallKernel): it is already in supervisor mode inside the
     * kernel, and an "sc" from there re-entered the kernel and clobbered the
     * interrupted task's saved state -- REVIEW-LIB.md K1. Returns the frame for
     * SC_CREATEMSG and SC_GETMSG, 0 for the other four.
     */
    switch (sc->sc_Function)
    {
        case SC_CREATEMSG:
        {
            return (ULONG)KCreateMsgFramePPC(PowerPCBase);
        }
        case SC_GETMSG:
        {
            return (ULONG)KGetMsgFramePPC(PowerPCBase);
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
        case SC_GETCPUSTATE:
        {
            /*
             * C7, REVIEW-LIB.md: THE THREE FIELDS GetInfo READS, REFRESHED AT ITS
             * CALL. pp_CPUHID0, pp_CPUSDR1 and pp_L2State are stored at every
             * exception entry (Exception_Entry, above), and a SetCache that
             * changes HID0 or L2CR traps into the kernel, whose entry stores
             * them BEFORE the arm runs: a GetInfo straight after it answered
             * the state from before the call, until the next exception, up to
             * a quantum later. 17.13b's GetInfo goes to supervisor mode, stores
             * HID0, SDR1 and (not on a MPC834x) L2CR in the base, and comes
             * back (ppcfunctions.p:1138-1154); WarpOS's reads HID0 in
             * supervisor mode at the call (powerpc.p:12016-12034): THIS
             * MATCHES both. The stores are the entry's own, so that a handler,
             * which calls here directly, gets the same. The e300 cards have no
             * L2CR.
             */
            PowerPCBase->pp_CPUSDR1 = getSDR1();
            PowerPCBase->pp_CPUHID0 = getHID0();

            if ((PowerPCBase->pp_DeviceID != DEVICE_MPC8343E) && (PowerPCBase->pp_DeviceID != DEVICE_MPC8314E))
            {
                PowerPCBase->pp_L2State = getL2State();
            }
            break;
        }
    }
    return 0;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL VOID FlushDCache(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG withL2)
{
    /*
     * C3 (REVIEW-LIB.md), 7 Nov 2026. THE WHOLE-CACHE FLUSH BY DISPLACEMENT.
     * It reads a range from EA 0 that is larger than the cache, so every
     * line the program left dirty is cast out to memory, then flushes the
     * range. Three changes to 17.15's one pass (load, then dcbf):
     *
     * - The range is flushed FIRST. Part of it is always cached and dirty --
     *   page zero (the tick counter, SRR0 and SRR1 share its first line)
     *   and 0x2000-0x21DF, written at every exception entry -- and a load
     *   that hits a cached line does not displace anything: it only
     *   reorders the pseudo-LRU, so an old line can stay. Flushed first,
     *   every load below is a miss that takes a way.
     * - The range is the L2 plus TWO times the L1 (17.15: plus one). The
     *   8-way tree-PLRU L1 fills invalid ways first, and one pass of eight
     *   lines per set does not always reach every way; sixteen distinct
     *   lines per set do. On a model of that cache, over every PLRU state
     *   and every pattern of empty ways: an old line stayed in 0 of 32,640
     *   cases (after a first flush, one pass of eight left one in 72%;
     *   17.15's pass, with one range line already cached, in 14%).
     * - withL2 forces the L2 into the range even with DisL2Flush set
     *   (pp_CacheDisDFlushAll). DCACHEOFF and L2CACHEOFF need it: the L1
     *   stops using the L2 when it is switched off, and L2CACHEOFF drops
     *   whatever the L2 holds. A plain full flush and DCACHELOCK still
     *   obey DisL2Flush.
     *
     * AGAINST THE ORIGINALS. 17.13b displaces from the top 4 MB of the card's
     * memory and, on a 7410, flushes the L2 with its hardware flush
     * (ppcfunctions.p:1536-1569, :1582-1585); it too skips the L2 under
     * DisL2Flush for every caller (:1529-1534). WarpOS flushes by dcbz over a
     * private buffer of the L1's size (warp.p:323-349, :2840-2858) and has no
     * L2. THIS DIVERGES from both: the range is flushed before and after,
     * and sized to leave no line behind. Test: tests/cacheflush.
     */
    ULONG lines;
    ULONG mem;
    ULONG base = 0;
    ULONG i;

    if ((PowerPCBase->pp_CacheDisDFlushAll) && !(withL2))
    {
        lines = 0;
    }
    else
    {
        lines = PowerPCBase->pp_CurrentL2Size >> 5;
    }

    lines += 2 * CACHE_L1SIZE;

    /*
     * S8 (REVIEW-LIB.md): THE RANGE MUST BE MAPPED. It starts at EA 0, the kernel's BAT0, which is 2 MB
     * long (setupppc.c:315-318); beyond it nothing is mapped, and a load there is a DSI inside the kernel
     * that kills the caller (the fault S2 fixed for a count of lines taken as bytes). With the L2 sized by
     * function a 2 MB L2 is possible, and its range, the L2 plus two L1 data caches, is 0x210000 bytes. A range
     * that does not fit BAT0 starts at the system memory instead (the PPC's first 4 MB are the kernel,
     * the zero page and the message window), which the page table maps copy-back as the rest of the PPC
     * memory is; any contiguous range of that many distinct lines displaces the caches the same way. A range
     * of 2 MB or less starts at EA 0 as it always did, so every card that has an L2 of 1 MB or less, and the
     * e300 cards, are unchanged.
     */
    if ((lines * CACHELINE_SIZE) > 0x200000)
    {
        base = PowerPCBase->pp_PPCMemBase + OFFSET_SYSMEM;
    }

    mem = base;
    for (i = 0; i < lines; i++)
    {
        dFlush(mem);
        mem += CACHELINE_SIZE;
    }
    sync();

    mem = base;
    for (i = 0; i < lines; i++)
    {
        loadWord(mem);
        mem += CACHELINE_SIZE;
    }

    mem = base;
    for (i = 0; i < lines; i++)
    {
        dFlush(mem);
        mem += CACHELINE_SIZE;
    }
    sync();

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL ULONG KCachedLine(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") ULONG line)
{
    /*
     * A LINE THE PPC'S DATA CACHE CAN HOLD (C2, REVIEW-LIB.md). SetCache's
     * ranged operations run here, in the trap, on the caller's addresses,
     * and a fault in the trap is a nested exception (K1). The PPC caches
     * only its system memory, copyback through the page table, and BAT1's
     * gfx window, write-through (setupppc.c:308-312, :358-364). Any other
     * address is either unmapped -- Amiga memory, which the PPC reaches only
     * through the proxy and never caches: dcbf, dcbi or icbi there faulted,
     * killing the caller and leaving the exception depth at 1 -- or
     * cache-inhibited (BAT2, BAT3, the registers). Below 2 MB the trap sees
     * BAT0, the kernel's own memory (setupppc.c:315-318), which a caller's
     * chip RAM address only aliases: DCACHEINV there discarded the kernel's
     * unwritten lines. WarpOS (DCacheFlushArea, DCacheInvArea,
     * ICacheInvArea, DCacheLockArea: warp.p:195, :364, :612, :454) and
     * 17.13b (ppcfunctions.p:405, :174, :364) check no address, as Amiga
     * RAM is mapped there: THIS DIVERGES, rightly. Test: tests/cacherange.
     */
    ULONG dbatu = PowerPCBase->pp_SystemBATs[1].ba_dbatu;

    if (line < 0x200000)
    {
        return FALSE;                               //BAT0: the kernel's 2 MB
    }
    if ((line - (PowerPCBase->pp_PPCMemBase + OFFSET_SYSMEM)) < (PowerPCBase->pp_PPCMemSize - OFFSET_SYSMEM))
    {
        return TRUE;                                //system memory
    }
    if ((dbatu & (BAT_VALID_SUPERVISOR | BAT_VALID_USER)) &&
        ((line - (dbatu & 0xfffe0000)) < ((((dbatu >> 2) & 0x7ff) + 1) << 17)))
    {
        return TRUE;                                //BAT1: BEPI, and (BL + 1) blocks of 128 KB
    }
    return FALSE;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCKERNEL VOID L2InvWait(VOID)
{
    /*
     * C12 (REVIEW-LIB.md): WAITS FOR A GLOBAL L2 INVALIDATE TO FINISH, on the bit the part has. The MPC7400, the
     * MPC7410 and the 750FX report it in L2CR[L2IP], bit 31 (MPC7410UM 3.7.3.7, PDF p. 235), and the caller then
     * writes the next L2CR value, which clears L2I. The MPC745x's L2CR has no L2IP (a search of MPC7450UM.pdf for
     * L2IP, 12 Nov, found 0 hits); its L2I, bit 10, is cleared by the chip when the invalidate completes (PDF p.
     * 152), so the wait is on L2I itself, as setupCaches' 745x arm does it (setupppc.c, S6). The PVR upper halves
     * 0x8000 to 0x8004 are the 745x and 744x, the range GetInfo and S6 use. 17.13b's .L2ENABLE sets L2E without an
     * invalidate or a wait (ppcfunctions.p:138-151); WarpOS: NO COUNTERPART (a search of warpos/ for l2cr, l2wt,
     * mfl2, mtl2 and L2E, 12 Nov, returns 0 lines).
     */
    ULONG pvrHi = getPVR() >> 16;

    if ((pvrHi >= 0x8000) && (pvrHi <= 0x8004))
    {
        while (getL2State() & L2CR_L2I);
    }
    else
    {
        while (getL2State() & L2CR_L2IP);
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
                /*
                 * C10 (REVIEW-LIB.md): THE L2 GOES OFF BEFORE THE DATA CACHE DOES. The MPC7410 manual:
                 * "if the data cache is disabled, the L2 cache must also be disabled" (PDF p. 215, p.
                 * 233, p. 110), and with the data cache off a load or a store is caching-inhibited on the
                 * bus (p. 253) and can bypass an L2 that still holds the line, which DCACHEON would then
                 * serve stale. The L2 is flushed with the L1 below, as L2CACHEOFF does it, so it is
                 * switched off the same way; pp_L2OffByD records that DCACHEON has to switch it on again.
                 * An L2 that was off before stays off. The cost: instruction misses go to memory too
                 * (the L2 is unified) for as long as the data cache is off, which the manual asks for
                 * whatever this arm does. 17.13b (ppcfunctions.p:250-274) and WarpOS's DCacheOff_S
                 * (hal/warp.p:588-597) leave the L2 on: THIS DIVERGES from both, toward the manual.
                 *
                 * THE ORDER OF THE STORES, found by an independent review of the first build and seen on
                 * the G4 (tests/l2dcache D1c and D2b failed on 96ff6b0d): a store made while the data
                 * cache is still on is a modified line, and with DCE clear the L1 is not looked at (the
                 * tag status bits are ignored, MPC7410UM PDF p. 216), so memory keeps the old value and
                 * DCACHEON's flash invalidate throws the line away (p. 216: any modified data in the cache
                 * will be lost). So pp_L2OffByD is stored BEFORE the flush, which writes it to memory
                 * with everything else (the flush does not read it), and pp_CurrentL2Size AFTER the HID0
                 * write, when a store goes to memory itself, as pp_CacheDState always has. NOTHING STORES
                 * BETWEEN THE FLUSH'S RETURN AND THE HID0 WRITE (C3: with the L2 whatever DisL2Flush says).
                 * 17.13b stores its DState byte after the HID0 write too (ppcfunctions.p:250-274, the store
                 * at :269-270) and WarpOS's DCacheOff_S stores nothing there (hal/warp.p:588-597): THIS
                 * MATCHES both on the order.
                 */
                if ((PowerPCBase->pp_DeviceID != DEVICE_MPC8343E) && (PowerPCBase->pp_DeviceID != DEVICE_MPC8314E))
                {
                    if (getL2State() & L2CR_L2E)
                    {
                        PowerPCBase->pp_L2OffByD = 1;
                    }
                }

                FlushDCache(PowerPCBase, 1);

                if (PowerPCBase->pp_L2OffByD)
                {
                    value = getL2State();
                    value &= ~L2CR_L2E;
                    setL2State(value);
                }

                value = getHID0();
                value &= ~HID0_DCE;
                setHID0(value);
                PowerPCBase->pp_CacheDState = -1;

                if (PowerPCBase->pp_L2OffByD)
                {
                    PowerPCBase->pp_CurrentL2Size = 0;
                }
            }
            break;
        }
        case CACHE_DCACHEON:
        {
            /*
             * C4: A CACHE SWITCHED BACK ON IS FLASH-INVALIDATED. While DCE is
             * off the L1 is not looked at, but it keeps its lines, and
             * switched on again they serve data that memory has since
             * changed. Nothing dirty is lost: DCACHEOFF flushed everything.
             * The invalidate must come after the enable -- the e300 does it
             * only in an enabled cache, and wants DCFI set and cleared by two
             * consecutive mtspr (MPC8349EA RM Table 7-2 for the K1's
             * MPC8343E, MPC8315ERM Table 7-3, PDF pp. 351-352, for the
             * MPC8314E) -- and is not done when the cache is already on,
             * which would drop dirty lines. 17.13b (ppcfunctions.p:307-321)
             * and WarpOS (DCacheOn_S, warp.p:553-558) set the bit alone:
             * THIS DIVERGES, rightly.
             */
            value = getHID0();
            if (!(value & HID0_DCE))
            {
                value |= HID0_DCE;
                setHID0(value);
                flashInvD(value);
            }
            PowerPCBase->pp_CacheDState = 0;

            /*
             * C10: THE L2 THAT DCACHEOFF SWITCHED OFF COMES BACK, after the data cache (the L2 may be on
             * only while the data cache is: MPC7410UM PDF p. 233), invalidated first and only if it is
             * still off: L2I with L2E clear, a wait for the invalidate (L2InvWait: L2IP, or L2I on a
             * 745x, C12), then L2E, as L2CACHEON does. An explicit L2CACHEOFF since the switch-off
             * cleared the flag, and an L2 that a program switched on itself in the meantime is left as
             * it is.
             */
            if (PowerPCBase->pp_L2OffByD)
            {
                PowerPCBase->pp_L2OffByD = 0;
                value = getL2State();
                if (!(value & L2CR_L2E))
                {
                    setL2State(value | L2CR_L2I);
                    L2InvWait();
                    setL2State(value | L2CR_L2E);
                    PowerPCBase->pp_CurrentL2Size = PowerPCBase->pp_L2Size;
                }
            }
            break;
        }
        case CACHE_DCACHELOCK:
        {
            if ((start) && (length) && !(PowerPCBase->pp_CacheDLockState))
            {
                FlushDCache(PowerPCBase, 0);
                ULONG iterations = (ULONG)start + length;
                ULONG mask = -32;
                start = (APTR)((ULONG)start & mask);
                ULONG mem = (ULONG)start;
                iterations = (((iterations + 31) & mask) - (ULONG)start) >> 5;

                for (int i = 0; i < iterations; i++)
                {
                    if (KCachedLine(PowerPCBase, mem))      //C2
                    {
                        loadWord(mem);
                    }
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
            /*
             * C5: THE RANGE IS FLUSHED ON EVERY CARD. 17.15 gave a card with
             * no L2 (pp_L2Size 0: the K1 and both M1s) a branch that read 32 KB
             * from EA 0 and never touched the range -- slow on every call
             * (Run68K makes one, iFUSION up to 2048 a frame), and not a flush:
             * a line of the range could stay dirty. With no L2 the L1 is all
             * there is, and dcbf acts on it. 17.13b flushes the range on every
             * CPU (ppcfunctions.p:405-435) and so does WarpOS
             * (DCacheFlushArea, warp.p:195-223): THIS MATCHES. A flush with no
             * range is the whole-cache flush, FlushDCache, on every card.
             */
            if (!(PowerPCBase->pp_CacheDState) && !(PowerPCBase->pp_CacheDLockState))
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
                        if (KCachedLine(PowerPCBase, mem))      //C2
                        {
                            dFlush(mem);
                        }
                        mem += CACHELINE_SIZE;
                    }
                    sync();
                }
                else
                {
                    FlushDCache(PowerPCBase, 0);
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
            /* C4: switched on from off, the I-cache is flash-invalidated
             * after the enable, as DCACHEON. WarpOS (ICacheOn_S,
             * warp.p:749-754) and 17.13b (.ICACHEON, ppcfunctions.p:337-348)
             * set the bit alone: THIS DIVERGES; the invalidate only discards
             * clean lines. */
            value = getHID0();
            if (!(value & HID0_ICE))
            {
                value |= HID0_ICE;
                setHID0(value);
                flashInvI(value);
            }
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
            /* C5: dcbi on the range on every card, as DCACHEFLUSH above. On
             * 17.15's branch for a card with no L2 a dirty line was written
             * back instead of discarded. 17.13b: ppcfunctions.p:174-200;
             * WarpOS: DCacheInvArea, warp.p:375-400. THIS MATCHES. */
            if ((start) && (length))
            {
                ULONG iterations = (ULONG)start + length;
                ULONG mask = -32;
                start = (APTR)((ULONG)start & mask);
                ULONG mem = (ULONG)start;
                iterations = (((iterations + 31) & mask) - (ULONG)start) >> 5;

                for (int i = 0; i < iterations; i++)
                {
                    if (KCachedLine(PowerPCBase, mem))          //C2
                    {
                        dInval(mem);
                    }
                    mem += CACHELINE_SIZE;
                }
                sync();
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
                    if (KCachedLine(PowerPCBase, mem))              //C2
                    {
                        iInval(mem);
                    }
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
                /*
                 * C4: AN L2 SWITCHED BACK ON IS INVALIDATED FIRST. Off, the L2
                 * keeps its lines; on again, they serve data memory has since
                 * changed. As setupCaches does it (setupppc.c): L2I with L2E
                 * clear, wait for the invalidate (L2InvWait: L2IP, or L2I on a
                 * 745x, C12), then L2E -- only when it is off (L2I on a running
                 * L2 would drop dirty lines) and there is an L2.
                 * 17.13b (ppcfunctions.p:138-151) sets L2E alone: THIS
                 * DIVERGES, rightly.
                 *
                 * C12 (REVIEW-LIB.md): NOT WHILE THE DATA CACHE IS OFF. The L2 may be on only while the L1
                 * data cache is (MPC7410UM PDF p. 110, 233); DCACHEOFF has switched it off for that reason,
                 * and an L2CACHEON in that state put it back under a disabled data cache. With the data
                 * cache off the request is recorded (pp_L2OffByD = 1, stored with the cache off, so it goes
                 * to memory) and the L2 stays off; DCACHEON then switches it on, invalidated, as it does
                 * for an L2 it switched off itself. 17.13b's .L2ENABLE sets L2E whatever the data cache is
                 * doing (ppcfunctions.p:138-151): THIS DIVERGES, toward the manual. WarpOS: NO COUNTERPART
                 * (a search of warpos/ for l2cr, l2wt, mfl2, mtl2 and L2E, 12 Nov, returns 0 lines). A
                 * card without an L2 (pp_L2Size 0) keeps the old behavior.
                 */
                if ((PowerPCBase->pp_CacheDState) && (PowerPCBase->pp_L2Size))
                {
                    PowerPCBase->pp_L2OffByD = 1;
                    break;
                }
                value = getL2State();
                if ((PowerPCBase->pp_L2Size) && !(value & L2CR_L2E))
                {
                    setL2State(value | L2CR_L2I);
                    L2InvWait();
                }
                value |= L2CR_L2E;
                setL2State(value);
                PowerPCBase->pp_CurrentL2Size = PowerPCBase->pp_L2Size;
                break;
            }
            case CACHE_L2CACHEOFF:
            {
                FlushDCache(PowerPCBase, 1);               //C3: with the L2 whatever DisL2Flush says
                value = getL2State();
                value &= ~L2CR_L2E;
                setL2State(value);
                PowerPCBase->pp_CurrentL2Size = 0;
                PowerPCBase->pp_L2OffByD = 0;              //C10: an explicit switch-off stays off after DCACHEON
                break;
            }
            case CACHE_L2WTON:
            {
                value = getL2State();
                if ((value & L2CR_L2E) && !(value & L2CR_L2WT))
                {
                    /*
                     * C11 (REVIEW-LIB.md): L2WT IS SET WITH THE L2 OFF. The MPC7410 manual, 3.7.3.5 (PDF
                     * p. 234): "L2WT must never be set after the L2 cache has been enabled as previously
                     * modified lines may get re-marked as exclusive", and an exclusive line is clean to the
                     * L2 and is dropped, not written back, when it is replaced. So the order is the one
                     * L2CACHEOFF and L2CACHEON already use: the flush with the L2, L2E clear, L2WT set, the
                     * invalidate (L2I with L2E clear, a wait for it: L2InvWait), L2E. With the L2 off the bit is set
                     * alone, as before. 17.13b's .L2WTENABLE sets the bit with L2E not looked at
                     * (ppcfunctions.p:110-121): THIS DIVERGES, toward the manual. WarpOS: NO COUNTERPART.
                     */
                    FlushDCache(PowerPCBase, 1);
                    value &= ~L2CR_L2E;
                    setL2State(value);
                    value |= L2CR_L2WT;
                    setL2State(value);
                    setL2State(value | L2CR_L2I);
                    L2InvWait();
                    setL2State(value | L2CR_L2E);
                }
                else
                {
                    value |= L2CR_L2WT;
                    setL2State(value);
                }
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

    /*
     * C1: THE TASK IS READ ONCE, AND THERE MAY BE NONE. pp_ThisPPCProc is NULL
     * while the PPC idles (kernel.s records it at the entry, :309-313, and
     * takes the idle frame at the exit, :371-379), and a report can come with
     * nobody running: a machine check taken by the idle loop, or a fault in
     * kernel code that runs inside an exception with no task current (HaltError
     * is "mr r0,r3 / .long 0"). The report read the name, stored TS_REMOVED and
     * the CRASHED flag, and took the removal's address through the NULL
     * pointer, which is page zero here (BAT0 maps it for the kernel,
     * setupppc.c:326-329): the name came from 0x0A, a byte went over zp_Status
     * at 0x0F, 0x8000 was or'ed into 0x6C, and tp_TaskPtr at 0x68 decided the
     * removal. Until W3 that removal was unconditional, as in 17.15
     * (src_175/kernel.c:1580-1582): RemovePPC(NULL) read MemBase and ExecBase
     * from page zero's words 0 and 4 and stored the first into the second's
     * lib_Node.ln_Succ, through the proxy. Since W3 it is the word at 0x68,
     * which no code of the library sets: whatever the RAM held at power-up.
     * With no task the name and the task words are 0 -- the 68k prints "Kernel
     * Panic!" for a zero name (support68k.c:1314-1317) -- and nothing is stored
     * through the pointer.
     * 17.13b reads LN_NAME and stores TS_REMOVED through ThisPPCProc with no
     * test (setupppc.p:6517-6520, :6659-6662): THIS DIVERGES, no store through
     * NULL. It zeroes the name for an SRR0 below 0x10000000 (:6522-6528), the
     * same "Kernel Panic!", and takes a task off the list only in .CheckWait,
     * which .TrySwitch reaches with ThisPPCProc not 0 (:3678-3679, :3714-3737):
     * MATCHES for an idle report. WarpOS takes the head of PPC_TASKRUN in each
     * of its eight crash arms with no test for an empty list (powerpc.p:5880-5882,
     * :5982-5984 and six more), and a sleep state with an empty run list exists
     * (:4573-4590): NO COUNTERPART for the test.
     * NOT DONE: 17.13b then goes to .DoIdle (:4194-4236) and restarts idle. A
     * fault in kernel code with no task current is a second exception inside
     * the first, and the idle frame is one fixed address for both
     * (kernel.s:315-321), which is a different problem. Test: tests/crashtask.
     */
    struct TaskPPC* crashTask = PowerPCBase->pp_ThisPPCProc;

    ULONG* errorData = (ULONG*)(PowerPCBase->pp_PPCMemBase + (ULONG)(FIFO_END + 0x100));
    errorData[0]  = (crashTask) ? (ULONG)crashTask->tp_Task.tc_Node.ln_Name : 0;
    errorData[1]  = (ULONG)crashTask;
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

    if (crashTask)          //C1: with no task there is nothing to tear down
    {
        crashTask->tp_Task.tc_State = TS_REMOVED;
        crashTask->tp_Flags |= TASKPPCF_CRASHED;
        /*
         * OFF THE TASK LIST ONCE (W3, REVIEW-LIB.md). The crashed task leaves
         * pp_AllTasks here and is counted out here, and tp_TaskPtr is cleared:
         * the DeleteTaskPPC that Kryten (supportppc.c SystemStart) or its parent
         * runs on it later must not remove it again through its stale links --
         * that cut out whatever task had been added after it -- nor count it
         * out twice. A crashed 68k-started task, which Kryten frees without
         * DeleteTaskPPC, was never counted out at all. WarpOS keeps a crashed
         * task in the list, waiting for ever until its parent deletes it
         * (powerpc.p:7960-7973); the user chose 17.15's automatic cleanup, done
         * once: THIS DIVERGES. Test: tests/crashtask.
         */
        if (crashTask->tp_TaskPtr)
        {
            RemovePPC((struct Node*)crashTask->tp_TaskPtr);
            crashTask->tp_TaskPtr = NULL;
            PowerPCBase->pp_NumAllTasks -= 1;
        }
    }
    SwitchPPC(PowerPCBase, iframe);
}
