/*
 * Copyright 2011, Ben Langmead <langmea@cs.jhu.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "outq.h"
#include <stdio.h>

/**
 * Caller is telling us that they're about to write output record(s) for
 * the read with the given id.
 */
void OutputQueue::beginReadImpl(TReadId rdid, size_t threadId) {
	assert_lt(threadId, nthreads_);
	assert_leq(perThreadCounter_[threadId], perThreadBufSize_);
	perThreadStarted_[threadId]++;
	if(reorder_) {
		assert_geq(rdid, cur_);
		assert_eq(lines_.size(), finished_.size());
		assert_eq(lines_.size(), started_.size());
		if(rdid - cur_ >= lines_.size()) {
			// Make sure there's enough room in lines_, started_ and finished_
			size_t oldsz = lines_.size();
			lines_.resize(rdid - cur_ + 1);
			started_.resize(rdid - cur_ + 1);
			finished_.resize(rdid - cur_ + 1);
			for(size_t i = oldsz; i < lines_.size(); i++) {
				started_[i] = finished_[i] = false;
			}
		}
		started_[rdid - cur_] = true;
		finished_[rdid - cur_] = false;
	}
}

void OutputQueue::beginRead(TReadId rdid, size_t threadId) {
	if(reorder_ && threadSafe_) {
		ThreadSafe ts(mutex_global_);
		beginReadImpl(rdid, threadId);
	} else {
		beginReadImpl(rdid, threadId);
	}
}

/**
 * Writer is finished writing to 
 */
void OutputQueue::finishReadImpl(const BTString& rec, TReadId rdid, size_t threadId) {
	assert_lt(threadId, nthreads_);
	if(reorder_) {
		assert_geq(rdid, cur_);
		assert_eq(lines_.size(), finished_.size());
		assert_eq(lines_.size(), started_.size());
		assert_lt(rdid - cur_, lines_.size());
		assert(started_[rdid - cur_]);
		assert(!finished_[rdid - cur_]);
		lines_[rdid - cur_] = rec;
		perThreadFinished_[threadId]++;
		finished_[rdid - cur_] = true;
		flush(false, false); // don't force; already have lock
	} else {
		perThreadFinished_[threadId]++;
		if(perThreadCounter_[threadId] >= perThreadBufSize_) {
			assert_eq(perThreadCounter_[threadId], perThreadBufSize_);
			// The first case tries to minimize the fraction of the
			int outidx = threadId % nmulti_output_;
			{
				ThreadSafe ts(*mutexes_[outidx]);
				for(int i = 0; i < perThreadBufSize_; i++) {
					writeString(perThreadBuf_[threadId][i], outidx);
				}
			}
			perThreadFlushed_[threadId] += perThreadBufSize_;
			perThreadCounter_[threadId] = 0;
		}
		perThreadBuf_[threadId][perThreadCounter_[threadId]++] = rec;
	}
}

void OutputQueue::finishRead(const BTString& rec, TReadId rdid, size_t threadId) {
	assert_lt(threadId, nthreads_);
	if(threadSafe_ && reorder_) {
		ThreadSafe ts(mutex_global_);
		finishReadImpl(rec, rdid, threadId);
	} else {
		finishReadImpl(rec, rdid, threadId);
	}
}

/**
 * Write already-finished lines starting from cur_.
 */
void OutputQueue::flushImpl(bool force) {
	if(!reorder_) {
		for(size_t i = 0; i < nthreads_; i++) {
			for(int j = 0; j < perThreadCounter_[i]; j++) {
				writeString(perThreadBuf_[i][j], 0);
			}
			perThreadFlushed_[i] += perThreadCounter_[i];
			perThreadCounter_[i] = 0;
		}
		return;
	}
	size_t nflush = 0;
	while(nflush < finished_.size() && finished_[nflush]) {
		assert(started_[nflush]);
		nflush++;
	}
	// Waiting until we have several in a row to flush cuts down on copies
	// (but requires more buffering)
	if(force || nflush >= NFLUSH_THRESH) {
		for(size_t i = 0; i < nflush; i++) {
			assert(started_[i]);
			assert(finished_[i]);
			writeString(lines_[i], 0);
		}
		lines_.erase(0, nflush);
		started_.erase(0, nflush);
		finished_.erase(0, nflush);
		cur_ += nflush;
		// TODO: index [0]?
		perThreadFlushed_[0] += nflush;
	}
}

