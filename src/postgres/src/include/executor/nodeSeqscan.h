/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeSeqscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESEQSCAN_H
#define NODESEQSCAN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern SeqScanState *ExecInitSeqScan(SeqScan *node, EState *estate, int eflags);
extern void ExecEndSeqScan(SeqScanState *node);
extern void ExecReScanSeqScan(SeqScanState *node);

/* parallel scan support */
extern void ExecSeqScanEstimate(SeqScanState *node, ParallelContext *pcxt);
extern void ExecSeqScanInitializeDSM(SeqScanState *node, ParallelContext *pcxt);
extern void ExecSeqScanReInitializeDSM(SeqScanState *node, ParallelContext *pcxt);
extern void ExecSeqScanInitializeWorker(SeqScanState *node,
							ParallelWorkerContext *pwcxt);

/*
 * Update YugabyteDB specific run-time statistics
 */
extern void YbExecUpdateInstrumentSeqScan(SeqScanState *node,
										  Instrumentation *instr);

#endif							/* NODESEQSCAN_H */
