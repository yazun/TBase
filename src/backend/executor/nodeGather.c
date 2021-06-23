/*-------------------------------------------------------------------------
 *
 * nodeGather.c
 *      Support routines for scanning a plan via multiple workers.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * A Gather executor launches parallel workers to run multiple copies of a
 * plan.  It can also run the plan itself, if the workers are not available
 * or have not started up yet.  It then merges all of the results it produces
 * and the results from the workers into a single output stream.  Therefore,
 * it will normally be used with a plan where running multiple copies of the
 * same plan does not produce duplicate output, such as parallel-aware
 * SeqScan.
 *
 * Alternatively, a Gather node can be configured to use just one worker
 * and the single-copy flag can be set.  In this case, the Gather node will
 * run the plan in one worker and will not execute the plan itself.  In
 * this case, it simply returns whatever tuples were returned by the worker.
 * If a worker cannot be obtained, then it will run the plan itself and
 * return the results.  Therefore, a plan used with a single-copy Gather
 * node need not be parallel-aware.
 *
 * IDENTIFICATION
 *      src/backend/executor/nodeGather.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/xact.h"
#include "executor/execdebug.h"
#include "executor/execParallel.h"
#include "executor/nodeGather.h"
#include "executor/nodeSubplan.h"
#include "executor/tqueue.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "postmaster/postmaster.h"
#ifdef __TBASE__
#include "pgxc/squeue.h"
#endif
static TupleTableSlot *ExecGather(PlanState *pstate);
static TupleTableSlot *gather_getnext(GatherState *gatherstate);
static HeapTuple gather_readnext(GatherState *gatherstate);
static void ExecShutdownGatherWorkers(GatherState *node);


/* ----------------------------------------------------------------
 *        ExecInitGather
 * ----------------------------------------------------------------
 */
GatherState *
ExecInitGather(Gather *node, EState *estate, int eflags)
{
    GatherState *gatherstate;
    Plan       *outerNode;
    bool        hasoid;
    TupleDesc    tupDesc;

    /* Gather node doesn't have innerPlan node. */
    Assert(innerPlan(node) == NULL);

    /*
     * create state structure
     */
    gatherstate = makeNode(GatherState);
    gatherstate->ps.plan = (Plan *) node;
    gatherstate->ps.state = estate;
    gatherstate->ps.ExecProcNode = ExecGather;
    gatherstate->need_to_scan_locally = !node->single_copy;
#ifdef __TBASE__
    gatherstate->get_tuples     = 0;
    gatherstate->get_total_time = -1;
#endif
    /*
     * Miscellaneous initialization
     *
     * create expression context for node
     */
    ExecAssignExprContext(estate, &gatherstate->ps);

    /*
     * initialize child expressions
     */
    gatherstate->ps.qual =
        ExecInitQual(node->plan.qual, (PlanState *) gatherstate);

    /*
     * tuple table initialization
     */
    gatherstate->funnel_slot = ExecInitExtraTupleSlot(estate);
    ExecInitResultTupleSlot(estate, &gatherstate->ps);

    /*
     * now initialize outer plan
     */
    outerNode = outerPlan(node);
    outerPlanState(gatherstate) = ExecInitNode(outerNode, estate, eflags);

    /*
     * Initialize result tuple type and projection info.
     */
    ExecAssignResultTypeFromTL(&gatherstate->ps);
    ExecAssignProjectionInfo(&gatherstate->ps, NULL);

    /*
     * Initialize funnel slot to same tuple descriptor as outer plan.
     */
    if (!ExecContextForcesOids(&gatherstate->ps, &hasoid))
        hasoid = false;
    tupDesc = ExecTypeFromTL(outerNode->targetlist, hasoid);
    ExecSetSlotDescriptor(gatherstate->funnel_slot, tupDesc);

    return gatherstate;
}

/* ----------------------------------------------------------------
 *        ExecGather(node)
 *
 *        Scans the relation via multiple workers and returns
 *        the next qualifying tuple.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecGather(PlanState *pstate)
{// #lizard forgives
    GatherState *node = castNode(GatherState, pstate);
    TupleTableSlot *fslot = node->funnel_slot;
    int            i;
    TupleTableSlot *slot;
    ExprContext *econtext;
#ifdef __TBASE__
    bool  parallel_send   = false;
    int   launchedWorkers = 0;
    TimestampTz begin = 0;
    TimestampTz end = 0;
#endif
    CHECK_FOR_INTERRUPTS();

    /*
     * Initialize the parallel context and workers on first execution. We do
     * this on first execution rather than during node initialization, as it
     * needs to allocate a large dynamic segment, so it is better to do it
     * only if it is really needed.
     */
