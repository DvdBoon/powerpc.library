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

#include <powerpc/powerpc.h>
#include <powerpc/portsPPC.h>
#include <powerpc/semaphoresPPC.h>
#include <powerpc/tasksPPC.h>
#include <exec/types.h>
#include <exec/exec.h>
#include <dos/dos.h>
#include <powerpc/portsPPC.h>
#include <powerpc/tasksPPC.h>
#include <powerpc/powerpc.h>

struct MsgFrame {
struct Message      mf_Message;
volatile ULONG      mf_Identifier;
struct MsgPort*     mf_MirrorPort;
struct TaskPPC*     mf_PPCTask;
struct PPCArgs      mf_PPCArgs;
ULONG               mf_Signals;
ULONG               mf_Arg[3];
};

struct CacheSize {
ULONG               cz_SizeBit;
ULONG               cz_SizeBytes;
};

struct DebugArgs {
STRPTR              db_ProcessName;
ULONG               db_Function;
ULONG               db_Arg[4];
};

struct SysCall {
ULONG               sc_Function;
ULONG               sc_Arg[4];
};

/* The dummy warp.library's base.
 *
 * Its PPC-side address-space functions are called with WarpBase in r3, not
 * PowerPCBase -- they are warp.library LVOs, so that is the convention -- and
 * they still need to reach the library. MakeLibrary is told 124 bytes of
 * positive size, so there is room past struct Library to carry it.
 *
 * wb_CallCount is bumped by the functions that are not implemented yet, so it
 * is possible to tell after a run whether anything ever called them. */
struct WarpBase {
struct Library      wb_Lib;
APTR                wb_PowerPCBase;     /* struct PrivatePPCBase*, declared
                                         * later in this header             */
ULONG               wb_CallCount[4];
};

struct MirrorTask {
struct MinNode      mt_Node;
struct TaskPPC*     mt_PPCTask;
struct Task*        mt_Task;
struct MsgPort*     mt_Port;
ULONG               mt_Flags;
};

struct PPCZeroPage {
ULONG               zp_PPCMemBase;
struct ExecBase*    zp_SysBase;
struct MemHeader*   zp_PPCMemHeader;
volatile ULONG      zp_Status;
struct PPCBase*     zp_PowerPCBase;         //Also used in kernel.s as 16(r0)!
ULONG               zp_PageTableSize;
ULONG               zp_CacheGap[2];
ULONG               zp_MemSize;
volatile ULONG      zp_DECCounter;
/*
 * APPENDED, so every offset above keeps its value -- constantsppc.i hardcodes
 * PPCMEMHEADER 8 and POWERPCBASE 0x10 against this struct.
 *
 * zp_ExcSRR0/zp_ExcSRR1 at 0x28/0x2C are where exception entry stages SRR0
 * and SRR1 until _StoreFrame can write them to the frame, and where _LoadFrame
 * puts them back for the exit to load. They used to be staged in SPRG0/SPRG1.
 *
 * zp_ExcScratch at 0x30 is what SPRG0 points at instead: the four words
 * iFUSION's guest trampoline writes at 0(r1)..12(r1), with four spare, mirroring
 * the 32 bytes warp.p:170 leaves above its exception stack. See FIXES.md item 9.
 */
volatile ULONG      zp_ExcSRR0;
volatile ULONG      zp_ExcSRR1;
volatile ULONG      zp_ExcScratch[8];
};

struct InternalConsts {
struct ExecBase*        ic_SysBase;
BPTR                    ic_SegList;
struct DosLibrary*      ic_DOSBase;
struct PciBase*         ic_PciBase;
struct PPCBase*         ic_PowerPCBase;
struct ExpansionBase*   ic_ExpansionBase;
ULONG                   ic_MemBase;
ULONG                   ic_gfxMem;
ULONG                   ic_gfxSize;
ULONG                   ic_gfxConfig;
ULONG                   ic_startBAT;
ULONG                   ic_sizeBAT;
ULONG                   ic_deviceID;
ULONG                   ic_env1;
ULONG                   ic_env2;
ULONG                   ic_env3;
UWORD                   ic_gfxType;
UWORD                   ic_gfxSubType;
UWORD                   ic_pciType;
};

