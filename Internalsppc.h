// Copyright (c) 2019-2020, 2024 Dennis van der Boon
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


#ifdef __PPC__
#define __r3 __reg("r3")
#define __r4 __reg("r4")
#define __r5 __reg("r5")
#define __r6 __reg("r6")
#define __r7 __reg("r7")
#define __r8 __reg("r8")
#define __r9 __reg("r9")
#define __r10 __reg("r10")
#else
#define __r3
#define __r4
#define __r5
#define __r6
#define __r7
#define __r8
#define __r9
#define __r10
#endif

#include <exec/types.h>

void illegal(void) = "\t.long\t0\n";                //debug function
VOID writeTest(ULONG, ULONG) ="\tstw\tr4,0(r3)\n"; // "
void counter(ULONG) = "\tlwzx\tr4,r0,r3\n\taddi\tr4,r4,1\n\tstwx\tr4,r0,r3\n";

#define PPCFUNCTION __section ("functions","acrx") __entry
#define PPCKERNEL __section ("kernel","acrx") __entry
#define PPCSETUP __section (".setupppc","acrx") __entry

#define _LVOAllocVec32      -54
#define _LVOFreeVec32       -60
#define _LVOSprintF68K      -66
#define _LVOAllocVec        -684
#define _LVOAllocMem        -198
#define _LVOStricmp         -162
#define _LVOInsertPPC       -402
#define _LVOFindNamePPC     -444
#define _LVOSystemStart     -870
#define _LVOStartTask       -876
#define _LVOEndTask         -882

#undef TASKPPCB_ATOMIC      //TasksPPC.h is wrong. See TasksPPC.i
#undef TASKPPCF_ATOMIC
#define TASKPPCB_EMULATOR   4
#define TASKPPCB_ATOMIC     5
#define TASKPPCB_CREATORPPC 14
#define TASKPPCB_CRASHED    15
#define TASKPPCF_EMULATOR   (1L<<4)
#define TASKPPCF_ATOMIC     (1L<<5)
#define TASKPPCF_CREATORPPC (1L<<14)
#define TASKPPCF_CRASHED    (1L<<15)

void        Reset  (void);
ULONG   GetExcTable(void);
ULONG   GetVecEntry(void);

ULONG SystemCall(APTR)                    = "\tsc\n";
ULONG getLeadZ(ULONG value)               = "\tcntlzw\tr3,r3\n";
ULONG getPVR(void)                        = "\tmfpvr\tr3\n";
ULONG getSDR1(void)                       = "\tmfsdr1\tr3\n";
void  setSDR1(ULONG value)                = "\tmtsdr1\tr3\n";
void  setIABR(ULONG value)                = "\tmtspr\t1010,r3\n";
void  setDABR(ULONG value)                = "\tmtspr\t1013,r3\n";
void  setSRR0(ULONG value)                = "\tmtsrr0\tr3\n";
void  setSRR1(ULONG value)                = "\tmtsrr1\tr3\n";
void  setSRIn(ULONG keyVal, ULONG segVal) = "\tmtsrin\tr3,r4\n";
ULONG getSRIn(ULONG address)              = "\tmfsrin\tr3,r3\n";
void  setMSR(ULONG value)                 = "\tsync\n\tmtmsr\tr3\n\tisync\n\tsync\n";
ULONG getMSR(void)                        = "\tmfmsr\tr3\n";
ULONG getHID0(void)                       = "\tmfspr\tr3,1008\n";
ULONG getHID1(void)                       = "\tmfspr\tr3,1009\n";
void  setHID0(ULONG)                      = "\tsync\n\tmtspr\t1008,r3\n\tsync\n\tisync\n";
void  setDEC(LONG value)                  = "\tmtdec\tr3\n";
ULONG getDEC(void)                        = "\tmfdec\tr3\n";
ULONG getTBU(void)                        = "\tmftbu\tr3\n";
ULONG getTBL(void)                        = "\tmftbl\tr3\n";
ULONG getL2CR(void)                       = "\tmfl2cr\tr3\n";
void  setL2CR(ULONG value)                = "\tmtl2cr\tr3\n\tsync\n";
void  tlbSync(void)                       = "\ttlbsync\n";
void  tlbIe(ULONG value)                  = "\ttlbie\tr3\n";
void  storeR0(ULONG value)                = "\tmr\tr0,r3\n";
ULONG getR0(void)                         = "\tmr\tr3,r0\n";
ULONG getR1(void)                         = "\tmr\tr3,r1\n";
ULONG getR2(void)                         = "\tmr\tr3,r2\n";
void  storeR2(ULONG value)                = "\tmr\tr2,r3\n";
void  HaltError(ULONG)                     = "\tmr\tr0,r3\n\t.long\t0\n";
void  dFlush(ULONG address)               = "\tdcbf\tr0,r3\n";
void  iInval(ULONG address)               = "\ticbi\tr0,r3\n";
void  dInval(ULONG address)               = "\tdcbi\tr0,r3\n";
ULONG getL2State(void)                    = "\tmfl2cr\tr3\n";
void  setL2State(ULONG value)             = "\tmtl2cr\tr3\n\tsync\n";
void  sync(void)                          = "\tsync\n";
void  isync(void)                         = "\tisync\n";
void  loadWord(ULONG)                     = "\tlwz\tr3,0(r3)\n"; //to bypass optimizations
ULONG getCPUState(ULONG res, ULONG key)   = "\tmfpvr\tr4\n";
ULONG roll(ULONG value, ULONG shift)      = "\trlwinm\tr3,r3,r4,0,31\n";
void  TaskHalt(void)                      = ".l1:\tb\t.l1\n";

