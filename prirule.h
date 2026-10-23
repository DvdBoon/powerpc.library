#ifndef PRIRULE_H
#define PRIRULE_H

/*
 * NEWPRIRULE -- THE V14 PRIORITY RULE, FIXES.md item 1.
 *
 * The original ships this ENABLED: powerpc_local.i:14-15 has
 * NEWSCHEDULER = -1 and NEWPRIRULE = -1, and powerpc.p:39 has FILTER = 0, so
 * there is no averaging of the activity and busy ratios. Everything here is
 * inside one of the original's IFNE NEWPRIRULE blocks.
 *
 * THE RULE IN ONE LINE: a task's effective priority is
 * tp_Priority + tp_Prioffset, it falls as the task consumes CPU relative to
 * the share tp_Desired says it deserves, no task may sit more than
 * pp_LowActivityPri above the running minimum, and when the minimum falls
 * further than that below zero the whole scale is dragged back up, relieving
 * every task by the same amount.
 *
 * THE TWO BASE FIELDS ARE NAMED FOR THE OPPOSITE OF WHAT THEY HOLD, and this
 * is settled, not guessed. powerpc_local.i:156-166 runs
 *
 *     PPC_CPULOAD / PPC_SYSTEMLOAD / PPC_NICETAB / PPC_REACTION /
 *     PPC_TASKID_SYSTEM / PPC_TASKID_CUSTOM / PPC_MINPRI
 *
 * and libstructs.h runs pp_SystemLoad / pp_CPULoad / pp_NICETable /
 * pp_LowActivityPri / pp_IdSysTasks / pp_IdUsrTasks /
 * pp_LowActivityPriOffset -- the same slots in the same order. Behaviour
 * confirms it independently: the original defaults PPC_REACTION to PRI_MAXDIST
 * (powerpc.p:906) and lets the SCHED_REACTION tag set it clamped to 1..20 then
 * multiplied by 1000 (:13215-13231), which is exactly what setupppc.c:968 and
 * functionsppc.c:3178-3189 do to pp_LowActivityPri. So:
 *
 *     pp_LowActivityPri        IS  PPC_REACTION   the tunable DISTANCE, 6000
 *     pp_LowActivityPriOffset  IS  PPC_MINPRI     the running MINIMUM, from 0
 *
 * PPC_MINPRI is written in only two places in the whole original -- lowered to
 * a task's effective priority at :4888 and raised by the per-pass relief at
 * :5578 -- and is never initialised, so it starts at zero. setupppc.c:1036 was
 * setting it to PRI_MAXDIST, which put the enqueue ceiling at 12000 instead of
 * 6000 from the first instruction.
 *
 * SIGNEDNESS IS LOAD-BEARING. tp_Priority, tp_Prioffset and tp_Nice are all
 * ULONG in the public tasksPPC.h (:47-49, :56), but the original compares them
 * with cmpw and branches on blt/bge after neg and subf -- signed throughout.
 * Every read here casts to LONG and every write casts back.
 *
 * WHY THIS IS A HEADER AND NOT A FUNCTION. kernel.c is in the KERNEL hunk and
 * supportppc.c is in the FUNCTIONS hunk, and the two are copied to unrelated
 * addresses with only the LVO table relocated, so a direct call between them
 * lands where the callee used to be -- that was the 21 Oct init crash. Public
 * entry points like EnqueuePPC are safe because the inline routes them through
 * the LVO table, but these are private. Defining them static in a header gives
 * each hunk its own copy from ONE source, with no call to relocate.
 */

/* The comparison key everywhere a priority is tested: powerpc.p:3064-3066,
 * :4644-4652, :4880-4882, :4896-4898, :5538-5540. */
static LONG priEffective(struct TaskPPC* task)
{
    return (LONG)task->tp_Priority + (LONG)task->tp_Prioffset;
}

/* The ceiling no enqueued task may exceed: MINPRI + REACTION.
 * powerpc.p:3067-3069 and :4899-4901 both build it this way. */
static LONG priCeiling(struct PrivatePPCBase* PowerPCBase)
{
    return PowerPCBase->pp_LowActivityPriOffset + PowerPCBase->pp_LowActivityPri;
}

/* Is a MinList empty? The original's idiom, powerpc.p:4613-4615:
 * "lwz r0,LH_TAILPRED(r4) / cmplw r0,r4 / beq empty". */
static LONG priListEmpty(struct MinList* list)
{
    return (list->mlh_TailPred == (struct MinNode*)list);
}