struct InitData {
ULONG               id_Reserved;
volatile ULONG      id_Status;
ULONG               id_MemBase;
ULONG               id_MemSize;
ULONG               id_GfxMemBase;
ULONG               id_GfxMemSize;
UWORD               id_GfxType;
UWORD               id_GfxSubType;
ULONG               id_GfxConfigBase;
ULONG               id_Environment1;
ULONG               id_Environment2;
ULONG               id_Environment3;
ULONG               id_DeviceID;
ULONG               id_ConfigBase;
ULONG               id_MsgsBase;
ULONG               id_MPICBase;
ULONG               id_StartBat;
ULONG               id_SizeBat;
};

struct MemSettings {
ULONG               ms_msar1;
ULONG               ms_msar2;
ULONG               ms_mear1;
ULONG               ms_mear2;
ULONG               ms_mesar1;
ULONG               ms_mesar2;
ULONG               ms_meear1;
ULONG               ms_meear2;
ULONG               ms_mben;
ULONG               ms_mccr1;
ULONG               ms_mccr1testhigh;
ULONG               ms_mccr1testlow1;
ULONG               ms_mccr1testlow2;
ULONG               ms_mccr1testlow3;
ULONG               ms_memsize;
};

struct BATArray {
ULONG                       ba_ibatu;
ULONG                       ba_ibatl;
ULONG                       ba_dbatu;
ULONG                       ba_dbatl;
};

struct LibSema {
struct SignalSemaphorePPC   ls_Semaphore;
ULONG                       ls_Reserved[8];
};