ULONG loadPCI(ULONG base, ULONG offset)                    = "\tlwbrx\tr3,r3,r4\n";
ULONG loadPCI16(ULONG base, ULONG offset)                  = "\tlhbrx\tr3,r3,r4\n";
void  storePCI(ULONG base, ULONG offset, LONG value)       = "\tstwbrx\tr5,r3,r4\n";
void  storePCI16(ULONG base, ULONG offset, LONG value)     = "\tsthbrx\tr5,r3,r4\n";
void  storePCI8(ULONG base, ULONG offset, LONG value)      = "\tstbx\tr5,r3,r4\n";

ULONG readmemLongPPC(ULONG base, ULONG offset)                    = "\tlwzx\tr3,r3,r4\n";
void  writememLongPPC(ULONG base, ULONG offset, LONG value)       = "\tstwx\tr5,r3,r4\n";

void setBAT0(ULONG ibatl, ULONG ibatu, ULONG dbatl, ULONG dbatu)
        ="\tmtibatl\t0,r3\n\tmtibatu\t0,r4\n\tmtdbatl\t0,r5\n\tmtdbatu\t0,r6\n";
void setBAT1(ULONG ibatl, ULONG ibatu, ULONG dbatl, ULONG dbatu)
        ="\tmtibatl\t1,r3\n\tmtibatu\t1,r4\n\tmtdbatl\t1,r5\n\tmtdbatu\t1,r6\n";
void setBAT2(ULONG ibatl, ULONG ibatu, ULONG dbatl, ULONG dbatu)
        ="\tmtibatl\t2,r3\n\tmtibatu\t2,r4\n\tmtdbatl\t2,r5\n\tmtdbatu\t2,r6\n";
void setBAT3(ULONG ibatl, ULONG ibatu, ULONG dbatl, ULONG dbatu)
        ="\tmtibatl\t3,r3\n\tmtibatu\t3,r4\n\tmtdbatl\t3,r5\n\tmtdbatu\t3,r6\n";

void mSync(void)                          = "\tsync\n\tisync\n";

void mulInt64(struct UInt64* myint64, ULONG value1, ULONG value2)
        ="\tmullw\tr6,r4,r5\n\
          \tstw\tr6,4(r3)\n\
          \tmulhw\tr6,r4,r5\n\
          \tstw\tr6,0(r3)\n";

void mvfrBAT0(struct BATArray*)
        ="\tmfibatu\tr4,0\n\
          \tstw\tr4,0(r3)\n\
          \tmfibatl\tr4,0\n\
          \tstw\tr4,4(r3)\n\
          \tmfdbatu\tr4,0\n\
          \tstw\tr4,8(r3)\n\
          \tmfdbatl\tr4,0\n\
          \tstw\tr4,12(r3)\n";