/*
 * THE DECAY, powerpc.p:4873-4877 and :4617-4626.
 *
 *     lwz   r3,TASKPPC_DESIRED(r5)
 *     divwu r3,r7,r3            ; r7 = tp_Totalelapsed, AFTER this slice
 *     neg   r3,r3
 *     subi  r3,r3,1
 *     stw   r3,TASKPPC_PRIORITY(r5)
 *
 * so tp_Priority = -(tp_Totalelapsed / tp_Desired) - 1, always <= -1: the
 * further a task is past its share, the more negative it goes. It is an
 * ASSIGNMENT, not an accumulation -- the running total lives in
 * tp_Totalelapsed, and ReSchedule folds each snapshot into tp_Prioffset and
 * zeroes tp_Priority.
 *
 * tp_Desired == 0 IS GUARDED HERE and is not a rare case: functionsppc.c:553
 * creates every task with zero and only CPUStats fills it from the NICE table,
 * so every task is in this state until the first pass. divwu by zero does not
 * trap on PowerPC, it returns an undefined result -- the same silent-garbage
 * hazard as the tp_Busy division fixed in 57984e9. A quotient of "infinity"
 * would mean infinitely over share and would starve every new task, so with
 * nothing measured yet this makes no judgement and leaves tp_Priority alone.
 */
static void priDecay(struct TaskPPC* task)
{
    if (task->tp_Desired)
    {
        LONG over = (LONG)(task->tp_Totalelapsed / task->tp_Desired);

        task->tp_Priority = (ULONG)(-over - 1);
    }
}

/*
 * THE CEILING APPLIED TO ONE TASK: rewrite tp_Prioffset so that
 * tp_Priority + tp_Prioffset lands exactly on the ceiling.
 * powerpc.p:3072-3075 "lwz r0,TASKPPC_PRIORITY(r5) / subf r0,r0,r3 /
 * stw r0,TASKPPC_PRIOFFSET(r5)", and identically at :4903-4906.
 */
static void priPullToCeiling(struct TaskPPC* task, LONG ceiling)
{
    task->tp_Prioffset = (ULONG)(ceiling - (LONG)task->tp_Priority);
}

/*
 * LOWER THE RUNNING MINIMUM AND PULL THE READY LIST DOWN WITH IT,
 * powerpc.p:4884-4908.
 *
 * If the outgoing task's effective priority is below the minimum, it BECOMES
 * the minimum -- and because the ceiling is MINPRI + REACTION, the ceiling has
 * just moved down too, so every ready task now above it is pulled onto it.
 * That is what bounds the spread: no task can ever be more than REACTION above
 * the least-favoured one, so none can starve behind it.
 *
 * The ceiling is recomputed inside the loop because the original reloads
 * PPC_MINPRI and PPC_REACTION on every iteration (:4899-4901). It cannot
 * change during the walk, but reading it once would be a different program
 * than the one being reproduced.
 */
static void priLowerMinimum(struct PrivatePPCBase* PowerPCBase, struct TaskPPC* task)
{
    LONG effective = priEffective(task);

    if (effective < PowerPCBase->pp_LowActivityPriOffset)
    {
        struct MinNode* node = PowerPCBase->pp_ReadyTasks.mlh_Head;
        struct MinNode* next;

        PowerPCBase->pp_LowActivityPriOffset = effective;

        while (next = node->mln_Succ)
        {
            struct TaskPPC* chkTask = (struct TaskPPC*)node;
            LONG ceiling = priCeiling(PowerPCBase);

            if (priEffective(chkTask) > ceiling)
            {
                priPullToCeiling(chkTask, ceiling);
            }
            node = next;
        }
    }
}

/*
 * THE PRIORITY-ORDERED INSERT, powerpc.p:3060-3105 (EnqueueTask).
 *
 * Two things at once: cap the incoming task's effective priority at the
 * ceiling -- rewriting its tp_Prioffset if it is over -- and then insert it by
 * effective priority rather than by ln_Pri, which is why the ready list cannot
 * use EnqueuePPC. Under NEWSCHEDULER the original's Schedule enqueues the
 * outgoing task with EnqueueTask too (:4918-4921), not with EnqueuePPC, so the
 * hot path goes through this.
 *
 * The ATOMIC arm is the original's at :3085-3092: an atomic task already in
 * the list is not displaced, the walk stops just past it and the new task goes
 * there. TASKPPCF_ATOMIC is (1L<<5) here, overriding the public header's
 * (1L<<4) at Internalsppc.h:65-71.
 */
static void priInsert(struct PrivatePPCBase* PowerPCBase, struct MinList* list,
                      struct TaskPPC* myTask)
{
    LONG realPri = priEffective(myTask);
    LONG ceiling = priCeiling(PowerPCBase);
    struct MinNode* node = list->mlh_Head;
    struct MinNode* next;
    struct MinNode* pred;

    if (realPri >= ceiling)
    {
        realPri = ceiling;
        priPullToCeiling(myTask, ceiling);
    }

    while (next = node->mln_Succ)
    {
        struct TaskPPC* chkTask = (struct TaskPPC*)node;

        if (chkTask->tp_Flags & TASKPPCF_ATOMIC)
        {
            node = next;
            break;
        }
        if (realPri > priEffective(chkTask))
        {
            break;
        }
        node = next;
    }

    pred = node->mln_Pred;
    node->mln_Pred = (struct MinNode*)myTask;
    myTask->tp_Task.tc_Node.ln_Succ = (struct Node*)node;
    myTask->tp_Task.tc_Node.ln_Pred = (struct Node*)pred;
    pred->mln_Succ = (struct MinNode*)myTask;
}

#endif  /* PRIRULE_H */