struct PrivatePPCBase {
struct PPCBase              pp_PowerPCBase;
struct MinList              pp_MirrorList;
ULONG                       pp_DeviceID;
ULONG                       pp_BridgeConfig;
ULONG                       pp_BridgeMsgs;
ULONG                       pp_BridgeMPIC;
struct MsgPort*             pp_MasterControl;
struct UtilityBase*         pp_UtilityBase;
UBYTE                       pp_DebugLevel;
UBYTE                       pp_EnAlignExc;
UBYTE                       pp_EnDAccessExc;
UBYTE                       pp_Pad;
ULONG                       pp_TaskExitCode;
ULONG                       pp_PPCMemBase;
ULONG                       pp_PPCMemSize;
volatile ULONG              pp_Mutex;
ULONG                       pp_L2Size;
ULONG                       pp_CurrentL2Size;
ULONG                       pp_L2State;
ULONG                       pp_CPUSDR1;
ULONG                       pp_CPUHID0;
ULONG                       pp_CPUSpeed;
ULONG                       pp_CPUInfo;
ULONG                       pp_PageTableSize;
ULONG                       pp_LowerLimit;
ULONG                       pp_UpperLimit;
struct TaskPPC*             pp_ThisPPCProc;
struct TaskPPC*             pp_TaskExcept;
struct MinList              pp_WaitingTasks;
struct MinList              pp_ReadyTasks;
struct MinList              pp_NewTasks;
struct MinList              pp_RemovedTasks;
struct MinList              pp_AllTasks;
struct MinList              pp_Snoop;
struct MinList              pp_Semaphores;
struct MinList              pp_RemovedExc;
struct MinList              pp_ReadyExc;
struct MinList              pp_InstalledExc;
struct MinList              pp_ExcInterrupt;
struct MinList              pp_ExcIABR;
struct MinList              pp_ExcPerfMon;
struct MinList              pp_ExcTrace;
struct MinList              pp_ExcSystemCall;
struct MinList              pp_ExcDecrementer;
struct MinList              pp_ExcFPUn;
struct MinList              pp_ExcProgram;
struct MinList              pp_ExcAlign;
struct MinList              pp_ExcIAccess;
struct MinList              pp_ExcDAccess;
struct MinList              pp_ExcMCheck;
struct MinList              pp_ExcSysMan;
struct MinList              pp_ExcTherMan;
struct MinList              pp_WaitTime;
struct MinList              pp_Ports;
struct MinList              pp_MsgQueue;
struct LibSema              pp_SemWaitList;
struct LibSema              pp_SemTaskList;
struct LibSema              pp_SemSemList;
struct LibSema              pp_SemPortList;
struct LibSema              pp_SemSnoopList;
struct LibSema              pp_SemMemory;
ULONG                       pp_AlignmentExcHigh;
ULONG                       pp_AlignmentExcLow;
ULONG                       pp_DataExcHigh;
ULONG                       pp_DataExcLow;
struct MsgPortPPC*          pp_CurrentPort;
UBYTE                       pp_EnAltivec;
volatile UBYTE              pp_ExceptionMode;
UBYTE                       pp_CacheDisDFlushAll;
UBYTE                       pp_CacheDState;
UBYTE                       pp_CacheDLockState;
/*
 * pp_FlagReschedule: set by anything that changes the run queue materially,
 * consumed by CPUStats, which treats it as "recompute the load figures on this
 * tick rather than waiting for pp_BusyCounter to expire". FIXES.md item 17 --
 * it used to be written at four places and read at none.
 *
 * HOW IT MAPS ONTO THE ORIGINAL. There the trigger is a TIMEBASE DEADLINE
 * rather than a flag: powerpc.p:4440 compares PPC_RESCH_TBL against mftbl,
 * calls ReSchedule when due, and re-arms to now + RESCH_TICKS (2500000, about
 * 150 ms; powerpc.p:21-23). CPUStats is already a port of ReSchedule's
 * .calcload loop -- the same tp_Activity, tp_Busy and tp_CPUusage formulas and
 * the same systemLoad/cpuLoad accumulation -- and already periodic, gated by
 * pp_BusyCounter = 25, which at a 20 ms quantum is every 500 ms. So the
 * deadline exists here as a tick counter, and the flag is the "sooner than
 * that" signal, matching the original's shorter RESCH_DEFAULT of about 6 ms.
 *
 * THAT SECOND PASS IS NOW IMPLEMENTED -- this comment used to end "writes
 * none of them, giving every task the same fixed pp_StdQuantum", which was
 * true until 21 Oct 2026. tp_Desired comes from tp_Nice through the NICE table
 * (FIXES.md item 18) and tp_Priority, tp_Prioffset and tp_Quantum are all
 * written by CPUStats and SwitchPPC under the V14 priority rule (item 1). See
 * prirule.h for the rule and for how the two base fields below were found to
 * be named for the opposite of what they hold.
 */
UBYTE                       pp_FlagReschedule;      //read by CPUStats
UBYTE                       pp_FlagForbid;
UBYTE                       pp_FlagWait;
UBYTE                       pp_FlagPortInUse;  //for nt_IntPort....not used
UBYTE                       pp_BusyCounter;
UBYTE                       pp_BytePad[2];
ULONG                       pp_NumAllTasks;
ULONG                       pp_StartTBL;
ULONG                       pp_CurrentTBL;
ULONG                       pp_SystemLoad;
ULONG                       pp_CPULoad;
ULONG                       pp_NICETable;
/* PPC_REACTION: the tunable DISTANCE, PRI_MAXDIST by default, SCHED_REACTION
 * tag -- NOT a priority. The name has it backwards; see prirule.h. */
LONG                        pp_LowActivityPri;
ULONG                       pp_IdSysTasks;
ULONG                       pp_IdUsrTasks;
/* PPC_MINPRI: the RUNNING MINIMUM effective priority, starts at 0 and goes
 * negative -- NOT an offset. The ceiling is these two added together. */
LONG                        pp_LowActivityPriOffset;
ULONG                       pp_ErrorStrings;
ULONG                       pp_BusClock;
ULONG                       pp_Quantum;
ULONG                       pp_StdQuantum;
ULONG                       pp_NumRun68k;
struct BATArray             pp_ExceptionBATs[4];
struct BATArray             pp_StoredBATs[4];
struct BATArray             pp_SystemBATs[4];
ULONG                       pp_SystemSegs[16];
};

struct killFIFO {
ULONG                       kf_MIOFH;
ULONG                       kf_MIOPT;
ULONG                       kf_MIIFT;
ULONG                       kf_MIIPH;
ULONG                       kf_CreatePrevious;
ULONG                       kf_GetPrevious;
ULONG                       kf_CacheGap[2];
ULONG                       kf_MIOFT;
ULONG                       kf_MIOPH;
ULONG                       kf_MIIFH;
ULONG                       kf_MIIPT;
};

struct iframe {
struct EXCContext           if_Context;
ULONG                       if_AlignStore[2];
ULONG                       if_VSCR[4];
ULONG                       if_regAltivec[32*4];
ULONG                       if_VRSAVE;
struct BATArray             if_BATs[4];
ULONG                       if_Segments[16];
ULONG                       if_ExcNum;
};