void mvfrBAT1(struct BATArray*)
        ="\tmfibatu\tr4,1\n\
          \tstw\tr4,0(r3)\n\
          \tmfibatl\tr4,1\n\
          \tstw\tr4,4(r3)\n\
          \tmfdbatu\tr4,r1\n\
          \tstw\tr4,8(r3)\n\
          \tmfdbatl\tr4,1\n\
          \tstw\tr4,12(r3)\n";

void mvfrBAT2(struct BATArray*)
        ="\tmfibatu\tr4,2\n\
          \tstw\tr4,0(r3)\n\
          \tmfibatl\tr4,2\n\
          \tstw\tr4,4(r3)\n\
          \tmfdbatu\tr4,2\n\
          \tstw\tr4,8(r3)\n\
          \tmfdbatl\tr4,2\n\
          \tstw\tr4,12(r3)\n";

void mvfrBAT3(struct BATArray*)
        ="\tmfibatu\tr4,3\n\
          \tstw\tr4,0(r3)\n\
          \tmfibatl\tr4,3\n\
          \tstw\tr4,4(r3)\n\
          \tmfdbatu\tr4,3\n\
          \tstw\tr4,8(r3)\n\
          \tmfdbatl\tr4,3\n\
          \tstw\tr4,12(r3)\n";

void mvtoBAT0(struct BATArray*)
        ="\tlwz\tr4,0(r3)\n\
          \tmtibatu\t0,r4\n\
          \tlwz\tr4,4(r3)\n\
          \tmtibatu\t0,r4\n\
          \tlwz\tr4,8(r3)\n\
          \tmtibatu\t0,r4\n\
          \tlwz\tr4,12(r3)\n\
          \tmtibatu\t0,r4\n";

void mvtoBAT1(struct BATArray*)
        ="\tlwz\tr4,0(r3)\n\
          \tmtibatu\t1,r4\n\
          \tlwz\tr4,4(r3)\n\
          \tmtibatu\t1,r4\n\
          \tlwz\tr4,8(r3)\n\
          \tmtibatu\t1,r4\n\
          \tlwz\tr4,12(r3)\n\
          \tmtibatu\t1,r4\n";

void mvtoBAT2(struct BATArray*)
        ="\tlwz\tr4,0(r3)\n\
          \tmtibatu\t2,r4\n\
          \tlwz\tr4,4(r3)\n\
          \tmtibatu\t2,r4\n\
          \tlwz\tr4,8(r3)\n\
          \tmtibatu\t2,r4\n\
          \tlwz\tr4,12(r3)\n\
          \tmtibatu\t2,r4\n";

void mvtoBAT3(struct BATArray*)
        ="\tlwz\tr4,0(r3)\n\
          \tmtibatu\t3,r4\n\
          \tlwz\tr4,4(r3)\n\
          \tmtibatu\t3,r4\n\
          \tlwz\tr4,8(r3)\n\
          \tmtibatu\t3,r4\n\
          \tlwz\tr4,12(r3)\n\
          \tmtibatu\t3,r4\n";