#ifdef __TBASE__
    if (IsParallelWorker())
    {
        if (!node->initialized)
        {
            node->reader = NULL;
            node->need_to_scan_locally = (node->reader == NULL);
            node->initialized = true;
        }
    }
    else
    {
#endif
    if (!node->initialized)
    {
        EState       *estate = node->ps.state;
        Gather       *gather = (Gather *) node->ps.plan;
        parallel_send    = gather->parallelWorker_sendTuple;

        /*
         * Sometimes we might have to run without parallelism; but if parallel
         * mode is active then we can try to fire up some workers.
         */
        if (gather->num_workers > 0 && IsInParallelMode())
        {
            ParallelContext *pcxt;
            ParallelWorkerStatus *num_parallel_workers = NULL;

			/* Initialize, or re-initialize, shared state needed by workers. */
            if (!node->pei)
#ifdef __TBASE__
                node->pei = ExecInitParallelPlan(node->ps.lefttree,
                                                 estate,
                                                 gather->num_workers,
                                                 gather);
#else
                node->pei = ExecInitParallelPlan(node->ps.lefttree,
                                                 estate,
                                                 gather->num_workers);
#endif
			else
				ExecParallelReinitialize(node->ps.lefttree,
										 node->pei);

            /*
             * Register backend workers. We might not get as many as we
             * requested, or indeed any at all.
             */
            pcxt = node->pei->pcxt;
            LaunchParallelWorkers(pcxt);
            node->nworkers_launched = pcxt->nworkers_launched;

            /* Set up tuple queue readers to read the results. */
            if (pcxt->nworkers_launched > 0)
            {
                if (!parallel_send)
                {
                    node->nreaders = 0;
                    node->nextreader = 0;
                    node->reader =
                        palloc(pcxt->nworkers_launched * sizeof(TupleQueueReader *));

                    for (i = 0; i < pcxt->nworkers_launched; ++i)
                    {
                        shm_mq_set_handle(node->pei->tqueue[i],
                                          pcxt->worker[i].bgwhandle);
                        node->reader[node->nreaders++] =
                            CreateTupleQueueReader(node->pei->tqueue[i],
                                                   fslot->tts_tupleDescriptor);
                    }
                }
                else
                {
                    node->nreaders = 0;
                    node->nextreader = 0;
                    node->reader = NULL;
                }

#ifdef __TBASE__
                launchedWorkers = pcxt->nworkers_launched;
                /* set up launched parallel workers' total number in shm */
                num_parallel_workers = GetParallelWorkerStatusInfo(pcxt->toc);
                num_parallel_workers->numLaunchedWorkers       = pcxt->nworkers_launched;
                num_parallel_workers->parallelWorkersSetupDone = true;
#endif
            }
            else
            {
                /* No workers?    Then never mind. */
                ExecShutdownGatherWorkers(node);
            }
        }

#ifdef __TBASE__
        if (parallel_send)
            node->need_to_scan_locally = false;
        else
            node->need_to_scan_locally = (node->reader == NULL);
#else
        /* Run plan locally if no workers or not single-copy. */
        node->need_to_scan_locally = (node->reader == NULL)
            || !gather->single_copy;
#endif
        node->initialized = true;
    }
#ifdef __TBASE__
    }
#endif
    /*
     * Reset per-tuple memory context to free any expression evaluation
     * storage allocated in the previous tuple cycle.  This will also clear
     * any previous tuple returned by a TupleQueueReader; to make sure we
     * don't leave a dangling pointer around, clear the working slot first.
     */
    ExecClearTuple(fslot);
    econtext = node->ps.ps_ExprContext;
    ResetExprContext(econtext);

#ifdef __TBASE__
    if (parallel_send)
    {
        WaitForParallelWorkerDone(launchedWorkers, (launchedWorkers == 0));

        return NULL;
    }
#endif

#ifdef __TBASE__
    if (enable_statistic && !node->need_to_scan_locally)
    {
        begin = GetCurrentTimestamp();
    }
#endif
    /*
     * Get next tuple, either from one of our workers, or by running the plan
     * ourselves.
     */
    slot = gather_getnext(node);
    if (TupIsNull(slot))
#ifdef __TBASE__
    {
        if (enable_statistic && !node->need_to_scan_locally)
        {
            elog(LOG, "Gather: get_tuples:%lu, get_total_time:%ld, avg_time:%lf.",
                       node->get_tuples, node->get_total_time,
                       ((double)node->get_total_time) / ((double)node->get_tuples));
        }

        return NULL;
    }