struct poolHeader {
struct MinNode              ph_Node;
ULONG                       ph_Requirements;
ULONG                       ph_PuddleSize;
ULONG                       ph_ThresholdSize;
struct MinList              ph_PuddleList;
struct MinList              ph_BlockList;
};

struct SemWait {
struct MinNode              sw_Node;
struct TaskPPC*             sw_Task;
ULONG                       sw_Pad[2];
struct SignalSemaphorePPC*  sw_Semaphore;
};

struct TagItemPtr {
struct TagItem* tip_TagItem;
};

struct ExcData {
struct Node                 ed_Node;
APTR                        ed_Code;
ULONG                       ed_Data;
struct TaskPPC*             ed_Task;
volatile ULONG              ed_Flags;
ULONG                       ed_ExcID;
/*
 * ed_TimeBasePtr IS AN ADDRESS, NOT A TIME. EXCATTR_TIMEDREMOVAL's value is
 * a pointer to a two-word timebase, [high, low]: iFUSION passes
 * &DecDeadline[0] and &PublishedDeadline[0], filled by its read_timebase at
 * 0x01ED0C and 0x01ED18. The original declares it the same way --
 * warpos/warpos/powerpc_local.i:199 is "APTR X_TB" with "ULONG X_TBU" and
 * "ULONG X_TBL" beside it, the same three fields in the same order -- and
 * dereferences it at powerpc.p:5268, comparing [ptr+4] against X_TBL.
 *
 * Renamed from ed_RemovalTime, which said "time" of something holding an
 * address. NOTHING READS IT YET: see FIXES.md item 8.
 */
APTR                        ed_TimeBasePtr;
ULONG                       ed_TimeBaseUpper;
ULONG                       ed_TimeBaseLower;
struct ExcData*             ed_LastExc;
};

struct ExcInfo {
struct ExcData              ei_ExcData;
struct Node*                ei_MachineCheck;
struct Node*                ei_DataAccess;
struct Node*                ei_InstructionAccess;
struct Node*                ei_Alignment;
struct Node*                ei_Program;
struct Node*                ei_FPUnavailable;
struct Node*                ei_Decrementer;
struct Node*                ei_SystemCall;
struct Node*                ei_Trace;
struct Node*                ei_PerfMon;
struct Node*                ei_IABR;
struct Node*                ei_Interrupt;
};

struct DataMsg {
ULONG                       dm_Type;
ULONG                       dm_Value;
ULONG                       dm_Address;
ULONG                       dm_LoadType;
ULONG                       dm_RegNumber;
ULONG                       dm_LoadFlag;
ULONG                       dm_IndexedFlag;
};

struct WaitTime {
struct Node                 wt_Node;
ULONG                       wt_TimeUpper;
ULONG                       wt_TimeLower;
struct TaskPPC*             wt_Task;
};

struct UInt64 {
ULONG                       ui_High;
ULONG                       ui_Low;
};

struct SnoopData {
struct Node                 sd_Node;
APTR                        sd_Code;
ULONG                       sd_Data;
ULONG                       sd_Type;
};

struct RDFData {
APTR                        rd_DataStream;
UBYTE                       rd_Buffer[16];
STRPTR                      rd_BufPointer;
STRPTR                      rd_FormatString;
ULONG                       rd_TruncateNum;
APTR                        rd_PutChData;
};

struct PrivateTask {
struct TaskPPC              pt_Task;
UWORD                       pt_Pad1;
struct Message*             pt_StartMsg;
struct Task*                pt_Mirror68K;
struct MsgPort*             pt_MirrorPort;
};

struct NewTask {
struct PrivateTask          nt_Task;
struct iframe               nt_Context;
struct TaskPtr              nt_TaskPtr;
UWORD                       nt_Pad2;
struct MsgPortPPC           nt_Port;
ULONG                       nt_SSReserved1[8];  //Belongs to Semaphore of nt_Port
struct MsgPortPPC           nt_IntPort;         //Currently not used or set-up. Also removed from functions atm.
ULONG                       nt_SSReserved2[8];  //Belongs to Sempahore of nt_IntPort
struct BATArray             nt_BatStore[4];
UBYTE                       nt_Name[256];
};