LONG myRun68K(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct PPCArgs* PPStruct);
LONG myCRun68K(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct PPCArgs* PPStruct);
LONG myWaitFor68K(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct PPCArgs* PPStruct);
VOID mySPrintF(__r3 struct PrivatePPCBase* PowerPCBase, __r4 STRPTR Formatstring, __r5 APTR Values);
APTR myAllocVecPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG size, __r5 ULONG flags, __r6 ULONG align);
LONG myFreeVecPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR memblock);
struct TaskPPC* myCreateTaskPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TagItem* taglist);
VOID myDeleteTaskPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TaskPPC* PPCtask);
struct TaskPPC* myFindTaskPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 STRPTR name);
LONG myInitSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
VOID myFreeSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
LONG myAddSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
VOID myRemSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
VOID myObtainSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
LONG myAttemptSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
VOID myReleaseSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
struct SignalSemaphorePPC* myFindSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 STRPTR name);
VOID myInsertPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list, __r5 struct Node* node, __r6 struct Node* pred);
VOID myAddHeadPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list, __r5 struct Node* node);
VOID myAddTailPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list, __r5 struct Node* node);
VOID myRemovePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct Node* node);
struct Node* myRemHeadPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list);
struct Node* myRemTailPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list);
VOID myEnqueuePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list, __r5 struct Node* node);
struct Node* myFindNamePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list, __r5 STRPTR name);
struct TagItem* myFindTagItemPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG value, __r5 struct TagItem* taglist);
ULONG myGetTagDataPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG tagvalue, __r5 ULONG tagdefault, __r6 struct TagItem* taglist);
struct TagItem* myNextTagItemPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TagItem** tagitem);
LONG myAllocSignalPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 LONG signum);
VOID myFreeSignalPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 LONG signum);
ULONG mySetSignalPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG signals, __r5 ULONG mask);
VOID mySignalPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TaskPPC* task, __r5 ULONG signals);
ULONG myWaitPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG signals);
LONG mySetTaskPriPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TaskPPC* task, __r5 LONG pri);
VOID mySignal68K(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct Task* task, __r5 ULONG signals);
VOID mySetCache(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG flags, __r5 APTR start, __r6 ULONG length);
VOID KSetCache(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG flags, __r5 APTR start, __r6 ULONG length);
APTR mySetExcHandler(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TagItem* taglist);
VOID myRemExcHandler(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR xlock);
ULONG mySuper(__r3 struct PrivatePPCBase* PowerPCBase);
VOID myUser(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG key);
ULONG mySetHardware(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG flags, __r5 APTR param);
VOID myModifyFPExc(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG fpflags);
ULONG myWaitTime(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG signals, __r5 ULONG time);
struct TaskPtr* myLockTaskList(__r3 struct PrivatePPCBase* PowerPCBase);
VOID myUnLockTaskList(__r3 struct PrivatePPCBase* PowerPCBase);
VOID mySetExcMMU(__r3 struct PrivatePPCBase* PowerPCBase);
VOID myClearExcMMU(__r3 struct PrivatePPCBase* PowerPCBase);
VOID myChangeMMU(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG mode);
VOID myGetInfo(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TagItem* taglist);
struct MsgPortPPC* myCreateMsgPortPPC(__r3 struct PrivatePPCBase* PowerPCBase);
VOID myDeleteMsgPortPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPortPPC* port);
VOID myAddPortPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPortPPC* port);
VOID myRemPortPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPortPPC* port);
struct MsgPortPPC* myFindPortPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 STRPTR port);
struct Message* myWaitPortPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPortPPC* port);
VOID myPutMsgPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPortPPC* port, __r5 struct Message* message);
struct Message* myGetMsgPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPortPPC* port);
VOID myReplyMsgPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct Message* message);
VOID myFreeAllMem(__r3 struct PrivatePPCBase* PowerPCBase);
VOID myCopyMemPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR source, __r5 APTR dest, __r6 ULONG size);
VOID CopyMemQuickPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR source, __r5 APTR dest, __r6 ULONG size);
VOID CopyMemQuickVMX(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR source, __r5 APTR dest, __r6 ULONG size);
VOID CopyMemAltivec(__r3 APTR dest, __r4 APTR source, __r5 ULONG size);
struct Message* myAllocXMsgPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG length, __r5 struct MsgPortPPC* port);
VOID myFreeXMsgPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct Message* message);
VOID myPutXMsgPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPort* port, __r5 struct Message* message);
VOID myGetSysTimePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct timeval* timeval);
VOID myAddTimePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct timeval* dest, __r5 struct timeval* source);
VOID mySubTimePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct timeval* dest, __r5 struct timeval* source);
LONG myCmpTimePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct timeval* dest, __r5 struct timeval* source);
struct MsgPortPPC* mySetReplyPortPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct Message* message, __r5 struct MsgPortPPC* port);
ULONG mySnoopTask(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TagItem* taglist);
VOID myEndSnoopTask(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG id);
VOID myGetHALInfo(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TagItem* taglist);
VOID mySetScheduling(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TagItem* taglist);
struct TaskPPC* myFindTaskByID(__r3 struct PrivatePPCBase* PowerPCBase, __r4 LONG id);
LONG mySetNiceValue(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TaskPPC* task, __r5 LONG nice);
LONG myTrySemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC, __r5 ULONG timeout);
VOID myNewListPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list);

/* warp.library LVOs, called with WarpBase in r3 rather than PowerPCBase.
 * See the block at the end of functionsppc.c. */