/**
 * Write already-finished lines starting from cur_.
 */
void OutputQueue::flush(bool force, bool getLock) {
	if(getLock && threadSafe_) {
		ThreadSafe ts(mutex_global_);
		flushImpl(force);
	} else {
		flushImpl(force);
	}
}

/**
 * Write a c++ string to the write buffer and, if necessary, flush.
 */
void OutputQueue::writeString(const BTString& s, int outidx) {
	const size_t slen = s.length();
	size_t nwritten = fwrite(s.toZBuf(), 1, slen, ofhs_[outidx]);
	if(nwritten != slen) {
		cerr << "Wrote only " << nwritten << " out of " << slen
		     << " bytes to output " << std::endl;
		perror("fwrite");
		throw 1;
	}
}

#ifdef OUTQ_MAIN

#include <iostream>

using namespace std;

int main(void) {
	cerr << "Case 1 (one thread) ... ";
	{
		OutFileBuf ofb;
		OutputQueue oq(ofb, false);
		assert_eq(0, oq.numFlushed());
		assert_eq(0, oq.numStarted());
		assert_eq(0, oq.numFinished());
		oq.beginRead(1);
		assert_eq(0, oq.numFlushed());
		assert_eq(1, oq.numStarted());
		assert_eq(0, oq.numFinished());
		oq.beginRead(3);
		assert_eq(0, oq.numFlushed());
		assert_eq(2, oq.numStarted());
		assert_eq(0, oq.numFinished());
		oq.beginRead(2);
		assert_eq(0, oq.numFlushed());
		assert_eq(3, oq.numStarted());
		assert_eq(0, oq.numFinished());
		oq.flush();
		assert_eq(0, oq.numFlushed());
		assert_eq(3, oq.numStarted());
		assert_eq(0, oq.numFinished());
		oq.beginRead(0);
		assert_eq(0, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(0, oq.numFinished());
		oq.flush();
		assert_eq(0, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(0, oq.numFinished());
		oq.finishRead(0);
		assert_eq(0, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(1, oq.numFinished());
		oq.flush();
		assert_eq(0, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(1, oq.numFinished());
		oq.flush(true);
		assert_eq(1, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(1, oq.numFinished());
		oq.finishRead(2);
		assert_eq(1, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(2, oq.numFinished());
		oq.flush(true);
		assert_eq(1, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(2, oq.numFinished());
		oq.finishRead(1);
		assert_eq(1, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(3, oq.numFinished());
		oq.flush(true);
		assert_eq(3, oq.numFlushed());
		assert_eq(4, oq.numStarted());
		assert_eq(3, oq.numFinished());
	}
	cerr << "PASSED" << endl;

	cerr << "Case 2 (one thread) ... ";
	{
		OutFileBuf ofb;
		OutputQueue oq(ofb, false);
		BTString& buf1 = oq.beginRead(0);
		BTString& buf2 = oq.beginRead(1);
		BTString& buf3 = oq.beginRead(2);
		BTString& buf4 = oq.beginRead(3);
		BTString& buf5 = oq.beginRead(4);
		assert_eq(5, oq.numStarted());
		assert_eq(0, oq.numFinished());
		buf1.install("A\n");
		buf2.install("B\n");
		buf3.install("C\n");
		buf4.install("D\n");
		buf5.install("E\n");
		oq.finishRead(4);
		oq.finishRead(1);
		oq.finishRead(0);
		oq.finishRead(2);
		oq.finishRead(3);
		oq.flush(true);
		assert_eq(5, oq.numFlushed());
		assert_eq(5, oq.numStarted());
		assert_eq(5, oq.numFinished());
		ofb.flush();
	}
	cerr << "PASSED" << endl;
	return 0;
}

#endif /*def ALN_SINK_MAIN*/
