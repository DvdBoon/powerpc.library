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
#include <exec/types.h>
#include <proto/powerpc.h>
#include <powerpc/semaphoresPPC.h>
#include "libstructs.h"
#include "constants.h"
#include "internalsppc.h"
#pragma pack(pop)

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCSETUP BOOL setupTBL(__reg("r3") ULONG startEffAddr, __reg("r4") ULONG endEffAddr, __reg("r5") ULONG physAddr, __reg("r6") ULONG WIMG, __reg("r7") ULONG ppKey)
{
    ULONG currSR, uPTE, lPTE, hash, mySDR, PTEG, tempPTEG, flag, checkPTEG;

    while (startEffAddr < endEffAddr)
    {
        flag   = 0;
        currSR = getSRIn(startEffAddr);
        uPTE   = ((((currSR << 7) & 0x7fffff80) | ((startEffAddr >> 22) & 0x3f)) | PTE_VALID);
        lPTE   = (((physAddr & 0xfffff000) | ((WIMG << 3) & 0x78)) | PTE_REFERENCED | PTE_CHANGED | ppKey);

        hash = (((startEffAddr >> 12) & 0xffff) ^ (currSR & 0x0007ffff));

        mySDR = getSDR1();

        while (1)
        {
            tempPTEG = ((((hash >> 10) & 0x1ff) & mySDR) | ((mySDR >> 16) & 0x1ff));
            PTEG = (((hash << 6) & 0xffc0) | ((tempPTEG << 16) & 0x01ff0000) | (mySDR & 0xfe000000));

            for (int n=8; n>0; n--)
            {
                checkPTEG = *((ULONG*)(PTEG));

                if (!(checkPTEG & PTE_VALID))
                {
                    *((ULONG*)(PTEG))     = uPTE;
                    *((ULONG*)(PTEG) + 1) = lPTE;

                    startEffAddr += MMU_PAGESIZE;
                    physAddr     += MMU_PAGESIZE;
                    flag = 1;
                    break;
                }
                
                PTEG += 8;
            }

            if (flag)
            {
                break;
            }

            if (uPTE & PTE_HASHID)
            {
                return FALSE;
            }

            hash ^= 0xffffffff;
            uPTE |= PTE_HASHID;
        }

    }

    return TRUE;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCSETUP void setupPT(void)
{
    ULONG ptLoc;
    struct PPCZeroPage *myZP = 0;

    ULONG memSize = myZP->zp_MemSize;
    ULONG ptSize  = myZP->zp_PageTableSize;

    if (ptSize < 0x10000)
    {
        ptSize = 0x10000;
    }

    ULONG myPVR = getPVR();

    /*
     * THE 831X (PCIe M1) PAGE TABLE GOES AT THE TOP OF RAM (memSize - ptSize),
     * as 17.13b does for the Killer (setupppc.p .SetupPT: card_mem_size -
     * pt_size) and as the Harrier/MPC107 branch below does. The 831X declares
     * 128 MB RAM + 128 MB gfx, so ptSize is 2 MB and the old
     * OFFSET_MESSAGES - ptSize collapsed to 0, wiping the setup code at
     * 0x100000 -- the card came up once the table moved to the top (6 Nov
     * 2026). libinit.c reserves this table from the 68k pool to match. The
     * 834X (K1, 64 MB) keeps OFFSET_MESSAGES - ptSize: its table stays low and
     * the shipping K1 path is unchanged.
     */
    if ((myPVR >> 16) == ID_MPC834X)
    {
        ptLoc      = OFFSET_MESSAGES - ptSize;
    }
    else
    {
        ptLoc      = memSize - ptSize;
    }

    LONG  HTABMASK = 0xffff;
    ULONG mask     = (HTABMASK <<16);
    ULONG HTABORG  = (ptLoc & mask);
    ULONG testVal  = (HTABORG & mask);

    while ((testVal) && (HTABMASK))
    {
        HTABMASK = HTABMASK >> 1;
        mask = HTABMASK << 16;
        testVal = (HTABORG & mask);
    }

    HTABORG |= HTABMASK;
    setSDR1(HTABORG);

    ULONG numClear = ptSize >> 2;

    for (int i = 0; i < numClear; i++)
    {
        *((ULONG*)(ptLoc)) = 0;
        ptLoc += 4;
    }

    ULONG keyVal = 0x20000000;
    ULONG segVal = 0;

    for (int i = 0; i < 16; i++)
    {
        setSRIn(keyVal, segVal);
        segVal += 0x10000000;
        keyVal += 1;
    }
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCSETUP void mmuSetup(__reg("r3") struct InitData* initData)
{
    ULONG leadZeros, ibatl, ibatu, dbatl, dbatu, batSize;
    ULONG startEffAddr, endEffAddr, physAddr, WIMG, ppKey;
    struct PPCZeroPage *myZP = 0;
    ULONG PTSizeShift        = myZP->zp_MemSize + initData->id_GfxMemSize;
    ULONG shiftVal           = 23;

    leadZeros = getLeadZ(PTSizeShift);

    PTSizeShift = shiftVal - leadZeros;

    if ((myZP->zp_MemSize) & (1 << (30 - leadZeros))) //|| (initData->id_GfxSubType == DEVICE_VOODOO45))
    {
        PTSizeShift++;
    }

    myZP->zp_PageTableSize = (1 << PTSizeShift);

    setupPT();

    switch (initData->id_DeviceID)
    {
        case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
        {
            startEffAddr = IMMR_ADDR_DEFAULT;
            break;
        }
        case DEVICE_HARRIER:
        {
            startEffAddr = initData->id_MPICBase;
            endEffAddr   = startEffAddr + 0x40000;
            physAddr     = startEffAddr;
            WIMG         = PTE_CACHE_INHIBITED | PTE_GUARDED;
            ppKey        = PP_SUPERVISOR_RW;

            if (!(setupTBL(startEffAddr, endEffAddr, physAddr, WIMG, ppKey)))
            {
                initData->id_Status = ERR_PPCMMU;
                return;
            }

            startEffAddr = PPC_XCSR_BASE;
            break;
        }
        case DEVICE_MPC107:
        {
            startEffAddr = PPC_EUMB_BASE;
            break;
        }
    }

    endEffAddr   = startEffAddr + 0x100000;
    physAddr     = startEffAddr;
    WIMG         = PTE_CACHE_INHIBITED | PTE_GUARDED;
    ppKey        = PP_USER_RW;

    if (!(setupTBL(startEffAddr, endEffAddr, physAddr, WIMG, ppKey)))
    {
        initData->id_Status = ERR_PPCMMU;
        return;
    }

    startEffAddr = VECTOR_TABLE_DEFAULT;
    endEffAddr   = startEffAddr + 0x20000;
    physAddr     = startEffAddr;
    WIMG         = PTE_CACHE_INHIBITED;
    ppKey        = PP_USER_RW;

    if (!(setupTBL(startEffAddr, endEffAddr, physAddr, WIMG, ppKey)))
    {
        initData->id_Status = ERR_PPCMMU;
        return;
    }

    switch (initData->id_GfxType)
    {
        case VENDOR_ATI:
        {
            startEffAddr = initData->id_GfxConfigBase;
            endEffAddr   = startEffAddr + 0x10000;
            physAddr     = startEffAddr + OFFSET_PCIMEM;
            WIMG         = PTE_CACHE_INHIBITED | PTE_GUARDED;
            ppKey        = PP_USER_RW;

            if (!(setupTBL(startEffAddr, endEffAddr, physAddr, WIMG, ppKey)))
            {
                initData->id_Status = ERR_PPCMMU;
                return;
            }

            if (initData->id_GfxMemSize == 0x08000000)
            {
                batSize = BAT_BL_128M;
            }
            else
            {
                batSize = BAT_BL_256M;
            }
            break;
        }

        case VENDOR_3DFX:
        {
            startEffAddr = initData->id_GfxConfigBase;
            endEffAddr   = startEffAddr + initData->id_GfxMemSize;
            physAddr     = startEffAddr + OFFSET_PCIMEM;
            WIMG         = PTE_CACHE_INHIBITED | PTE_GUARDED;
            ppKey        = PP_USER_RW;

            if (!(setupTBL(startEffAddr, endEffAddr, physAddr, WIMG, ppKey)))
            {
                initData->id_Status = ERR_PPCMMU;
                return;
            }

            if (initData->id_GfxSubType == DEVICE_VOODOO45)
            {
                batSize = BAT_BL_128M;
            }
            else
            {
                batSize = BAT_BL_32M;
            }
            break;
        }
        case VENDOR_TI:
        {
            startEffAddr = initData->id_GfxConfigBase;
            endEffAddr   = startEffAddr + 0x20000;
            physAddr     = startEffAddr + OFFSET_PCIMEM;
            WIMG         = PTE_CACHE_INHIBITED | PTE_GUARDED;
            ppKey        = PP_USER_RW;

            if (!(setupTBL(startEffAddr, endEffAddr, physAddr, WIMG, ppKey)))
            {
                initData->id_Status = ERR_PPCMMU;
                return;
            }
            batSize = BAT_BL_16M;
            break;
        }
    }

    ibatl = ((initData->id_GfxMemBase + OFFSET_PCIMEM) | BAT_READ_WRITE);
    ibatu = initData->id_GfxMemBase | batSize | BAT_VALID_SUPERVISOR | BAT_VALID_USER;
    dbatl = ibatl | BAT_WRITE_THROUGH;

    setBAT1(ibatl, ibatu, dbatl, ibatu);
    mSync();

    ibatl = BAT_READ_WRITE;
    ibatu = BAT_BL_2M | BAT_VALID_SUPERVISOR;

    setBAT0(ibatl, ibatu, ibatl, ibatu);
    mSync();

    ibatl = OFFSET_MESSAGES | BAT_READ_WRITE;
    ibatu = (initData->id_MemBase + OFFSET_MESSAGES) | BAT_BL_2M | BAT_VALID_SUPERVISOR | BAT_VALID_USER;
    dbatl = ibatl | BAT_CACHE_INHIBITED;

    setBAT2(ibatl, ibatu, dbatl, ibatu);

    /*
     * BAT3: THE SECOND CARD'S MEMORY AS EXTRA MEMORY, where the marker
     * "adding using second ATI card as memory" stood (libinit.c:496-500: a
     * Radeon's memory, offered by the Mediator as "pcidma memory", with a
     * Voodoo as the primary card). As 17.13b's mmuSetup (setupppc.p:1577-1610):
     * EA id_StartBat, PA id_StartBat + OFFSET_PCIMEM, cache-inhibited, for
     * user and supervisor; none when the primary card is an ATI. id_SizeBat
     * is the byte size the detection found (17.13b passed an index).
     * FIXES.md "#3: RESTORE 17.13b's MAPPING", 1 Nov 2026.
     */
    if ((initData->id_SizeBat) && (initData->id_GfxType != VENDOR_ATI))
    {
        if (initData->id_SizeBat == 0x4000000)
        {
            batSize = BAT_BL_64M;
        }
        else if (initData->id_SizeBat == 0x8000000)
        {
            batSize = BAT_BL_128M;
        }
        else
        {
            batSize = BAT_BL_256M;
        }
        ibatl = (initData->id_StartBat + OFFSET_PCIMEM) | BAT_CACHE_INHIBITED | BAT_READ_WRITE;
        ibatu = initData->id_StartBat | batSize | BAT_VALID_SUPERVISOR | BAT_VALID_USER;

        setBAT3(ibatl, ibatu, ibatl, ibatu);
        mSync();
    }

    physAddr     = OFFSET_SYSMEM;
    startEffAddr = physAddr + (initData->id_MemBase);
    endEffAddr   = (initData->id_MemBase) + (initData->id_MemSize);
    WIMG         = PTE_COPYBACK;
    ppKey        = PP_USER_RW;

    if (!(setupTBL(startEffAddr, endEffAddr, physAddr, WIMG, ppKey)))
    {
        initData->id_Status = ERR_PPCMMU;
        return;
    }

    physAddr    = 0;

    for (int n=64; n>0; n--)
    {
        tlbIe(physAddr);
        physAddr += 0x1000;
    }

    tlbSync();

    setMSR(PSL_IR | PSL_DR | PSL_FP);

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCSETUP void installExceptions(void)
{
    UWORD excVector;
    ULONG ExcDataTable;
    ULONG ExcCode;

    ExcDataTable = GetExcTable();
    ExcCode      = GetVecEntry();

    for (int i=0; i<0x20; i++)
    {
        excVector = *((UWORD*)(ExcDataTable+(i*2)));

        if (excVector == 0xffff)
        {
            break;
        }
        switch (excVector)
        {
            case VEC_ITLBMISS:
            case VEC_DLOADTLBMISS:
            case VEC_DSTORETLBMISS :
            {
                *((UWORD*)(excVector)) = 0x4800;
                *((UWORD*)(excVector+2)) = ((excVector>>6)& 0xf) + OFFSET_KERNEL - excVector;
                break;
            }
            default:
            {
                int j;
                for (j=0; j<PPC_VECLEN; j++)
                {
                    *((ULONG*)(excVector+(j*4))) = *((ULONG*)(ExcCode+(j*4)));
                }
                *((UWORD*)(excVector-2+(j*4))) = 17 + OFFSET_KERNEL - excVector - (PPC_VECLEN<<2);
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

PPCSETUP void setupFIFOs(__reg("r3") struct InitData* initData)
{
    ULONG memIF, memIP, memOF, memOP;
    ULONG memBase  = initData->id_MemBase;
    ULONG memFIFO  = memBase + BASE_KMSG;
    ULONG memFIFO2 = memFIFO + SIZE_KBASE;

    switch (initData->id_DeviceID)
    {
        case DEVICE_HARRIER:
        {
            memIF    = XCSR_MIQB_DEFAULT;
            memIP    = memIF + SIZE_HFIFO;
            memOF    = memIP + SIZE_HFIFO;
            memOP    = memOF + SIZE_HFIFO;

            writememLongPPC(PPC_XCSR_BASE, XCSR_MIQB, XCSR_MIQB_DEFAULT);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIIFT, 4);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIIFH, 0);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIIPT, SIZE_HFIFO);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIIPH, SIZE_HFIFO);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIOFT, (SIZE_HFIFO * 2) + 4);
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIOFH, (SIZE_HFIFO * 2));
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIOPT, (SIZE_HFIFO * 3));
            writememLongPPC(PPC_XCSR_BASE, XCSR_MIOPH, (SIZE_HFIFO * 3));
            break;
        }
        case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
        {
            memIF    = FIFO_OFFSET;
            memIP    = memIF + SIZE_KFIFO;
            memOF    = memIP + SIZE_KFIFO;
            memOP    = memOF + SIZE_KFIFO;

            struct killFIFO* baseFIFO = (struct killFIFO*)(memIF + (SIZE_KFIFO *4));

            baseFIFO->kf_MIIFT = memBase + memIF + 4;
            baseFIFO->kf_MIIFH = memBase + memIF;
            baseFIFO->kf_MIIPT = memBase + memIP;
            baseFIFO->kf_MIIPH = memBase + memIP;
            baseFIFO->kf_MIOFH = memBase + memOF;
            baseFIFO->kf_MIOFT = memBase + memOF + 4;
            baseFIFO->kf_MIOPT = memBase + memOP;
            baseFIFO->kf_MIOPH = memBase + memOP;
            baseFIFO->kf_Doorbell = 0;              //the PCIe M1's doorbell count and the count acted on (E9)
            baseFIFO->kf_DoorbellSeen = 0;

            break;
        }
        case DEVICE_MPC107:
        {
            memIF    = MPC107_QBAR_DEFAULT;
            memIP    = memIF + SIZE_SFIFO;
            memOP    = memIP + SIZE_SFIFO;
            memOF    = memOP + SIZE_SFIFO;

            storePCI(PPC_EUMB_BASE, MPC107_QBAR, MPC107_QBAR_DEFAULT);
            storePCI(PPC_EUMB_BASE, MPC107_IFTPR, 4);
            storePCI(PPC_EUMB_BASE, MPC107_IFHPR, 0);
            storePCI(PPC_EUMB_BASE, MPC107_IPTPR, SIZE_SFIFO);
            storePCI(PPC_EUMB_BASE, MPC107_IPHPR, SIZE_SFIFO);
            storePCI(PPC_EUMB_BASE, MPC107_OPTPR, SIZE_SFIFO * 2);
            storePCI(PPC_EUMB_BASE, MPC107_OPHPR, SIZE_SFIFO * 2);
            storePCI(PPC_EUMB_BASE, MPC107_OFTPR, (SIZE_SFIFO * 3) + 4);
            storePCI(PPC_EUMB_BASE, MPC107_OFHPR, SIZE_SFIFO * 3);

            break;
        }
    }

    for (int i=0; i<4096; i++)
    {
        *((ULONG*)(memIP)) = 0;
        *((ULONG*)(memOP)) = 0;
        *((ULONG*)(memIF)) = memFIFO;
        *((ULONG*)(memOF)) = memFIFO2;

        memIP    += 4;
        memOP    += 4;
        memIF    += 4;
        memOF    += 4;
        memFIFO  += MSGLEN;
        memFIFO2 += MSGLEN;
    }

    switch (initData->id_DeviceID)
    {
        case DEVICE_HARRIER:
        {
            writememLongPPC(PPC_XCSR_BASE, XCSR_MICT, (XCSR_MICT_ENA | XCSR_MICT_QSZ_16K));
            break;
        }
        case DEVICE_MPC107:
        {
            storePCI(PPC_EUMB_BASE, MPC107_MUCR, (MPC107_MUCR_CQS_FIFO4K | MPC107_MUCR_CQE_ENABLE));
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

void setupOpenPIC(__reg("r3") struct InitData* initData)
{
    ULONG volatile value = readmemLongPPC(initData->id_MPICBase, XMPI_GLBC);

    writememLongPPC(initData->id_MPICBase, XMPI_GLBC, value | XMPI_GLBC_RESET);

    while (1)
    {
        value = readmemLongPPC(initData->id_MPICBase, XMPI_GLBC);  //different in asm version
        if (!(value & XMPI_GLBC_RESET))
        {
            writememLongPPC(initData->id_MPICBase, XMPI_GLBC, value | XMPI_GLBC_M);
            break;
        }
    }

    writememLongPPC(initData->id_MPICBase, XMPI_IFEVP, 0x00050042);
    writememLongPPC(initData->id_MPICBase, XMPI_IFEDE, XMPI_IFEDE_P0);
    writememLongPPC(initData->id_MPICBase, XMPI_P0CTP, 0);

    /*
     * S5: THE COUNT IS NIRQ, NOT THE WHOLE REGISTER. FREP is NIRQ (bits 27:16, the
     * highest external IRQ source supported), NCPU and VID (Harrier manual
     * p. 3-114: reset $00F, $01 and $03, so 0x000F0103). Counting down the word
     * itself made 983,299 uncached acknowledge and end-of-interrupt pairs here,
     * with the MMU and the caches still off (setupFIFOs and this run before
     * mmuSetup and setupCaches), instead of 15. 17.13b takes the field and
     * loops on it ("rlwinm r28,r27,16,20,31", setupppc.p:1051-1061); setupEPIC
     * below does the same for the MPC107's. WarpOS has no counterpart: it has no
     * OpenPIC code. tests/frep reads FREP back.
     */
    value = (readmemLongPPC(initData->id_MPICBase, XMPI_FREP) >> 16) & 0xfff;

    while (value)
    {
        readmemLongPPC(initData->id_MPICBase, XMPI_P0IAC);
        writememLongPPC(initData->id_MPICBase, XMPI_P0EOI, 0);
        value--;
    }

    writememLongPPC(PPC_XCSR_BASE, XCSR_FEEN, XCSR_FEEN_MIP | XCSR_FEEN_MIM0);
    writememLongPPC(PPC_XCSR_BASE, XCSR_FEMA, XCSR_FEMA_MIPM0);
    writememLongPPC(PPC_XCSR_BASE, XCSR_MCSR, XCSR_MCSR_OPI);

    return;
}
/********************************************************************************************
*
*
*
*********************************************************************************************/

void setupEPIC(void)
{
    writememLongPPC(PPC_EUMB_BASE, EPIC_GCR, EPIC_GCR_RESET);
    while ((readmemLongPPC(PPC_EUMB_BASE, EPIC_GCR) & EPIC_GCR_RSTATUS));
    writememLongPPC(PPC_EUMB_BASE, EPIC_GCR, EPIC_GCR_MIXMODE);

    storePCI(PPC_EUMB_BASE, EPIC_IIVPR3, 0x80050042);

    ULONG value = readmemLongPPC(PPC_EUMB_BASE, EPIC_EICR);
    writememLongPPC(PPC_EUMB_BASE, EPIC_EICR, (value & ~(1<<11)));   //SIE = 0

    value = readmemLongPPC(PPC_EUMB_BASE, EPIC_IIVPR3);

    writememLongPPC(PPC_EUMB_BASE, EPIC_IIVPR3, (value & ~(1<<7))); //Mask M bit

    writememLongPPC(PPC_EUMB_BASE, EPIC_PCTPR, 0);

    value = (loadPCI(PPC_EUMB_BASE, EPIC_FRR) >> 16 ) & 0x7ff;

    while (value)
    {
        ULONG value2 = readmemLongPPC(PPC_EUMB_EPICPROC, EPIC_IACK);
        sync();
        writememLongPPC(PPC_EUMB_EPICPROC, EPIC_EOI, 0);
        value--;
    }

    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCSETUP void initSema(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct SignalSemaphorePPC* SemaphorePPC)
{
    NewListPPC((struct List*)&SemaphorePPC->ssppc_SS.ss_WaitQueue);
    struct LibSema* libSema = (struct LibSema*)SemaphorePPC;
	SemaphorePPC->ssppc_SS.ss_Owner = 0;
	SemaphorePPC->ssppc_SS.ss_NestCount = 0;
	SemaphorePPC->ssppc_SS.ss_QueueCount = -1;
    SemaphorePPC->ssppc_reserved = (APTR)&libSema->ls_Reserved;
                                             
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

PPCSETUP void setupCaches(__reg("r3") struct PrivatePPCBase* PowerPCBase, __reg("r4") struct InitData* initData)
{
    ULONG value0 = getHID0();
    ULONG value1 = getHID1();
    ULONG num = 0;
    ULONG pll, l2Size, l2Setting;
    struct CacheSize cz;

    /*
     * L7, REVIEW-LIB.md: THE L2 CLOCK DIVIDER IS CHOSEN BEFORE THE L2 IS PROGRAMMED.
     * ENV:sonnet/SetCMemDiv: 1, 2, 3, 4 give /1, /1.5, /2, /2.5 and every other value
     * /3 (17.13b's parse, setupppc.p:1308-1329; its default also clocked a 400 MHz CPU
     * with 1 MB of L2 at /2, :1331-1338, which is left out, so an unset variable
     * changes nothing). The divider goes into the first write of L2CR below, with L2E
     * clear, so the DLL locks while the invalidate runs and the L2 is enabled after
     * both: the manual's sequence (MPC7410UM 3.7.5, PDF p. 240: clear L2E, set L2CLK,
     * wait for the lock or run the global invalidate, which takes much longer, then set
     * L2E). This function used to program /3 and, after the CPU speed, rewrite L2CR
     * with the chosen divider while L2E was set, with no flush, invalidate or wait.
     * 17.13b does that (setupppc.p:1340-1355), but the manual wants the DLL to
     * stabilize before the L2 interface is enabled (PDF p. 237), and the L2 ran while
     * it re-locked. THIS DIVERGES from 17.13b in the order, in the safe direction; the
     * size probe now runs at the chosen divider. Only the arms that program an external
     * L2 themselves use it: 17.13b also wrote L2CR on a 750FX or 745x, whose L2 is on
     * the chip. An if/else chain, not a switch: this function lives in .setupppc
     * (PPCSETUP) and a jump table would not.
     */
    UBYTE memDiv = (UBYTE)initData->id_Environment2;
    ULONG l2Clock = L2CR_L2CLK_3;

    if (memDiv == 1)
    {
        l2Clock = L2CR_L2CLK_1;
    }
    else if (memDiv == 2)
    {
        l2Clock = L2CR_L2CLK_1_5;
    }
    else if (memDiv == 3)
    {
        l2Clock = L2CR_L2CLK_2;
    }
    else if (memDiv == 4)
    {
        l2Clock = L2CR_L2CLK_2_5;
    }

    /*
     * S8, S6 (REVIEW-LIB.md): WHAT THE L2 ARMS NEED. pvrHi is the PVR's upper half: 0x000C is the MPC7400,
     * 0x800C the MPC7410 (both have the 2 MB setting, MPC7410UM PDF p. 110), 0x8000 to 0x8004 the MPC745x.
     * l2Scratch is the block that getL2Size fills to find the size of an external L2: L2_SCRATCH_GAP below
     * the page table, which setupPPC has put at the top of the PPC memory (the table is at least 64 KB, as
     * in mmuSetup), clear of what libinit.c allocated just below the table. The probe this replaces used the
     * 1 MB that starts 4 MB below the top of the memory.
     * S9: l2Scratch is a PHYSICAL address. getL2Size runs with address translation off, so that the table walks
     * of the search cannot meet an L2 whose size is wrong (REVIEW-LIB.md S9; the manual's own L2 SRAM test starts
     * with MSR[DR] = 0, MPC7410UM 3.7.9.2 step 1, PDF p. 245). System memory is mapped at MemBase + PA
     * (mmuSetup: EA = id_MemBase + PA from PA OFFSET_SYSMEM to the top, and pp_PPCMemSize is that top), so the
     * physical address of "the top, less the table, less L2_SCRATCH_GAP" has no MemBase in it.
     */
    ULONG pvrHi = getPVR() >> 16;
    ULONG allow2M = ((pvrHi == 0x000C) || (pvrHi == 0x800C)) ? 1 : 0;
    ULONG l2PtSize = PowerPCBase->pp_PageTableSize;
    ULONG l2Scratch;

    if (l2PtSize < 0x10000)
    {
        l2PtSize = 0x10000;
    }
    l2Scratch = PowerPCBase->pp_PPCMemSize - l2PtSize - L2_SCRATCH_GAP;

    switch (PowerPCBase->pp_DeviceID)
    {
        case DEVICE_HARRIER:
        {
            value0 |= HID0_ICE | HID0_DCE | HID0_SGE | HID0_BTIC | HID0_BHTE;
            setHID0(value0);

            if (getPVR() >> 24 == 0x70)
            {
                l2Size = L2_SIZE_HM;

                if (!(getL2CR() & L2CR_L2E))
                {
                    setL2CR(L2CR_L2I);
                    while (getL2CR() & L2CR_L2IP);
                    setL2CR(L2CR_L2E);
                }

                pll = value1 >> 27;

                ULONG* pllTable = getTableFX();

                while (pllTable[num])
                {
                    if (pllTable[num] == pll)
                    {
                        PowerPCBase->pp_CPUSpeed = pllTable[num+1];
                        break;
                    }
                    num +=2;
                }
            }
            else if ((pvrHi >= 0x8000) && (pvrHi <= 0x8004))
            {
                /*
                 * S6 (REVIEW-LIB.md): THE MPC745x, PVR UPPER HALF 0x8000 TO 0x8004 (the range GetInfo
                 * takes for the 745x and 744x). It used to fall into the branch below, which writes the
                 * 7410's layout into a register that has another one (L2CR, MPC7450UM Table 2-12: bits
                 * 2-3 must be 0b00, 4-9 are reserved, no clock, size or test-support field), and polls
                 * L2IP, bit 31, which the part does not define, so the L2 could be enabled before its
                 * invalidate was done; it also left HID0[TBEN] clear: HID0 resets to 0x80000000 (Table
                 * 2-45), and the time base and the decrementer run only with TBEN set (Table 2-7).
                 * Now: TBEN is set with an isync after the mtspr (setHID0 ends in one; Table 2-7 note 1);
                 * the on-chip L2 is invalidated and enabled as 3.6.3.1.1 (PDF p. 295) has it, polling
                 * L2I, bit 10, which the chip clears when the invalidate is done; its size is the part's
                 * (256 KB for 0x8000 and 0x8001, 512 KB for 0x8002 and 0x8003, 1 MB for 0x8004: PDF
                 * pp. 60, 78 and 245); and the speed comes from 17.13b's table for 0x8000 with HID1 bits
                 * 15-19 (setupppc.p:1212-1224), and stays 0 for 0x8001 to 0x8004, whose tables are in
                 * the hardware specifications. 17.13b sets TBEN for 0x8000 only, without the isync
                 * (setupppc.p:585-592), and programs the 750FX's L2 sequence for it (:1106-1137): THIS
                 * DIVERGES in the PVR range, the isync and the poll. WarpOS: NO COUNTERPART (a search
                 * of warpos/ for TBEN and l2cr returns no line). Checked against the object and the
                 * manual; no 745x card has run it.
                 */
                if (pvrHi == 0x8004)
                {
                    l2Size = L2_SIZE_1M;
                }
                else if (pvrHi >= 0x8002)
                {
                    l2Size = L2_SIZE_HM;
                }
                else
                {
                    l2Size = L2_SIZE_QM;
                }

                value0 |= HID0_TBEN;
                setHID0(value0);

                if (!(getL2CR() & L2CR_L2E))
                {
                    setL2CR(L2CR_L2I);
                    while (getL2CR() & L2CR_L2I);
                    setL2CR(L2CR_L2E);
                }

                if (pvrHi == 0x8000)
                {
                    pll = (value1 >> 12) & 0x1f;

                    ULONG* pllTable = getTableVGer();

                    while (pllTable[num])
                    {
                        if (pllTable[num] == pll)
                        {
                            PowerPCBase->pp_CPUSpeed = pllTable[num+1];
                            break;
                        }
                        num +=2;
                    }
                }
            }
            else
            {
                /*
                 * S8 (REVIEW-LIB.md): the L2's size is found by function, largest first, with the L2
                 * disabled (getL2Size, setupppc.s), and the final L2CR is written with the size bits,
                 * invalidated and enabled in the manual's order (MPC7410UM 3.7.5, PDF p. 240). It used
                 * to program 1 MB with test support, enable the L2, count the surviving test lines,
                 * and then write the size bits with L2E set; its 2 MB test was commented out, as in
                 * 17.13b (setupppc.p:1172-1174), so a 2 MB L2 ran as 1 MB. cz_SizeBytes is 0 only if
                 * getL2Size did not run: it always returns a size (1 MB when nothing reads back clean).
                 */
                l2Setting = l2Clock | L2CR_L2RAM_BURST;

                getL2Size(l2Scratch, (APTR)&cz, l2Setting, allow2M);

                l2Setting |= cz.cz_SizeBit;
                l2Size = cz.cz_SizeBytes;

                setL2CR(l2Setting | L2CR_L2I);
                while (getL2CR() & L2CR_L2IP);
                setL2CR(l2Setting | L2CR_L2E);

                pll = value1 >> 28;

                ULONG* pllTable = getTable100();

                while (pllTable[num])
                {
                    if (pllTable[num] == pll)
                    {
                        PowerPCBase->pp_CPUSpeed = pllTable[num+1];
                        break;
                    }
                    num +=2;
                }
            }
            break;
        }
        case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
        {
            l2Size = 0;
            pll = (value1 >> 25) & 0x7f;
            value0 |= HID0_ICE | HID0_DCE | HID0_EMCP;
            setHID0(value0);
            /*
             * C6: THE L1 IS FLASH-INVALIDATED HERE, AFTER THE ENABLE. The one
             * at the start (setupppc.s, DCFI|ICFI) runs with both caches off,
             * and the e300 invalidates only an enabled cache (MPC8349EA RM
             * Table 7-2; MPC8315ERM Table 7-3, PDF pp. 351-352), so it did
             * nothing. A cache that was off keeps its lines: on the PCIe M1,
             * whose core was running the card's own Linux with its caches on,
             * those are Linux's lines -- stale ones that would shadow what this
             * set-up wrote with the caches off, and dirty ones that would later
             * be written back over the library's memory. The core has been
             * running this code with the caches off since _Reset, so no line
             * of ours is in them to lose. 17.13b's Caches routine enables them
             * with no invalidate at all (setupppc.p:1082-1090): THIS DIVERGES,
             * rightly. WarpOS starts no second CPU: NO COUNTERPART.
             */
            flashInvDI(value0);
            switch (pll)
            {
                case 0x23:
                case 0x06:
                {
                    PowerPCBase->pp_CPUSpeed = 400000000;
                    break;
                }
                case 0x25:
                {
                    PowerPCBase->pp_CPUSpeed = 333333333;
                    break;
                }
            }
            break;
        }
        case DEVICE_MPC107:
        {
            value0 |= HID0_ICE | HID0_DCE | HID0_SGE | HID0_BTIC | HID0_BHTE;
            setHID0(value0);

            /* S8: as the Harrier's branch above: the size by function with the L2 off, then the final L2CR,
             * the invalidate and the enable. 2 MB only on a 7400 or a 7410 (allow2M). */
            l2Setting = l2Clock | L2CR_L2RAM_BURST;

            getL2Size(l2Scratch, (APTR)&cz, l2Setting, allow2M);

            l2Setting |= cz.cz_SizeBit;
            l2Size = cz.cz_SizeBytes;

            setL2CR(l2Setting | L2CR_L2I);
            while (getL2CR() & L2CR_L2IP);
            setL2CR(l2Setting | L2CR_L2E);

            ULONG* pllTable;
            pll = value1 >> 28;

            if (initData->id_ConfigBase == 0x13)
            {
                pllTable = getTable100();
            }
            else
            {
                pllTable = getTable66();
            }

            while (pllTable[num])
            {
                if (pllTable[num] == pll)
                {
                    PowerPCBase->pp_CPUSpeed = pllTable[num+1];
                    break;
                }
                num +=2;
            }
            break;
        }
    }

    PowerPCBase->pp_L2Size = l2Size << 8;
    PowerPCBase->pp_CurrentL2Size = l2Size << 8;
    return;
}

/********************************************************************************************
*
*
*
*********************************************************************************************/

#ifdef PCIE_PT_TOP
/* DEBUG-INIT 7: a progress code to zp_Status (the zero-page status at 0x0C,
 * the one the host reads -- NOT id_Status), flushed so the host read is
 * current. The last code before a hang is the stage setupPPC died in. Debug
 * build only; no WarpOS counterpart (warp.p:3455-3459). */
#define DBGMARK(n) do { myZP->zp_Status = 0x50430000u | (n); dFlush((ULONG)&myZP->zp_Status); sync(); } while (0)
#else
#define DBGMARK(n) do { } while (0)
#endif

PPCSETUP void setupPPC(__reg("r3") struct InitData* initData)
{
    ULONG mem = 0;
    ULONG copySrc = 0;
    struct PPCZeroPage *myZP = 0;

    if (initData->id_DeviceID == DEVICE_MPC107)
    {
        struct MemSettings ms;

        writePCI(MPC107_PICR1, MPC107_PICR1_DEFAULT);
        writePCI(MPC107_PICR2, MPC107_PICR2_DEFAULT);
        writePCI16(MPC107_PMCR1, MPC107_PMCR1_DEFAULT);
        writePCI(MPC107_EUMBBAR, PPC_EUMB_BASE);

        if (initData->id_ConfigBase == 0x13)
        {
            writePCI(MPC107_MCCR4, 0x35303232);
            writePCI(MPC107_MCCR3, 0x78400000);
            writePCI(MPC107_MCCR2, 0x04400700);

            ms.ms_mccr1testhigh = 0x75880000;
            ms.ms_mccr1testlow1 = 0xaaaa;
            ms.ms_mccr1testlow2 = 0x5555;
            ms.ms_mccr1testlow3 = 0;
        }
        else
        {
            writePCI(MPC107_MCCR4, 0x00100000);
            writePCI(MPC107_MCCR3, 0x0002a29c);
            writePCI(MPC107_MCCR2, 0xe0001040); //EDO?
            writePCI(MPC107_MCCR1, 0xffe20000);


            ms.ms_mccr1testhigh = 0xffea0000;
            ms.ms_mccr1testlow1 = 0xffff;
            ms.ms_mccr1testlow2 = 0xaaaa;
            ms.ms_mccr1testlow3 = 0x5555;
        }

        writePCI(MPC107_MSAR1, 0);
        writePCI(MPC107_MESAR1, 0);
        writePCI(MPC107_MSAR2, 0);
        writePCI(MPC107_MESAR2, 0);
        writePCI(MPC107_MEAR1, -1);
        writePCI(MPC107_MEEAR1, 0);
        writePCI(MPC107_MEAR2, -1);
        writePCI(MPC107_MEEAR2, 0);
        writePCI(MPC107_MCCR1, ms.ms_mccr1testhigh);

        ms.ms_msar1 = 0;
        ms.ms_msar2 = 0;
        ms.ms_mear1 = 0;
        ms.ms_mear2 = 0;
        ms.ms_mesar1 = 0;
        ms.ms_mesar2 = 0;
        ms.ms_meear1 = 0;
        ms.ms_meear2 = 0;
        ms.ms_mben = 0;
        ms.ms_mccr1 = 0;

        detectMem((struct MemSettings*)&ms);

        writePCI(MPC107_MSAR1, ms.ms_msar1);
        writePCI(MPC107_MESAR1, ms.ms_mesar1);
        writePCI(MPC107_MSAR2, ms.ms_msar2);
        writePCI(MPC107_MESAR2, ms.ms_mesar2);
        writePCI(MPC107_MEAR1, ms.ms_mear1);
        writePCI(MPC107_MEEAR1, ms.ms_meear1);
        writePCI(MPC107_MEAR2, ms.ms_mear2);
        writePCI(MPC107_MEEAR2, ms.ms_meear2);
        writePCI8(MPC107_PGMAX, 0x32);
        writePCI8(MPC107_MBEN, ms.ms_mben);

        sync();

        /*
         * L10: THE PAUSE BETWEEN THE BANK ENABLE AND THE FINAL MCCR1, as 17.13b
         * has it (setupppc.p:1924-1928, .MPC107Wait200us: 0x10000 turns of bdnz,
         * about 200 us by its name). The Sonnet PCI 7200 (MPC107 revision 0x12,
         * the FPM/EDO timings above) does not start with the library without it
         * (the user, 14 Nov 2026). The Tsi107 manual: FPM or EDO DRAM is
         * available after eight refreshes once MEMGO is set (section 6.3.4,
         * PDF p. 275). The same loop form as the settle below, which vbcc keeps.
         */
        ULONG wait200 = 0x10000;

        while (wait200)
        {
            wait200--;
        }

        writePCI(MPC107_MCCR1, ms.ms_mccr1);

        ULONG settle = 0x2ffff;

        while (settle)
        {
            settle--;
        }

        initData->id_MemSize = ms.ms_memsize;
    }

    Reset();
 
    initData->id_Status = STATUS_INIT;

    for (LONG i=0; i<0x40; i++)
    {
        *((ULONG*)(mem)) = 0;
        mem += 4;
    }

    if (initData->id_DeviceID == DEVICE_MPC107)
    {
        dInval(0);
        while (initData->id_Status != STATUS_MEM);
        initData->id_Status = STATUS_INIT;
    }

    myZP->zp_MemSize = initData->id_MemSize;

    setupFIFOs(initData);

    switch (initData->id_DeviceID)
    {
        case DEVICE_HARRIER:
        {
            setupOpenPIC(initData);
            break;
        }
        case DEVICE_MPC8343E:
        case DEVICE_MPC8314E:
        {
            break;
        }
        case DEVICE_MPC107:
        {
            setupEPIC();
            break;
        }
    }

    ULONG* memx = 0;

    ULONG* IdleTask = (ULONG*)OFFSET_SYSMEM;
    IdleTask[0] = OPCODE_NOP;
    IdleTask[1] = OPCODE_NOP;
    IdleTask[2] = OPCODE_BBRANCH - 8;
    
    installExceptions();

    mmuSetup(initData);
                
    while (initData->id_Status != STATUS_INIT);

    initData->id_Status = ERR_PPCOK;
    dFlush((ULONG)initData);

    while (myZP->zp_Status != STATUS_INIT)
    {
        dInval(0);
    }
    struct PrivatePPCBase* PowerPCBase = (struct PrivatePPCBase*)myZP->zp_PowerPCBase;

    DBGMARK(1);

    NewListPPC((struct List*)&PowerPCBase->pp_WaitingTasks);
    NewListPPC((struct List*)&PowerPCBase->pp_AllTasks);
    NewListPPC((struct List*)&PowerPCBase->pp_Snoop);
    NewListPPC((struct List*)&PowerPCBase->pp_Semaphores);
    NewListPPC((struct List*)&PowerPCBase->pp_RemovedExc);
    NewListPPC((struct List*)&PowerPCBase->pp_ReadyExc);
    NewListPPC((struct List*)&PowerPCBase->pp_InstalledExc);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcInterrupt);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcIABR);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcPerfMon);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcTrace);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcSystemCall);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcDecrementer);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcFPUn);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcProgram);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcAlign);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcIAccess);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcDAccess);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcMCheck);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcSysMan);
    NewListPPC((struct List*)&PowerPCBase->pp_ExcTherMan);
    NewListPPC((struct List*)&PowerPCBase->pp_WaitTime);
    NewListPPC((struct List*)&PowerPCBase->pp_Ports);
    NewListPPC((struct List*)&PowerPCBase->pp_NewTasks);
    NewListPPC((struct List*)&PowerPCBase->pp_ReadyTasks);
    NewListPPC((struct List*)&PowerPCBase->pp_RemovedTasks);
    NewListPPC((struct List*)&PowerPCBase->pp_TaskLinks);           //a handler's SignalPPC, for HandleDeferred
    NewListPPC((struct List*)&PowerPCBase->pp_MsgQueue);

    initSema(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemWaitList);
    initSema(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemTaskList);
    initSema(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSemList);
    initSema(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemPortList);
    initSema(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemSnoopList);
    initSema(PowerPCBase, (struct SignalSemaphorePPC*)&PowerPCBase->pp_SemMemory);

    DBGMARK(2);

    PowerPCBase->pp_PPCMemSize = initData->id_MemSize;
    PowerPCBase->pp_IdUsrTasks = 100;
    PowerPCBase->pp_BusyCounter = 24;
    /* powerpc.p:906, "stwi PRI_MAXDIST,PPC_REACTION(r30)" -- THIS is the
     * field that slot holds. The SCHED_REACTION tag overrides it at
     * functionsppc.c:3496, as the original does at :13215-13231. */
    PowerPCBase->pp_LowActivityPri = PRI_MAXDIST;
    PowerPCBase->pp_EnAlignExc = initData->id_Environment1 >> 8;
    PowerPCBase->pp_EnDAccessExc = initData->id_Environment2 >> 8;
    PowerPCBase->pp_CacheDisDFlushAll = initData->id_Environment2 >> 24;
    PowerPCBase->pp_DebugLevel = initData->id_Environment1 >> 16;
    PowerPCBase->pp_CPUInfo = getPVR();
    PowerPCBase->pp_PageTableSize = myZP->zp_PageTableSize;

    ULONG quantum = QUANTUM_100;
    ULONG busclock = BUSCLOCK_100;

    DBGMARK(3);

    switch (PowerPCBase->pp_DeviceID)
    {
        case DEVICE_HARRIER:
        {
            if ((PowerPCBase->pp_CPUInfo >> 16 & 0xfff) == 0xc)
            {
#ifndef G4_KEEP_ALTIVEC
                if (!(PowerPCBase->pp_PowerPCBase.PPC_Flags & LIB_FLAGPPC_RG3))
#endif
                {
                    PowerPCBase->pp_EnAltivec = -1;
                }
                /* With ENV:sonnet/ReportG4asG3 (LIB_FLAGPPC_RG3, set by the
                 * 68k before it hands this base over; forced on in the
                 * G4-as-G3 test build) it stays 0, and that is every AltiVec
                 * path: kernel.c's AltiVec-unavailable case refuses, so no
                 * task gets PSL_VEC, so kernel.s never saves or restores a
                 * vector register, and CopyMemPPC copies without AltiVec.
                 * G4_KEEP_ALTIVEC, that build's second variant, sets it
                 * regardless. */
            }
            break;
        }
        case DEVICE_MPC8343E:
        {
            quantum = QUANTUM_E300;
            busclock = BUSCLOCK_E300;

            /*
             * S4: RCWLR IS READ AS THE BIG-ENDIAN REGISTER IT IS. loadPCI is lwbrx,
             * for the little-endian message unit registers; read through it the word
             * arrives byte-reversed, and (1<<RCWLR_DDRCM) then tested bit 25 of the
             * register (0x02000000, a bit of SPMF) and never DDRCM. DDRCM is bit 1 in
             * the manual's numbering, 0x40000000 (MPC8349EA RM, Figure 4-3 and Table
             * 4-8, PDF pages 163-164): set, the DDR controller runs at twice csb_clk,
             * and the csb clock, which the quantum and the time base follow, is half
             * of BUSCLOCK_E300. The read is readmemLongPPC (lwzx), as every other
             * big-endian IMMR register here, and the 17.13b read (lwz, setupppc.p:486).
             * 17.13b halves when either of the two top bits is set (rlwinm. 2,30,31,
             * :487-488), LBIUCM (the local bus ratio, which says nothing of csb_clk)
             * or DDRCM: this tests DDRCM alone, so THE LIBRARY DIVERGES on LBIUCM
             * (tests/rcwlr prints both bits). WarpOS has no such board (a search of
             * warpos/ for RCWLR, DDRCM and MPC8343 finds no line, 12 Nov 2026).
             */
            if (readmemLongPPC(IMMR_ADDR_DEFAULT, IMMR_RCWLR) & (0x80000000UL >> RCWLR_DDRCM))
            {
                quantum = quantum >> 1;
                busclock = busclock >> 1;
            }
            break;
        }
        case DEVICE_MPC8314E:
        {
            quantum = QUANTUM_E300;
            busclock = BUSCLOCK_E300;

            /*
             * S10: THE PCIe M1's CLOCKS FOLLOW RCWLR, AS THE MPC8343E's DO (S4 above). The
             * two constants were halved whatever the strap: right on a card strapped DDRCM
             * 1, which is the one board read so far (RCWLR 0x44060000, 6 Nov 2026), and
             * twice too fast, time conversions and task slice alike, on one strapped DDRCM
             * 0. The MPC8315E RM's RCWLR (Rev. 2, Figure 4-3 and Table 4-8, PDF p. 181) has
             * DDRCM at bit 1 as the MPC8343E's does, 0x40000000: set, the DDR controller
             * runs at twice csb_clk, which the quantum and the time base follow. RCWLR is
             * an ordinary big-endian CCSR register in the reset configuration block at
             * 0x900 (Table 3-1, PDF p. 166), so the read is readmemLongPPC (lwzx), as in
             * the MPC8343E's case. The model is the same: the DDR clock is the table's
             * value and csb_clk is that divided by one plus DDRCM; a card with another DDR
             * clock is served by neither this nor the constants (REVIEW-LIB.md S10, option
             * C). 17.13b reads RCWLR in one place, its K1 branch (setupppc.p:484-492; a
             * search of src_173b finds the name there and in ppcdefines.i:1046 alone) and
             * has no PCIe M1; WarpOS: NO COUNTERPART (a search of warpos/ for RCWLR,
             * DDRCM, MPC8314, MPC8315 and MPC8343 returns no line, 12 Nov 2026).
             * tests/rcwlr decodes this card's word and checks the clocks.
             */
            if (readmemLongPPC(IMMR_ADDR_DEFAULT, IMMR_RCWLR) & (0x80000000UL >> RCWLR_DDRCM))
            {
                quantum = quantum >> 1;
                busclock = busclock >> 1;
            }
            writememLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_IMBCR, 0);
            writememLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_CSMISR, readmemLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_CSMISR));
            //storePCI16(IMMR_ADDR_DEFAULT, IMMR_PEX_MSICR, (loadPCI16(IMMR_ADDR_DEFAULT, IMMR_PEX_MSICR) | PEX_MISCR_MSIE));
            writememLongPPC(IMMR_ADDR_DEFAULT, IMMR_PEX_HIER, PEX_HIER_OMBIE);
            break;
        }
        case DEVICE_MPC107:
        {
            if (initData->id_ConfigBase != 0x13)
            {
                quantum = QUANTUM_66;
                busclock = BUSCLOCK_66;
            }
            if ((PowerPCBase->pp_CPUInfo >> 16 & 0xfff) == 0xc)
            {
#ifndef G4_KEEP_ALTIVEC
                if (!(PowerPCBase->pp_PowerPCBase.PPC_Flags & LIB_FLAGPPC_RG3))
#endif
                {
                    PowerPCBase->pp_EnAltivec = -1;
                }
                /* With ENV:sonnet/ReportG4asG3 (LIB_FLAGPPC_RG3, set by the
                 * 68k before it hands this base over; forced on in the
                 * G4-as-G3 test build) it stays 0, and that is every AltiVec
                 * path: kernel.c's AltiVec-unavailable case refuses, so no
                 * task gets PSL_VEC, so kernel.s never saves or restores a
                 * vector register, and CopyMemPPC copies without AltiVec.
                 * G4_KEEP_ALTIVEC, that build's second variant, sets it
                 * regardless. */
            }
            break;
        }
    }

    /*
     * The NICE weighting table, FIXES.md item 18, is NOT pointed at from here
     * any more (S3). This stored GetNiceTable()'s result, the address of the
     * copy in setupppc.s -- in the SETUP hunk, by the address that hunk RUNS
     * at: on a Harrier the high alias 0xFFF13000 (MSR[IP] is set at reset,
     * ENRV maps RAM at 0xFFF00000, the reset branch is relative), so
     * 0xFFF13404, and the pointer stayed there after mmuSetup cleared IP. User
     * code reads it only through the vector window's PTE (:238).
     * mymakeLibrary (libinit.c) now stores the address of supportppc.s's
     * NiceTable in the functions hunk, which is the library's own RAM, and it
     * does so before this runs; storing here would have put the setup copy
     * back. GetNiceTable and its copy stay in setupppc.s unused. The values:
     * setupppc.s says why they are in assembler -- under -sc a C table is
     * addressed through the small-data area and this library has no
     * _SDA_BASE_.
     */

    /*
     * THE RUNNING MINIMUM STARTS AT ZERO, and this line used to set it to
     * PRI_MAXDIST with powerpc.p:906's citation attached -- the right citation
     * on the wrong field. pp_LowActivityPriOffset sits in PPC_MINPRI's slot,
     * not PPC_REACTION's (see prirule.h for how that is settled), and the
     * original never initialises PPC_MINPRI at all: it is written in exactly
     * two places, lowered to a task's effective priority at :4888 and raised
     * by the per-pass relief at :5578. Setting it to 6000 put the enqueue
     * ceiling at MINPRI + REACTION = 12000 instead of 6000 from the first
     * instruction. Written explicitly rather than relying on the allocation
     * being zeroed.
     */
    PowerPCBase->pp_LowActivityPriOffset = 0;

    PowerPCBase->pp_LowerLimit = *((ULONG*)(((ULONG)PowerPCBase + _LVOInsertPPC + 2)));
    PowerPCBase->pp_UpperLimit = *((ULONG*)(((ULONG)PowerPCBase + _LVOFindNamePPC + 2)));
    PowerPCBase->pp_StdQuantum = quantum;
    PowerPCBase->pp_BusClock = busclock;

    DBGMARK(4);

    /*
     * pp_ExceptionBATs: the set a WarpOS SetExcMMU would load (WarpOS loads
     * PPC_BAT, powerpc.p:8116-8148). The same live BATs as pp_SystemBATs
     * below, and read by nothing: SetExcMMU is a no-op here, as in 17.13b,
     * because the handlers already run under the library's set. FIXES.md
     * "ITEM 6, SetExcMMU / ClearExcMMU", 31 Oct 2026.
     */
    mvfrBAT0(&PowerPCBase->pp_ExceptionBATs[0]);
    mvfrBAT1(&PowerPCBase->pp_ExceptionBATs[1]);
    mvfrBAT2(&PowerPCBase->pp_ExceptionBATs[2]);
    mvfrBAT3(&PowerPCBase->pp_ExceptionBATs[3]);

    mvfrBAT0(&PowerPCBase->pp_SystemBATs[0]);
    mvfrBAT1(&PowerPCBase->pp_SystemBATs[1]);
    mvfrBAT2(&PowerPCBase->pp_SystemBATs[2]);
    mvfrBAT3(&PowerPCBase->pp_SystemBATs[3]);

    for (int i = 0; i < 16; i++)
    {
        PowerPCBase->pp_SystemSegs[i] = getSRIn(i<<28);
    }

    DBGMARK(5);

    /*
     * THE SAME SET AGAIN, AT PHYSICAL 0x2000, for the exception entry in
     * kernel.s. PowerPCBase is a 68k-view address and the entry runs
     * untranslated until it has installed a known MMU state, so the state it
     * installs has to live where EA == PA: the low 2 MB under BAT0. Reached
     * here translated through that same BAT. FIXES.md "THE MSR UNDER THE
     * EXCEPTION PATH", 23 Oct 2026.
     */
    {
        ULONG* libBATs = (ULONG*)MMU_ZERO_LIB_BATS;
        ULONG* libSegs = (ULONG*)MMU_ZERO_LIB_SEGS;
        ULONG* src     = (ULONG*)&PowerPCBase->pp_SystemBATs[0];

        for (int i = 0; i < 16; i++)
        {
            libBATs[i] = src[i];
            libSegs[i] = PowerPCBase->pp_SystemSegs[i];
        }
        *((ULONG*)MMU_ZERO_LIB_SDR1) = getSDR1();
#ifdef PCIE_PT_TOP
        /*
         * DEBUG-INIT 6: push the save area to RAM. _ExcCommon reads the
         * library BATs/segs/SDR1 from here with translation OFF (kernel.s),
         * and BAT0 is copyback (setupppc.c:330), so without this the writes
         * sit in the d-cache while physical RAM keeps whatever was there -- on
         * the PCIe M1 that is the card's resident firmware code, which the
         * untranslated read then loads as BAT0 and faults on (the 0x32C4 ISI).
         * NO WarpOS counterpart (its exception path switches no MMU,
         * warp.p:3455-3459); 17.13b has no MPC8314E path. Harmless on a card
         * with no resident code (the K1 resets into a bare card).
         */
        for (ULONG a = MMU_ZERO_LIB_BATS; a <= MMU_ZERO_LIB_SDR1; a += CACHELINE_SIZE)
        {
            dFlush(a);
        }
        sync();
#endif
    }

    DBGMARK(6);

    ULONG* cmem = (APTR)(PowerPCBase->pp_PPCMemBase + MEM_GAP - 0xC000);
    for (int i = 0; i < 0x1000; i++)
    {
        cmem[i] = 0;
    }

    setupCaches(PowerPCBase, initData);

    DBGMARK(7);

    /*
     * SPRG0 GETS THE MEANING IT HAS UNDER REAL WarpOS: a pointer, set once at
     * init, that exception entry never overwrites. warp.p:170-172 points it at
     * the top of a 16 KB exception stack plus 32 bytes, and every WarpOS
     * exception entry moves DOWN from it (warp.p:3453), so those top words are
     * dead space. iFUSION's guest trampoline at h2:0x0058 borrows them: it does
     * "mfspr r1,SPRG0" and writes four words at 0(r1)..12(r1) to preserve the
     * guest's r3-r6 across its host-service "sc". Nothing in iFUSION ever
     * writes SPRG0 -- it expects to find it set up.
     *
     * zp_ExcScratch is eight words in page zero serving that purpose. It is
     * absolutely addressed: no allocation, no assumption about the mapping.
     * BAT0, which covers page zero, is supervisor-only (setupppc.c:316) --
     * enough, since reading SPRG0, which finds it, is privileged (R6).
     *
     * This is only safe because kernel.s no longer stages SRR0/SRR1 in
     * SPRG0/SPRG1 -- it uses zp_ExcSRR0/zp_ExcSRR1 -- and _SmallExcHandler no
     * longer parks anything in the SPRGs. See FIXES.md item 9.
     */
    /*
     * THE KERNEL'S EXCEPTION STACK (constants.h EXCSTACK_*): the entry's r1
     * for the outermost exception, as WarpOS's stubs take theirs from SPRG0
     * (warp.p:3453). Until this word is written the entry keeps using the
     * interrupted r1. FIXES.md "THE LIBRARY HALF OF R", 24 Oct 2026.
     */
    *((ULONG*)MMU_ZERO_EXCDEPTH) = 0;
    *((ULONG*)MMU_ZERO_BUILDMARK) = LIB_BUILDMARK;
#ifdef EXC_TIMING
    for (ULONG a = MMU_ZERO_TIME_T0; a < MMU_ZERO_TIME_MARK; a += 4)
    {
        *((ULONG*)a) = 0;
    }
    *((ULONG*)MMU_ZERO_TIME_MARK) = TIME_MARK_VALUE;
#endif
    *((ULONG*)MMU_ZERO_EXCSTACK) = (PowerPCBase->pp_ExcStackMem)
        ? (((ULONG)PowerPCBase->pp_ExcStackMem + EXCSTACK_SIZE - EXCSTACK_TOPGAP) & ~15UL)
        : 0;                     /* 0: the entry keeps the interrupted r1 */
    setSPRG0((ULONG)&myZP->zp_ExcScratch[0]);

    myZP->zp_Status = STATUS_READY;

    setDEC(PowerPCBase->pp_StdQuantum);
    setSRR0((ULONG)(PowerPCBase->pp_PPCMemBase) + OFFSET_SYSMEM);
    setSRR1(MACHINESTATE_DEFAULT);

    return;
}