ULONG myCalcTableSize(__r3 struct WarpBase* WarpBase, __r4 ULONG size);
VOID  myAddAddrSpace(__r3 struct WarpBase* WarpBase, __r4 APTR pageTable,
                     __r5 ULONG start, __r6 ULONG end, __r7 ULONG physAddr,
                     __r8 ULONG protMode, __r9 ULONG cacheMode,
                     __r10 ULONG tableSize);
VOID  myRemoveAddrSpace(__r3 struct WarpBase* WarpBase, __r4 APTR pageTable,
                        __r5 ULONG start, __r6 ULONG end, __r7 ULONG tableSize,
                        __r8 ULONG vSpace);
ULONG myReadRC(__r3 struct WarpBase* WarpBase, __r4 ULONG arg1, __r5 ULONG arg2,
               __r6 ULONG arg3);
ULONG mySetExceptPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG signals, __r5 ULONG mask, __r6 ULONG flag);
VOID myObtainSemaphoreSharedPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
LONG myAttemptSemaphoreSharedPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
VOID myProcurePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC, __r5 struct SemaphoreMessage* SemaphoreMessage);
VOID myVacatePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC, __r5 struct SemaphoreMessage* SemaphoreMessage);
VOID myCauseInterrupt(__r3 struct PrivatePPCBase* PowerPCBase);
APTR myCreatePoolPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG flags, __r5 ULONG puddle_size, __r6 ULONG trehs_size);
VOID myDeletePoolPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR poolheader);
APTR myAllocPooledPPC(__r3 struct PrivatePPCBase* PowerPCBase,__r4 APTR poolheader, __r5 ULONG size);
VOID myFreePooledPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR poolheader, __r5 APTR ptr, __r6 ULONG size);
APTR myRawDoFmtPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 STRPTR formatstring, __r5 APTR datastream, __r6 APTR (*putchproc)(), __r7 APTR putchdata);
LONG myPutPublicMsgPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 STRPTR portname, __r5 struct Message* message);
LONG myAddUniquePortPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgPortPPC* port);
LONG myAddUniqueSemaphorePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct SignalSemaphorePPC* SemaphorePPC);
BOOL myIsExceptionMode(__r3 struct PrivatePPCBase* PowerPCBase);
VOID myAllocPrivateMem(void);
VOID myFreePrivateMem(void);
VOID myResetPPC(void);
BOOL myChangeStack(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG NewStackSize);
ULONG myRun68KLowLevel(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG Code, __r5 LONG Offset, __r6 ULONG a0, __r7 ULONG a1, __r8 ULONG d0, __r9 ULONG d1);
VOID printDebug(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct DebugArgs* args);
APTR AllocVec68K(__r3 struct PrivatePPCBase* PowerPCBase, __r4 ULONG size, __r5 ULONG flags);
VOID FreeVec68K(__r3 struct PrivatePPCBase* PowerPCBase, __r4 APTR memBlock);
LONG LockMutexPPC(ULONG mutex);
VOID FreeMutexPPC(ULONG mutex);
ULONG CheckExcSignal(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TaskPPC* myTask, __r5 ULONG signal);
VOID GetBATs(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TaskPPC* task);
VOID StoreBATs(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct TaskPPC* task);
VOID MoveToBAT(__r3 ULONG BATnumber, __r4 struct BATArray* batArray);
VOID MoveFromBAT(__r3 ULONG BATnumber, __r4 struct BATArray* batArray);
struct MsgFrame* CreateMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase);
struct MsgFrame* GetMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase);
VOID SendMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* msgFrame);
VOID FreeMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* msgFrame);
struct MsgFrame* KCreateMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase);
struct MsgFrame* KGetMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase);
VOID KSendMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* msgFrame);
VOID KFreeMsgFramePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* msgFrame);
LONG StricmpPPC(__r3 STRPTR string1, __r4 STRPTR string2);
VOID CauseDECInterrupt(__r3 struct PrivatePPCBase* PowerPCBase);
VOID InsertOnPri(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct List* list, __r5 struct TaskPPC* myTask);
VOID HandleMsgs(__r3 struct PrivatePPCBase* PowerPCBase);
VOID SwitchPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct iframe* iframe);
void TaskCheck(__r3 struct PrivatePPCBase* PowerPCBase);
VOID SystemStart(__r3 struct PrivatePPCBase* PowerPCBase);
void DispatchPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* newtask);
void StartTask(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* myFrame);
void KillTask(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* myFrame);
void CommonExcHandler(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct iframe* iframe, __r5 struct List* excList);
void CommonExcError(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct iframe* iframe);
VOID CPUStats(__r3 struct PrivatePPCBase* PowerPCBase);
VOID FlushDCache(__r3 struct PrivatePPCBase* PowerPCBase);
VOID EndTask(VOID);
ULONG GetLen(__r3 STRPTR string);
STRPTR CopyStr(__r3 APTR source, __r4 APTR dest);
ULONG SmallExcHandler(__r3 struct ExcData* data, __r4 struct iframe* iframe);
ULONG DoAlign(struct iframe* iframe, ULONG SRR0);
ULONG DoDataStore(struct iframe* iframe, ULONG SRR0, struct DataMsg* data);
ULONG FinDataStore(ULONG value, struct iframe* iframe, ULONG SRR0, struct DataMsg* data);
VOID SwapStack(ULONG oldspupper, ULONG newspupper);
APTR AllocatePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MemHeader* memHeader, __r5 ULONG byteSize);
VOID DeallocatePPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MemHeader* memHeader, __r5 APTR memoryBlock, __r6 ULONG byteSize);
VOID PerformPad(__r3 struct RDFData* rdfData, __r4 ULONG flag, __r5 APTR (*putchproc)(), __r6 LONG prependNum);
VOID MakeHex(__r3 struct RDFData* rdfData, __r4 ULONG flag, __r5 LONG value);
VOID MakeDecimal(__r3 struct RDFData* rdfData, __r4 BOOL sign, __r5 LONG value);
LONG AdjustParam(__r3 struct RDFData* rdfData, __r4 ULONG flag);
LONG AdjustParamInt(__r3 struct RDFData* rdfData);
ULONG getNum(__r3 struct RDFData* rdfData);
VOID SetFPExc(VOID);
VOID FinalCalc(ULONG counter, ULONG tbu, ULONG tbl, ULONG value1, ULONG value2, struct WaitTime* myWait);
ULONG Calculator(ULONG value1, ULONG value2, ULONG value3);
VOID FreeAllExcMem(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct ExcInfo* excInfo);
VOID AddExcList(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct ExcInfo* excInfo, __r5 struct ExcData* newData, __r6 ULONG* currExc, __r7 ULONG flag);
VOID FPE_Enable(ULONG value);
VOID FPE_Disable(ULONG value);
VOID ForbidPPC(__r3 struct PrivatePPCBase* PowerPCBase);
VOID PermitPPC(__r3 struct PrivatePPCBase* PowerPCBase);
VOID RunCPP(__r3 struct iframe* frame, __r4 ULONG Code, __r5 APTR args);
VOID SetupRunPPC(__r3 struct PrivatePPCBase* PowerPCBase, __r4 struct MsgFrame* myFrame);
VOID FlushICache(VOID);
ULONG* GetDecTable(VOID);
ULONG* getTableFX(VOID);
ULONG* getTable100(VOID);
ULONG* getTable66(VOID);
VOID getL2Size(ULONG testmem, APTR cz);
VOID detectMem(struct MemSettings* ms);
#if 0
VOID writememLongPPC(ULONG Base, ULONG offset, ULONG value);
ULONG readmemLongPPC(ULONG Base, ULONG offset);
#endif


#define writePCI(address, value) \
        storePCI(CONFIG_ADDR, 0, CMD_BASE | address); \
        sync();                                 \
        storePCI(CONFIG_DAT, 0, value); \
        sync();

#define writePCI16(address, value) \
        storePCI(CONFIG_ADDR, 0, CMD_BASE | address); \
        sync();                                 \
        storePCI16(CONFIG_DAT, 0, value); \
        sync();

#define writePCI8(address, value) \
        storePCI(CONFIG_ADDR, 0, CMD_BASE | address); \
        sync();                                 \
        storePCI8(CONFIG_DAT | (address & 0x3), 0, value); \
        sync();