#else
        return NULL;
#endif

#ifdef __TBASE__
    if (enable_statistic && !node->need_to_scan_locally)
    {
        end = GetCurrentTimestamp();

        if (node->get_total_time == -1)
        {
            node->get_tuples++;
            node->get_total_time = 0;
        }
        else
        {
            node->get_tuples++;
            node->get_total_time += (end - begin);
        }
    }
#endif

    /*
     * Form the result tuple using ExecProject(), and return it.
     */
    econtext->ecxt_outertuple = slot;
    return ExecProject(node->ps.ps_ProjInfo);
}

/* ----------------------------------------------------------------
 *        ExecEndGather
 *
 *        frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndGather(GatherState *node)
{
    ExecEndNode(outerPlanState(node));    /* let children clean up first */
    ExecShutdownGather(node);
    ExecFreeExprContext(&node->ps);
    ExecClearTuple(node->ps.ps_ResultTupleSlot);
}

/*
 * Read the next tuple.  We might fetch a tuple from one of the tuple queues
 * using gather_readnext, or if no tuple queue contains a tuple and the
 * single_copy flag is not set, we might generate one locally instead.
 */
static TupleTableSlot *
gather_getnext(GatherState *gatherstate)
{
    PlanState  *outerPlan = outerPlanState(gatherstate);
    TupleTableSlot *outerTupleSlot;
    TupleTableSlot *fslot = gatherstate->funnel_slot;
    MemoryContext tupleContext = gatherstate->ps.ps_ExprContext->ecxt_per_tuple_memory;
    HeapTuple    tup;

    while (gatherstate->reader != NULL || gatherstate->need_to_scan_locally)
    {
        CHECK_FOR_INTERRUPTS();

        if (gatherstate->reader != NULL)
        {
            MemoryContext oldContext;

            /* Run TupleQueueReaders in per-tuple context */
            oldContext = MemoryContextSwitchTo(tupleContext);
            tup = gather_readnext(gatherstate);
            MemoryContextSwitchTo(oldContext);

            if (HeapTupleIsValid(tup))
            {
                ExecStoreTuple(tup, /* tuple to store */
                               fslot,    /* slot in which to store the tuple */
                               InvalidBuffer,    /* buffer associated with this
                                                 * tuple */
                               false);    /* slot should not pfree tuple */
                return fslot;
            }
        }

        if (gatherstate->need_to_scan_locally)
        {
            outerTupleSlot = ExecProcNode(outerPlan);

            if (!TupIsNull(outerTupleSlot))
                return outerTupleSlot;

            gatherstate->need_to_scan_locally = false;
        }
    }

    return ExecClearTuple(fslot);
}

/*
 * Attempt to read a tuple from one of our parallel workers.
 */
static HeapTuple
gather_readnext(GatherState *gatherstate)
{// #lizard forgives
    int            nvisited = 0;

    for (;;)
    {
        TupleQueueReader *reader;
        HeapTuple    tup;
        bool        readerdone;

        /* Check for async events, particularly messages from workers. */
        CHECK_FOR_INTERRUPTS();

        /* Attempt to read a tuple, but don't block if none is available. */
        Assert(gatherstate->nextreader < gatherstate->nreaders);
        reader = gatherstate->reader[gatherstate->nextreader];
        tup = TupleQueueReaderNext(reader, true, &readerdone);

        /*
         * If this reader is done, remove it.  If all readers are done, clean
         * up remaining worker state.
         */
        if (readerdone)
        {
            Assert(!tup);
            DestroyTupleQueueReader(reader);
            --gatherstate->nreaders;
            if (gatherstate->nreaders == 0)
            {
                ExecShutdownGatherWorkers(gatherstate);
                return NULL;
            }
            memmove(&gatherstate->reader[gatherstate->nextreader],
                    &gatherstate->reader[gatherstate->nextreader + 1],
                    sizeof(TupleQueueReader *)
                    * (gatherstate->nreaders - gatherstate->nextreader));
            if (gatherstate->nextreader >= gatherstate->nreaders)
                gatherstate->nextreader = 0;
            continue;
        }

        /* If we got a tuple, return it. */
        if (tup)
            return tup;

        /*
         * Advance nextreader pointer in round-robin fashion.  Note that we
         * only reach this code if we weren't able to get a tuple from the
         * current worker.  We used to advance the nextreader pointer after
         * every tuple, but it turns out to be much more efficient to keep
         * reading from the same queue until that would require blocking.
         */
        gatherstate->nextreader++;
        if (gatherstate->nextreader >= gatherstate->nreaders)
            gatherstate->nextreader = 0;

        /* Have we visited every (surviving) TupleQueueReader? */
        nvisited++;
        if (nvisited >= gatherstate->nreaders)
        {
            /*
             * If (still) running plan locally, return NULL so caller can
             * generate another tuple from the local copy of the plan.
             */
            if (gatherstate->need_to_scan_locally)
                return NULL;

            /* Nothing to do except wait for developments. */
            WaitLatch(MyLatch, WL_LATCH_SET, 0, WAIT_EVENT_EXECUTE_GATHER);
            ResetLatch(MyLatch);
            nvisited = 0;
        }
    }
}

/* ----------------------------------------------------------------
 *        ExecShutdownGatherWorkers
 *
 *        Destroy the parallel workers.  Collect all the stats after
 *        workers are stopped, else some work done by workers won't be
 *        accounted.
 * ----------------------------------------------------------------
 */
static void
ExecShutdownGatherWorkers(GatherState *node)
{
    /* Shut down tuple queue readers before shutting down workers. */
    if (node->reader != NULL)
    {
        int            i;

        for (i = 0; i < node->nreaders; ++i)
            DestroyTupleQueueReader(node->reader[i]);

        pfree(node->reader);
        node->reader = NULL;
    }

    /* Now shut down the workers. */
    if (node->pei != NULL)
        ExecParallelFinish(node->pei);
}

/* ----------------------------------------------------------------
 *        ExecShutdownGather
 *
 *        Destroy the setup for parallel workers including parallel context.
 *        Collect all the stats after workers are stopped, else some work
 *        done by workers won't be accounted.
 * ----------------------------------------------------------------
 */
void
ExecShutdownGather(GatherState *node)
{
    ExecShutdownGatherWorkers(node);

    /* Now destroy the parallel context. */
    if (node->pei != NULL)
    {
        ExecParallelCleanup(node->pei);
        node->pei = NULL;
    }
}

/* ----------------------------------------------------------------
 *                        Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *        ExecReScanGather
 *
 *		Prepare to re-scan the result of a Gather.
 * ----------------------------------------------------------------
 */
void
ExecReScanGather(GatherState *node)
{
    /*
     * Re-initialize the parallel workers to perform rescan of relation. We
     * want to gracefully shutdown all the workers so that they should be able
     * to propagate any error or other information to master backend before
     * dying.  Parallel context will be reused for rescan.
     */
#if 0 /* pg latest code disable for now */
	Gather	   *gather = (Gather *) node->ps.plan;
	PlanState  *outerPlan = outerPlanState(node);
#endif
	/* Make sure any existing workers are gracefully shut down */
    ExecShutdownGatherWorkers(node);

	/* Mark node so that shared state will be rebuilt at next call */
    node->initialized = false;

    if (node->pei)
		ExecParallelReinitialize(&node->ps, node->pei);

    ExecReScan(node->ps.lefttree);
#if 0
	=======
	/*
	 * Set child node's chgParam to tell it that the next scan might deliver a
	 * different set of rows within the leader process.  (The overall rowset
	 * shouldn't change, but the leader process's subset might; hence nodes
	 * between here and the parallel table scan node mustn't optimize on the
	 * assumption of an unchanging rowset.)
	 */
	if (gather->rescan_param >= 0)
		outerPlan->chgParam = bms_add_member(outerPlan->chgParam,
											 gather->rescan_param);

	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  Note: because this does nothing if we have a
	 * rescan_param, it's currently guaranteed that parallel-aware child nodes
	 * will not see a ReScan call until after they get a ReInitializeDSM call.
	 * That ordering might not be something to rely on, though.  A good rule
	 * of thumb is that ReInitializeDSM should reset only shared state, ReScan
	 * should reset only local state, and anything that depends on both of
	 * those steps being finished must wait until the first ExecProcNode call.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
>>>>>>> 41b0dd987d... Separate reinitialization of shared parallel-scan state from ExecReScan.
#endif
}
#ifdef __TBASE__
void
ExecFinishGather(PlanState *pstate)
{
    TupleTableSlot *slot = NULL;
    GatherState *node = castNode(GatherState, pstate);

    (*node->pei->executor_done) = true;

    if (g_DataPumpDebug)
    {
        elog(LOG, "ExecFinishGather: pid %d inform worker to finish current work", MyProcPid);
    }

    do
    {
        /* read all data from workers */
        slot = ExecGather(pstate);
    } while(!TupIsNull(slot));

    if (g_DataPumpDebug)
    {
        elog(LOG, "ExecFinishGather: pid %d get all data from worker", MyProcPid);
    }
}
#endif

