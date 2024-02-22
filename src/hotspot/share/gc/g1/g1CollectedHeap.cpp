/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/metadataOnStackMark.hpp"
#include "classfile/stringTable.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "compiler/oopMap.hpp"
#include "gc/g1/g1Allocator.inline.hpp"
#include "gc/g1/g1Arguments.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1BatchedTask.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectionSet.hpp"
#include "gc/g1/g1CollectionSetCandidates.hpp"
#include "gc/g1/g1CollectorState.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
#include "gc/g1/g1ConcurrentRefineThread.hpp"
#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1EvacStats.inline.hpp"
#include "gc/g1/g1FullCollector.hpp"
#include "gc/g1/g1GCCounters.hpp"
#include "gc/g1/g1GCParPhaseTimesTracker.hpp"
#include "gc/g1/g1GCPhaseTimes.hpp"
#include "gc/g1/g1GCPauseType.hpp"
#include "gc/g1/g1HeapSizingPolicy.hpp"
#include "gc/g1/g1HeapTransition.hpp"
#include "gc/g1/g1HeapVerifier.hpp"
#include "gc/g1/g1InitLogger.hpp"
#include "gc/g1/g1MemoryPool.hpp"
#include "gc/g1/g1MonotonicArenaFreeMemoryTask.hpp"
#include "gc/g1/g1OopClosures.inline.hpp"
#include "gc/g1/g1ParallelCleaning.hpp"
#include "gc/g1/g1ParScanThreadState.inline.hpp"
#include "gc/g1/g1PeriodicGCTask.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RedirtyCardsQueue.hpp"
#include "gc/g1/g1RegionToSpaceMapper.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1RootClosures.hpp"
#include "gc/g1/g1RootProcessor.hpp"
#include "gc/g1/g1SATBMarkQueueSet.hpp"
#include "gc/g1/g1ServiceThread.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/g1Trace.hpp"
#include "gc/g1/g1UncommitRegionTask.hpp"
#include "gc/g1/g1VMOperations.hpp"
#include "gc/g1/g1YoungCollector.hpp"
#include "gc/g1/g1YoungGCEvacFailureInjector.hpp"
#include "gc/g1/heapRegion.inline.hpp"
#include "gc/g1/heapRegionRemSet.inline.hpp"
#include "gc/g1/heapRegionSet.inline.hpp"
#include "gc/shared/concurrentGCBreakpoints.hpp"
#include "gc/shared/gcBehaviours.hpp"
#include "gc/shared/gcHeapSummary.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcLocker.inline.hpp"
#include "gc/shared/gcTimer.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/generationSpec.hpp"
#include "gc/shared/isGCActiveMark.hpp"
#include "gc/shared/locationPrinter.inline.hpp"
#include "gc/shared/oopStorageParState.hpp"
#include "gc/shared/preservedMarks.inline.hpp"
#include "gc/shared/referenceProcessor.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/taskqueue.inline.hpp"
#include "gc/shared/taskTerminator.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "gc/shared/workerPolicy.hpp"
#include "gc/shared/weakProcessor.inline.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "memory/heapInspection.hpp"
#include "memory/iterator.hpp"
#include "memory/metaspaceUtils.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/align.hpp"
#include "utilities/autoRestore.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/stack.inline.hpp"

size_t G1CollectedHeap::_humongous_object_threshold_in_words = 0;

// INVARIANTS/NOTES
//
// All allocation activity covered by the G1CollectedHeap interface is
// serialized by acquiring the HeapLock.  This happens in mem_allocate
// and allocate_new_tlab, which are the "entry" points to the
// allocation code from the rest of the JVM.  (Note that this does not
// apply to TLAB allocation, which is not part of this interface: it
// is done by clients of this interface.)

void G1RegionMappingChangedListener::reset_from_card_cache(uint start_idx, size_t num_regions) {
  HeapRegionRemSet::invalidate_from_card_cache(start_idx, num_regions);
}

void G1RegionMappingChangedListener::on_commit(uint start_idx, size_t num_regions, bool zero_filled) {
  // The from card cache is not the memory that is actually committed. So we cannot
  // take advantage of the zero_filled parameter.
  reset_from_card_cache(start_idx, num_regions);
}

void G1CollectedHeap::run_batch_task(G1BatchedTask* cl) {
  uint num_workers = MAX2(1u, MIN2(cl->num_workers_estimate(), workers()->active_workers()));
  cl->set_max_workers(num_workers);
  workers()->run_task(cl, num_workers);
}

uint G1CollectedHeap::get_chunks_per_region() {
  uint log_region_size = HeapRegion::LogOfHRGrainBytes;
  // Limit the expected input values to current known possible values of the
  // (log) region size. Adjust as necessary after testing if changing the permissible
  // values for region size.
  assert(log_region_size >= 20 && log_region_size <= 29,
         "expected value in [20,29], but got %u", log_region_size);
  return 1u << (log_region_size / 2 - 4);
}

HeapRegion* G1CollectedHeap::new_heap_region(uint hrs_index,
                                             MemRegion mr) {
  return new HeapRegion(hrs_index, bot(), mr, &_card_set_config);
}

/**
 * 分配一个新的堆区域。
 * 这段代码是G1垃圾收集器中用于分配新堆区域的私有方法。它首先尝试从空闲区域列表中分配一个区域，如果分配失败并且允许扩展堆，那么它将尝试扩展堆。这个方法执行了以下步骤：
 * 断言检查，确保当尝试分配一个巨大的区域时，它的大小不超过单个区域的大小。
 * 调用 _hrm.allocate_free_region 方法尝试从空闲区域列表中分配一个区域。
 * 如果分配失败并且 do_expand 参数为 true，表示允许扩展堆，那么将进一步执行以下步骤：
 * 断言检查，确保当前处于安全点。
 * 记录日志，表明尝试扩展堆以应对区域分配请求失败。
 * 断言检查，确保请求的扩展大小不超过单个区域的大小。
 * 调用 expand_single_region 方法尝试扩展堆。
 * 如果扩展成功，再次尝试从空闲区域列表中分配一个区域。
 *
 * @param word_size 区域的大小（以字为单位）
 * @param type 区域的类型
 * @param do_expand 如果没有可用的区域，是否尝试扩展堆
 * @param node_index 节点索引
 * @return 分配的区域，如果分配失败则为nullptr
 */
HeapRegion* G1CollectedHeap::new_region(size_t word_size,
                                        HeapRegionType type,
                                        bool do_expand,
                                        uint node_index) {
  // 断言：当尝试分配一个巨大的区域时，它的大小不应超过单个区域的大小
  assert(!is_humongous(word_size) || word_size <= HeapRegion::GrainWords,
         "the only time we use this to allocate a humongous region is "
         "when we are allocating a single humongous region");

  // 尝试从空闲区域列表中分配一个区域
  HeapRegion* res = _hrm.allocate_free_region(type, node_index);

  // 如果分配失败并且允许扩展堆，则尝试扩展堆
  if (res == nullptr && do_expand) {
    // 目前，只有尝试分配GC alloc区域时才会设置do_expand为true。
    // 所以，我们只应该在安全点时到达这里。
    assert(SafepointSynchronize::is_at_safepoint(), "invariant");

    // 记录日志：尝试扩展堆（区域分配请求失败）
    log_debug(gc, ergo, heap)("Attempt heap expansion (region allocation request failed). Allocation request: " SIZE_FORMAT "B",
                              word_size * HeapWordSize);

    // 断言：这种扩展永远不应超过一个区域的大小
    assert(word_size * HeapWordSize < HeapRegion::GrainBytes,
           "This kind of expansion should never be more than one region. Size: " SIZE_FORMAT,
           word_size * HeapWordSize);
    // 尝试扩展堆
    if (expand_single_region(node_index)) {
      // 扩展堆成功后，理论上空闲列表不应为空。
      // 无论哪种情况，allocate_free_region()都会检查nullptr。
      res = _hrm.allocate_free_region(type, node_index);
    }
  }
  return res;
}

/**
 * 设置大对象的元数据。
 * 这段代码设置巨大对象的元数据，并在G1垃圾收集器中分配巨大对象。巨大对象是指那些大小超过一个普通对象阈值（通常是一个区域的大小）的对象。代码执行了以下步骤：
 * 计算巨大对象的新顶部位置。
 * 确定是否有足够的空间来填充对象，如果没有，则计算无法填充的字节数。
 * 如果有空间，则用填充对象填充最后一个区域的未使用尾部。
 * 将第一个区域设置为 “starts humongous”，并更新其顶部字段。
 * 如果需要，更新Remembered Sets。
 * 对于系列中的其他区域，将它们设置为 “continues humongous”，并更新其顶部字段。
 * 如果最后一个区域无法放入填充对象，则将其顶部设置为巨大对象的末端。
 * 执行一系列的断言来确保分配的正确性。
 *
 * @param first_hr 第一个区域
 * @param num_regions 区域的数量
 * @param word_size 对象的大小（以字为单位）
 * @param update_remsets 是否更新Remembered Sets
 */
void G1CollectedHeap::set_humongous_metadata(HeapRegion* first_hr,
                                             uint num_regions,
                                             size_t word_size,
                                             bool update_remsets) {
  // 计算大对象的新顶部
  HeapWord* obj_top = first_hr->bottom() + word_size;
  // 所有用到的区域的总字大小
  size_t word_size_sum = num_regions * HeapRegion::GrainWords;
  assert(word_size <= word_size_sum, "sanity");

  // 计算无法用于填充对象的字节数
  size_t words_not_fillable = 0;

  // 用填充对象填充最后一个区域的未使用尾部，以提高使用率计算。

  // 可以用于填充对象的字节数
  size_t words_fillable = word_size_sum - word_size;

  if (words_fillable >= G1CollectedHeap::min_fill_size()) {
    G1CollectedHeap::fill_with_objects(obj_top, words_fillable);
  } else {
    // 有空间填充，但无法放入对象。
    words_not_fillable = words_fillable;
    words_fillable = 0;
  }

  // 将第一个区域设置为 "starts humongous"。这也会更新覆盖所有区域的BOT，
  // 反映出一个从第一个区域底部开始的单个对象。
  first_hr->hr_clear(false /* clear_space */);
  first_hr->set_starts_humongous(obj_top, words_fillable);

  if (update_remsets) {
    _policy->remset_tracker()->update_at_allocate(first_hr);
  }

  // 获取第一个和最后一个区域的索引
  uint first = first_hr->hrm_index();
  uint last = first + num_regions - 1;

  HeapRegion* hr = nullptr;
  for (uint i = first + 1; i <= last; ++i) {
    hr = region_at(i);
    hr->hr_clear(false /* clear_space */);
    hr->set_continues_humongous(first_hr);
    if (update_remsets) {
      _policy->remset_tracker()->update_at_allocate(hr);
    }
  }

  // 到目前为止，没有并发线程能够扫描这个系列中的任何区域。
  // 所有顶部字段仍然指向底部，因此 [bottom,top] 和 [card_start,card_end] 的交集将为空。
  // 在更新顶部字段之前，我们将执行一个storestore，以确保没有线程在对象头部的零初始化和BOT初始化之前看到顶部更新。
  OrderAccess::storestore();

  // 现在，我们将更新除了最后一个区域之外的所有 "continues humongous" 区域的顶部字段。
  for (uint i = first; i < last; ++i) {
    hr = region_at(i);
    hr->set_top(hr->end());
  }

  hr = region_at(last);
  // 如果我们无法放入填充对象，我们必须将顶部设置为大对象的末端，
  // 否则我们无法迭代堆，并且BOT将不完整。
  hr->set_top(hr->end() - words_not_fillable);

  assert(hr->bottom() < obj_top && obj_top <= hr->end(),
         "obj_top should be in last region");

  assert(words_not_fillable == 0 ||
         first_hr->bottom() + word_size_sum - words_not_fillable == hr->top(),
         "Miscalculation in humongous allocation");
}


/**
 * 分配并初始化一个大的对象所使用的区域。
 * 这段代码用于在G1垃圾收集器中分配并初始化一个大的对象所使用的区域。大的对象是指那些大小超过一个普通对象阈值（通常是一个区域的大小）的对象。代码执行了以下步骤：
 * 确认传入的第一个区域不为空，对象大小是大的，并且区域的总大小足够容纳对象。
 * 计算最后一个区域的索引。
 * 将新对象的头部放置在第一个区域的底部。
 * 清零新对象头部空间，以防止精化线程在对象初始化完成前错误地扫描对象。
 * 更新区域的元数据，标记这些区域为大的对象区域。
 * 计算已使用的大小，并更新堆的已使用内存大小。
 *
 * @param first_hr 第一个区域
 * @param num_regions 区域的数量
 * @param word_size 对象的大小（以字为单位）
 * @return 分配的对象的起始地址
 */
HeapWord* G1CollectedHeap::humongous_obj_allocate_initialize_regions(HeapRegion* first_hr,
                                                                    uint num_regions,
                                                                    size_t word_size) {
  // 断言：第一个区域不为空
  assert(first_hr != nullptr, "pre-condition");
  // 断言：对象大小应该是巨大的
  assert(is_humongous(word_size), "word_size should be humongous");
  // 断言：区域的总大小应该大于或等于对象的大小
  assert(num_regions * HeapRegion::GrainWords >= word_size, "pre-condition");

  // 计算最后一个区域的索引
  uint first = first_hr->hrm_index();
  uint last = first + num_regions - 1;

  // 我们需要初始化我们刚刚发现的区域。由于这可能与精化线程同时发生，
  // 因此需要小心地按照特定的顺序设置区域。

  // 传入的第一个区域将是 "starts humongous" 区域。新对象的头部将放在这个区域的底部。
  HeapWord* new_obj = first_hr->bottom();

  // 首先，我们需要将我们即将分配的空间的头部清零。当我们稍后更新顶部时，
  // 一些精化线程可能会尝试扫描该区域。通过清零头部，我们可以确保任何尝试扫描该区域的线程
  // 都会遇到零的klass字，并退出。
  //
  // 注意：使用 CollectedHeap::fill_with_object() 并使空间看起来像一个int数组是不正确的。
  // 执行分配的线程稍后会将对象头更新为可能是不同数组类型的klass，并且在这很短的时间内，
  // klass和长度字段将是不一致的。这可能导致精化线程计算对象大小不正确。
  Copy::fill_to_words(new_obj, oopDesc::header_size(), 0);

  // 接下来，更新区域的元数据。
  set_humongous_metadata(first_hr, num_regions, word_size, true);

  // 获取最后一个区域
  HeapRegion* last_hr = region_at(last);
  // 计算已使用的大小
  size_t used = byte_size(first_hr->bottom(), last_hr->top());

  // 增加已使用的内存大小
  increase_used(used);

  // 将所有区域添加到巨大的集合中，并更新打印信息
  for (uint i = first; i <= last; ++i) {
    HeapRegion *hr = region_at(i);
    _humongous_set.add(hr);
    _hr_printer.alloc(hr);
  }

  // 返回新分配的对象的起始地址
  return new_obj;
}

size_t G1CollectedHeap::humongous_obj_size_in_regions(size_t word_size) {
  assert(is_humongous(word_size), "Object of size " SIZE_FORMAT " must be humongous here", word_size);
  return align_up(word_size, HeapRegion::GrainWords) / HeapRegion::GrainWords;
}

// If could fit into free regions w/o expansion, try.
// Otherwise, if can expand, do so.
// Otherwise, if using ex regions might help, try with ex given back.
HeapWord* G1CollectedHeap::humongous_obj_allocate(size_t word_size) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  // 确保堆被锁定或处于安全点状态，这是进行垃圾回收操作时的必要条件。

  _verifier->verify_region_sets_optional();
  // 验证区域集，确保堆内存的管理数据结构是正确的。

  uint obj_regions = (uint) humongous_obj_size_in_regions(word_size);
  // 计算所需的大对象区域数量。

  // 尝试从空闲列表中分配一个大型对象。
  HeapRegion* humongous_start = _hrm.allocate_humongous(obj_regions);
  if (humongous_start == nullptr) {
    // 如果从空闲列表中没有找到足够的区域，则尝试通过扩展堆来分配。
    humongous_start = _hrm.expand_and_allocate_humongous(obj_regions);
    if (humongous_start != nullptr) {
      // 如果通过扩展堆找到了足够的区域，记录堆扩展信息。
      log_debug(gc, ergo, heap)("Heap expansion (humongous allocation request). Allocation request: " SIZE_FORMAT "B",
                                word_size * HeapWordSize);
      policy()->record_new_heap_size(num_regions());
    } else {
      // 如果即使扩展堆后仍然找不到足够的区域，可能需要触发一次整理垃圾的回收。
    }
  }

  // word_size 大于 100KB
  if (word_size > 100 * 1024) {
    // 获取当前Java进程的进程ID
    pid_t process_id = getpid();
    if (kill(process_id, SIGQUIT) == 0) {
        log_info(gc, ergo, heap)("word_size > 10000 ========== Java process %d exists.", process_id);
      } else {
        log_info(gc, ergo, heap)("word_size > 10000 ========== Java process %d does not exist or cannot be signaled.", process_id);
      }
    log_info(gc, ergo, heap)("word_size > 10000 ========== Current Java process ID: %d", process_id);
    log_info(gc, ergo, heap)("word_size > 10000 ========== Heap expansion (humongous allocation request). Allocation request: " SIZE_FORMAT "B",
                              word_size * HeapWordSize);
     Thread* thread = Thread::current();
      if (thread != nullptr) {
        const char* thread_name = thread->name();
        log_info(gc, ergo, heap)("word_size > 10000 ========== Current Java thread executing this method: %s", thread_name);
      }
  }

  HeapWord* result = nullptr;
  if (humongous_start != nullptr) {
    result = humongous_obj_allocate_initialize_regions(humongous_start, obj_regions, word_size);
    assert(result != nullptr, "it should always return a valid result");
    // 如果成功分配了大型对象，更新监控支持以反映新的堆大小。
    monitoring_support()->update_sizes();
  }

  _verifier->verify_region_sets_optional();
  // 再次验证区域集，确保分配操作没有破坏堆内存的管理数据结构。

  return result;
}


/**
 * 分配一个新的TLAB（线程局部分配缓冲区）。
 *
 * @param min_size 最小所需大小
 * @param requested_size 请求的大小
 * @param actual_size 实际分配的大小
 * @return TLAB的起始地址
 */
HeapWord* G1CollectedHeap::allocate_new_tlab(size_t min_size,
                                             size_t requested_size,
                                             size_t* actual_size) {
  // 断言：堆未被锁定且不在安全点上
  assert_heap_not_locked_and_not_at_safepoint();
  // 断言：不允许分配巨大的TLAB
  assert(!is_humongous(requested_size), "we do not allow humongous TLABs");

  // 尝试分配TLAB
  return attempt_allocation(min_size, requested_size, actual_size);
}

/**
 * 在堆上分配内存。
 *
 * @param word_size 要分配的内存大小（以字为单位）
 * @param gc_overhead_limit_was_exceeded GC开销限制是否被超过
 * @return 分配的内存的起始地址
 */
HeapWord* G1CollectedHeap::mem_allocate(size_t word_size,
                                        bool* gc_overhead_limit_was_exceeded) {
  // 断言：堆未被锁定且不在安全点上
  assert_heap_not_locked_and_not_at_safepoint();

  // 如果请求的大小是巨大的，则尝试分配巨大的内存块
  if (is_humongous(word_size)) {
    return attempt_allocation_humongous(word_size);
  }
  // 如果不是巨大的，则使用dummy变量来接收实际分配的大小（因为调用者不关心）
  size_t dummy = 0;
  // 尝试分配内存
  return attempt_allocation(word_size, word_size, &dummy);
}

// G1CollectedHeap类的成员函数，尝试分配内存，如果快速分配失败，将尝试慢速分配。
// 这个函数用于处理在G1垃圾收集器中分配内存的慢速路径。
HeapWord* G1CollectedHeap::attempt_allocation_slow(size_t word_size) {
  // ResourceMark用于在日志消息中检索线程名称，确保在函数执行期间资源被正确管理。
  ResourceMark rm;

  // 断言确保堆没有加锁，且不在安全点。
  assert_heap_not_locked_and_not_at_safepoint();
  // 断言确保这次调用不是为巨大对象分配内存。
  assert(!is_humongous(word_size), "attempt_allocation_slow() should not "
         "be called for humongous allocation requests");

  // result用于存储分配结果，初始值为nullptr。
  HeapWord* result = nullptr;

  // 开始一个无限循环，直到成功分配内存或成功安排了一次垃圾收集但未能分配内存为止。
  for (uint try_count = 1, gclocker_retry_count = 0; /* we'll return */; try_count += 1) {
    bool should_try_gc;
    uint gc_count_before;

    {
      // 加锁，防止并发操作。
      MutexLocker x(Heap_lock);

      // 重新尝试分配，因为可能在等待锁的时候有其他线程改变了内存区域。
      result = _allocator->attempt_allocation_locked(word_size);
      // 如果成功分配，直接返回。
      if (result != nullptr) {
        return result;
      }

      // 如果GCLocker激活并需要GC，且允许扩展年轻代，尝试强制分配。
      if (GCLocker::is_active_and_needs_gc() && policy()->can_expand_young_list()) {
        result = _allocator->attempt_allocation_force(word_size);
        if (result != nullptr) {
          return result;
        }
      }

      // 仅当GCLocker没有信号需要GC时，才尝试GC。
      should_try_gc = !GCLocker::needs_gc();
      // 在持有Heap_lock时读取GC计数。
      gc_count_before = total_collections();
    }
    // 释放锁后执行GC。
    if (should_try_gc) {
      bool succeeded;
      result = do_collection_pause(word_size, gc_count_before, &succeeded, GCCause::_g1_inc_collection_pause);
      // 如果成功分配，返回结果。
      if (result != nullptr) {
        assert(succeeded, "only way to get back a non-null result");
        log_trace(gc, alloc)("%s: Successfully scheduled collection returning " PTR_FORMAT,
                             Thread::current()->name(), p2i(result));
        return result;
      }

      // 如果成功安排了垃圾收集但未能分配内存，不再尝试分配，直接返回nullptr。
      if (succeeded) {
        log_trace(gc, alloc)("%s: Successfully scheduled collection failing to allocate "
                             SIZE_FORMAT " words", Thread::current()->name(), word_size);
        return nullptr;
      }
      log_trace(gc, alloc)("%s: Unsuccessfully scheduled collection allocating " SIZE_FORMAT " words",
                           Thread::current()->name(), word_size);
    } else {
      // 如果GCLocker重试次数过多，发出警告并返回nullptr。
      if (gclocker_retry_count > GCLockerRetryAllocationCount) {
        log_warning(gc, alloc)("%s: Retried waiting for GCLocker too often allocating "
                               SIZE_FORMAT " words", Thread::current()->name(), word_size);
        return nullptr;
      }
      log_trace(gc, alloc)("%s: Stall until clear", Thread::current()->name());
      // 等待GCLocker清除后再重试。
      GCLocker::stall_until_clear();
      gclocker_retry_count += 1;
    }

    // 尝试分配内存，如果成功，返回结果。
    size_t dummy = 0;
    result = _allocator->attempt_allocation(word_size, word_size, &dummy);
    if (result != nullptr) {
      return result;
    }

    // 如果重试次数达到警告阈值，记录警告信息。
    if ((QueuedAllocationWarningCount > 0) &&
        (try_count % QueuedAllocationWarningCount == 0)) {
      log_warning(gc, alloc)("%s:  Retried allocation %u times for " SIZE_FORMAT " words",
                             Thread::current()->name(), try_count, word_size);
    }
  }

  // 正常情况下不会执行到这里，如果执行到这里，说明出现了未预期的错误。
  ShouldNotReachHere();
  return nullptr;
}


bool G1CollectedHeap::check_archive_addresses(MemRegion range) {
  return _hrm.reserved().contains(range);
}

template <typename Func>
// G1CollectedHeap类的成员函数，用于遍历指定内存范围内所有的G1区域。
void G1CollectedHeap::iterate_regions_in_range(MemRegion range, const Func& func) {
  // 获取range开始地址所在的HeapRegion。
  HeapRegion* curr_region = _hrm.addr_to_region(range.start());
  // 获取range结束地址所在的HeapRegion。
  HeapRegion* end_region = _hrm.addr_to_region(range.last());

  // 循环遍历所有被range覆盖的HeapRegion。
  while (curr_region != nullptr) {
    // 检查当前区域是否是最后一个区域。
    bool is_last = curr_region == end_region;
    // 获取下一个区域，如果是最后一个区域则为nullptr。
    HeapRegion* next_region = is_last ? nullptr : _hrm.next_region_in_heap(curr_region);

    // 调用传入的函数对象func，对当前区域进行处理。
    func(curr_region, is_last);

    // 更新当前区域为下一个区域。
    curr_region = next_region;
  }
}


// G1CollectedHeap类的成员函数，用于在JVM初始化时分配存档区域。
bool G1CollectedHeap::alloc_archive_regions(MemRegion range) {
  // 断言确保在JVM初始化时调用此函数。
  assert(!is_init_completed(), "Expect to be called at JVM init time");
  // 加锁以防止并发操作。
  MutexLocker x(Heap_lock);

  // 获取保留的内存区域。
  MemRegion reserved = _hrm.reserved();

  // 暂时禁用堆页面的预触摸。这个接口用于映射已存档的堆数据，因此预触摸是浪费的。
  FlagSetting fs(AlwaysPreTouch, false);

  // 为指定的MemRegion范围分配相应的G1区域，并将其标记为旧区域。
  HeapWord* start_address = range.start();
  size_t word_size = range.word_size();
  HeapWord* last_address = range.last();
  size_t commits = 0;

  // 保证指定的内存区域在堆的范围内。
  guarantee(reserved.contains(start_address) && reserved.contains(last_address),
            "MemRegion outside of heap [" PTR_FORMAT ", " PTR_FORMAT "]",
            p2i(start_address), p2i(last_address));

  // 执行实际区域分配，如果失败则退出。
  // 然后记录我们分配了多少新的空间。
  if (!_hrm.allocate_containing_regions(range, &commits, workers())) {
    return false;
  }
  // 增加使用的内存量。
  increase_used(word_size * HeapWordSize);
  // 如果有新的提交，记录日志。
  if (commits != 0) {
    log_debug(gc, ergo, heap)("Attempt heap expansion (allocate archive regions). Total size: " SIZE_FORMAT "B",
                              HeapRegion::GrainWords * HeapWordSize * commits);
  }

  // 遍历range内的每个G1区域，将它们标记为旧区域，添加到旧区域集合中，并设置顶部。
  auto set_region_to_old = [&] (HeapRegion* r, bool is_last) {
    // 断言确保区域是空的。
    assert(r->is_empty(), "Region already in use (%u)", r->hrm_index());

    // 设置区域的顶部。
    HeapWord* top = is_last ? last_address + 1 : r->end();
    r->set_top(top);

    // 将区域标记为旧区域。
    r->set_old();
    _hr_printer.alloc(r);
    _old_set.add(r);
  };

  // 遍历区域并应用set_region_to_old函数。
  iterate_regions_in_range(range, set_region_to_old);
  return true;
}


void G1CollectedHeap::populate_archive_regions_bot_part(MemRegion range) {
  assert(!is_init_completed(), "Expect to be called at JVM init time");

  iterate_regions_in_range(range,
                           [&] (HeapRegion* r, bool is_last) {
                             r->update_bot();
                           });
}

/**
 * 在JVM初始化期间，释放指定范围内G1收集器区域的内存。
 *
 * @param range 要释放的内存区域
 */
void G1CollectedHeap::dealloc_archive_regions(MemRegion range) {
  // 断言：确保在JVM初始化时调用此方法
  assert(!is_init_completed(), "Expect to be called at JVM init time");
  MemRegion reserved = _hrm.reserved(); // 获取保留的内存区域
  size_t size_used = 0; // 已使用内存大小
  uint shrink_count = 0; // 收缩计数

  // 加锁，保护堆操作
  MutexLocker x(Heap_lock);
  HeapWord* start_address = range.start(); // 获取内存区域起始地址
  HeapWord* last_address = range.last(); // 获取内存区域结束地址

  // 断言：确保指定的内存区域在堆内存范围内
  assert(reserved.contains(start_address) && reserved.contains(last_address),
         "MemRegion outside of heap [" PTR_FORMAT ", " PTR_FORMAT "]",
         p2i(start_address), p2i(last_address));
  size_used += range.byte_size(); // 累加已使用内存大小

  // 释放、清空并取消提交包含CDS存档内容的区域
  auto dealloc_archive_region = [&] (HeapRegion* r, bool is_last) {
    // 保证：确保是旧区域
    guarantee(r->is_old(), "Expected old region at index %u", r->hrm_index());
    _old_set.remove(r); // 从旧区域集合中移除
    r->set_free(); // 设置为空闲
    r->set_top(r->bottom()); // 重置顶部
    _hrm.shrink_at(r->hrm_index(), 1); // 在HRM中收缩
    shrink_count++; // 增加收缩计数
  };

  // 遍历指定范围内的所有区域，并执行释放操作
  iterate_regions_in_range(range, dealloc_archive_region);

  // 如果有区域被收缩，尝试减少堆大小
  if (shrink_count != 0) {
    log_debug(gc, ergo, heap)("Attempt heap shrinking (CDS archive regions). Total size: " SIZE_FORMAT "B",
                              HeapRegion::GrainWords * HeapWordSize * shrink_count);
    // 显式取消提交
    uncommit_regions(shrink_count);
  }
  // 减少已使用的内存大小
  decrease_used(size_used);
}

inline HeapWord* G1CollectedHeap::attempt_allocation(size_t min_word_size,
                                                     size_t desired_word_size,
                                                     size_t* actual_word_size) {
  // 确保堆没有被锁定，并且不在安全点。
  assert_heap_not_locked_and_not_at_safepoint();
  // 断言分配请求不是巨大的，因为attempt_allocation()不应用于处理巨大的分配请求。
  assert(!is_humongous(desired_word_size), "attempt_allocation() should not "
         "be called for humongous allocation requests");

  // 尝试分配内存。首先尝试最小的内存大小，然后是期望的内存大小。
  HeapWord* result = _allocator->attempt_allocation(min_word_size, desired_word_size, actual_word_size);

  // 如果分配失败，返回null。
  if (result == nullptr) {
    *actual_word_size = desired_word_size;
    // 如果快速分配失败，则尝试慢速分配。
    result = attempt_allocation_slow(desired_word_size);
  }

  // 确保堆没有被锁定。
  assert_heap_not_locked();
  // 如果分配成功，确保实际大小已经被设置。
  if (result != nullptr) {
    assert(*actual_word_size != 0, "Actual size must have been set here");
    // 标记年轻代块为脏。这意味着这个块已经被分配并且可能需要被垃圾回收器标记。
    dirty_young_block(result, *actual_word_size);
  } else {
    // 如果分配失败，设置实际大小为0。
    *actual_word_size = 0;
  }

  // 返回分配的内存地址，如果有的话。
  return result;
}


HeapWord* G1CollectedHeap::attempt_allocation_humongous(size_t word_size) {
  ResourceMark rm;
  // 用于在日志消息中检索线程名称。
  // 此方法的结构与attempt_allocation_slow()有许多相似之处。
  // 这两个方法没有合并成一个的原因是，这样一个方法将需要许多"如果分配不是巨大的则执行此操作，
  // 否则执行那操作"的条件路径，这将使流程难以理解。事实上，这个代码的早期版本确实使用了一个统一的方法，
  // 但这使得跟踪其中的微妙错误变得困难。因此，保持这两个方法分开可以使每个方法都更容易理解。
  // 尽可能地保持这两个方法同步是很重要的。
  assert_heap_not_locked_and_not_at_safepoint();
  assert(is_humongous(word_size), "attempt_allocation_humongous() "
         "should only be called for humongous allocations");
  // 大型对象可以迅速耗尽堆，因此我们应该在每次大型对象分配时检查是否需要开始标记周期。
  // 我们会在实际分配之前进行检查。在实际分配之前进行检查的原因是，
  // 在执行GC时，我们避免了需要跟踪新分配的内存。
  if (policy()->need_to_start_conc_mark("concurrent humongous allocation",
                                        word_size)) {
    collect(GCCause::_g1_humongous_allocation);
  }
  // 我们将循环直到 a) 我们成功执行了分配 b) 我们成功安排了一个收集，
  // 该收集未能执行分配。 b) 是唯一返回null的情况。
  HeapWord* result = nullptr;
  for (uint try_count = 1, gclocker_retry_count = 0; /* 我们将返回 */; try_count += 1) {
    bool should_try_gc;
    uint gc_count_before;
    {
      MutexLocker x(Heap_lock);
      size_t size_in_regions = humongous_obj_size_in_regions(word_size);
      // 由于大型对象不在年轻代区域分配，我们首先尝试在堆中没有进行收集的情况下进行分配，
      // 希望堆中还有足够的空间。
      result = humongous_obj_allocate(word_size);
      if (result != nullptr) {
        policy()->old_gen_alloc_tracker()->
          add_allocated_humongous_bytes_since_last_gc(size_in_regions * HeapRegion::GrainBytes);
        return result;
      }
      // 只有当GCLocker不信号需要GC时才尝试GC。等待直到GCLocker引发的GC被执行，然后重试。
      // 这包括GCLocker不活动但尚未执行的情况。
      should_try_gc = !GCLocker::needs_gc();
      // 在释放Heap_lock之前读取GC计数。
      gc_count_before = total_collections();
    }

  if (should_try_gc) {
  // 如果应该尝试进行垃圾回收，那么我们尝试安排一次垃圾回收周期。
  bool succeeded;
  result = do_collection_pause(word_size, gc_count_before, &succeeded, GCCause::_g1_humongous_allocation);
  if (result != nullptr) {
    // 如果垃圾回收成功并且分配了内存，则返回分配的内存地址。
    assert(succeeded, "only way to get back a non-null result");
    log_trace(gc, alloc)("%s: Successfully scheduled collection returning " PTR_FORMAT,
                         Thread::current()->name(), p2i(result));
    size_t size_in_regions = humongous_obj_size_in_regions(word_size);
    policy()->old_gen_alloc_tracker()->
      record_collection_pause_humongous_allocation(size_in_regions * HeapRegion::GrainBytes);
    return result;
  }

  if (succeeded) {
    // 如果垃圾回收被成功安排，但尝试分配内存时失败了，那么没有必要继续尝试分配。
    // 我们将返回null，因为垃圾回收没有释放足够的空间来满足分配请求。
    log_trace(gc, alloc)("%s: Successfully scheduled collection failing to allocate "
                         SIZE_FORMAT " words", Thread::current()->name(), word_size);
    return nullptr;
  }
  log_trace(gc, alloc)("%s: Unsuccessfully scheduled collection allocating " SIZE_FORMAT "",
                       Thread::current()->name(), word_size);
  } else {
    // 如果无法安排垃圾回收，那么我们可能因为GCLocker的活动而阻塞。
    // 如果重试等待GCLocker次数过多，则记录警告并返回null。
    if (gclocker_retry_count > GCLockerRetryAllocationCount) {
      log_warning(gc, alloc)("%s: Retried waiting for GCLocker too often allocating "
                            SIZE_FORMAT " words", Thread::current()->name(), word_size);
      return nullptr;
    }
    log_trace(gc, alloc)("%s: Stall until clear", Thread::current()->name());
    // 阻塞直到GCLocker被清除，然后重试分配。
    GCLocker::stall_until_clear();
    gclocker_retry_count += 1;
  }

  // 如果由于另一个线程先前的垃圾回收或GCLocker的阻塞，我们未能成功安排垃圾回收，
  // 我们应该在下次循环中重试分配尝试。由于大型对象分配总是需要锁定，
  // 我们将在下一次循环中等待重试，与常规迭代情况不同。
  // 如果看起来我们在无限循环中，给出警告。
  if ((QueuedAllocationWarningCount > 0) &&
      (try_count % QueuedAllocationWarningCount == 0)) {
    log_warning(gc, alloc)("%s: Retried allocation %u times for " SIZE_FORMAT " words",
                          Thread::current()->name(), try_count, word_size);
  }
  }
  // 这段代码应该不会被执行，因为我们在循环中一直尝试直到成功为止。
  ShouldNotReachHere();
  return nullptr;

}

// G1CollectedHeap类的成员函数，用于在安全点尝试分配内存。
HeapWord* G1CollectedHeap::attempt_allocation_at_safepoint(size_t word_size,
                                                           bool expect_null_mutator_alloc_region) {
  // 断言确保当前线程在VM线程上且处于安全点。
  assert_at_safepoint_on_vm_thread();
  // 断言确保分配器没有分配区域，或者调用者期望分配区域不为空。
  assert(!_allocator->has_mutator_alloc_region() || !expect_null_mutator_alloc_region,
         "the current alloc region was unexpectedly found to be non-null");

  // 如果请求的大小不是巨大对象的大小，尝试在锁定状态下分配内存。
  if (!is_humongous(word_size)) {
    return _allocator->attempt_allocation_locked(word_size);
  } else {
    // 如果是巨大对象的大小，尝试分配巨大对象。
    HeapWord* result = humongous_obj_allocate(word_size);
    // 如果分配成功，并且根据策略需要启动并发标记，设置并发标记标志。
    if (result != nullptr && policy()->need_to_start_conc_mark("STW humongous allocation")) {
      collector_state()->set_initiate_conc_mark_if_possible(true);
    }
    return result;
  }

  // 正常情况下不会执行到这里，如果执行到这里，说明出现了未预期的错误。
  ShouldNotReachHere();
}

class PostCompactionPrinterClosure: public HeapRegionClosure {
private:
  G1HRPrinter* _hr_printer;
public:
  bool do_heap_region(HeapRegion* hr) {
    assert(!hr->is_young(), "not expecting to find young regions");
    _hr_printer->post_compaction(hr);
    return false;
  }

  PostCompactionPrinterClosure(G1HRPrinter* hr_printer)
    : _hr_printer(hr_printer) { }
};

void G1CollectedHeap::print_heap_after_full_collection() {
  // Post collection region logging.
  // We should do this after we potentially resize the heap so
  // that all the COMMIT / UNCOMMIT events are generated before
  // the compaction events.
  if (_hr_printer.is_active()) {
    PostCompactionPrinterClosure cl(hr_printer());
    heap_region_iterate(&cl);
  }
}

bool G1CollectedHeap::abort_concurrent_cycle() {
  // Disable discovery and empty the discovered lists
  // for the CM ref processor.
  _ref_processor_cm->disable_discovery();
  _ref_processor_cm->abandon_partial_discovery();
  _ref_processor_cm->verify_no_references_recorded();

  // Abandon current iterations of concurrent marking and concurrent
  // refinement, if any are in progress.
  return concurrent_mark()->concurrent_cycle_abort();
}

void G1CollectedHeap::prepare_heap_for_full_collection() {
  // Make sure we'll choose a new allocation region afterwards.
  _allocator->release_mutator_alloc_regions();
  _allocator->abandon_gc_alloc_regions();

  // We may have added regions to the current incremental collection
  // set between the last GC or pause and now. We need to clear the
  // incremental collection set and then start rebuilding it afresh
  // after this full GC.
  abandon_collection_set(collection_set());

  _hrm.remove_all_free_regions();
}

void G1CollectedHeap::verify_before_full_collection() {
  assert_used_and_recalculate_used_equal(this);
  if (!VerifyBeforeGC) {
    return;
  }
  if (!G1HeapVerifier::should_verify(G1HeapVerifier::G1VerifyFull)) {
    return;
  }
  _verifier->verify_region_sets_optional();
  _verifier->verify_before_gc();
  _verifier->verify_bitmap_clear(true /* above_tams_only */);
}

/**
 * 在完整收集后准备堆，以便于Mutator（应用程序线程）使用。
 */
void G1CollectedHeap::prepare_for_mutator_after_full_collection() {
  // 删除未加载类加载器的元空间，并清理loader_data图
  ClassLoaderDataGraph::purge(true /* at_safepoint */);
  // 仅在调试模式下验证元空间
  DEBUG_ONLY(MetaspaceUtils::verify();)

  // 准备堆进行正常收集。
  assert(num_free_regions() == 0, "we should not have added any free regions");
  // 重建区域集合，不包括空闲列表
  rebuild_region_sets(false /* free_list_only */);
  // 中止精化过程
  abort_refinement();
  // 根据需要调整堆大小
  resize_heap_if_necessary();
  // 根据需要取消提交区域
  uncommit_regions_if_necessary();

  // 为每个区域重建代码根列表
  rebuild_code_roots();

  // 开始新的收集集合
  start_new_collection_set();
  // 初始化Mutator的分配区域
  _allocator->init_mutator_alloc_regions();

  // 收集后的状态更新。
  MetaspaceGC::compute_new_size();
}


void G1CollectedHeap::abort_refinement() {
  // Discard all remembered set updates and reset refinement statistics.
  G1BarrierSet::dirty_card_queue_set().abandon_logs_and_stats();
  assert(G1BarrierSet::dirty_card_queue_set().num_cards() == 0,
         "DCQS should be empty");
  concurrent_refine()->get_and_reset_refinement_stats();
}

// G1CollectedHeap类的成员函数，用于在完整垃圾收集后验证堆的状态。
void G1CollectedHeap::verify_after_full_collection() {
  // 如果没有开启VerifyAfterGC选项，直接返回。
  if (!VerifyAfterGC) {
    return;
  }
  // 如果不需要进行G1VerifyFull验证，直接返回。
  if (!G1HeapVerifier::should_verify(G1HeapVerifier::G1VerifyFull)) {
    return;
  }
  // 验证堆的RegionManager。
  _hrm.verify_optional();
  // 验证Region集合。
  _verifier->verify_region_sets_optional();
  // 在垃圾收集后验证堆。
  _verifier->verify_after_gc();
  // 验证位图是否清除。
  _verifier->verify_bitmap_clear(false /* above_tams_only */);

  // 在这个点上，整个堆中不应该有被标记为年轻代的区域。
  assert(check_young_list_empty(), "young list should be empty at this point");

  // 注意：由于我们刚刚进行了完整的GC，并发标记不再活跃。
  // 因此我们不需要重新启用CM引用处理器的引用发现功能。
  // 这将在下一个标记周期开始时完成。
  // 我们也知道STW处理器不应该再发现任何新的引用。
  assert(!_ref_processor_stw->discovery_enabled(), "Postcondition");
  assert(!_ref_processor_cm->discovery_enabled(), "Postcondition");
  // 验证STW引用处理器没有记录任何引用。
  _ref_processor_stw->verify_no_references_recorded();
  // 验证CM引用处理器没有记录任何引用。
  _ref_processor_cm->verify_no_references_recorded();
}


// G1CollectedHeap类中执行全量垃圾收集的成员函数
bool G1CollectedHeap::do_full_collection(bool clear_all_soft_refs,
                                         bool do_maximal_compaction) {
  assert_at_safepoint_on_vm_thread(); // 断言当前处于安全点

  if (GCLocker::check_active_before_gc()) {
    // 如果GC活动，则全量GC无法完成
    return false;
  }

  const bool do_clear_all_soft_refs = clear_all_soft_refs ||
      soft_ref_policy()->should_clear_all_soft_refs();

  G1FullGCMark gc_mark; // 创建全量GC标记器
  GCTraceTime(Info, gc) tm("Pause Full", nullptr, gc_cause(), true); // 开始全量GC暂停计时
  G1FullCollector collector(this, do_clear_all_soft_refs, do_maximal_compaction, gc_mark.tracer());

  collector.prepare_collection(); // 准备全量GC
  collector.collect(); // 执行全量GC
  collector.complete_collection(); // 完成全量GC

  // 全量GC成功完成
  return true;
}

// G1CollectedHeap类中执行全量垃圾收集的成员函数，不关心返回值
void G1CollectedHeap::do_full_collection(bool clear_all_soft_refs) {
  // 目前，do_full_collection(bool) API没有通知调用者收集是否成功的方法
  // （例如，因为被GC locker锁定）。所以，目前我们会忽略返回值。

  do_full_collection(clear_all_soft_refs,
                     false /* do_maximal_compaction */);
}

// G1CollectedHeap类中尝试升级到全量垃圾收集的成员函数
bool G1CollectedHeap::upgrade_to_full_collection() {
  GCCauseSetter compaction(this, GCCause::_g1_compaction_pause);
  log_info(gc, ergo)("Attempting full compaction clearing soft references");
  bool success = do_full_collection(true  /* clear_all_soft_refs */,
                                    false /* do_maximal_compaction */);
  // do_full_collection只有在被GC locker锁定时才会失败，而这种情况在这里不会发生
  assert(success, "invariant");
  return success;
}

// G1CollectedHeap类中根据需要调整堆大小的成员函数
void G1CollectedHeap::resize_heap_if_necessary() {
  assert_at_safepoint_on_vm_thread(); // 断言当前处于安全点

  bool should_expand;
  size_t resize_amount = _heap_sizing_policy->full_collection_resize_amount(should_expand);

  if (resize_amount == 0) {
    return; // 如果调整大小为0，则不需要调整
  } else if (should_expand) {
    expand(resize_amount, _workers); // 如果需要扩展，则扩展堆
  } else {
    shrink(resize_amount); // 如果需要收缩，则收缩堆
  }
}


// G1CollectedHeap类中处理分配失败情况的成员函数
HeapWord* G1CollectedHeap::satisfy_failed_allocation_helper(size_t word_size,
                                                            bool do_gc,
                                                            bool maximal_compaction,
                                                            bool expect_null_mutator_alloc_region,
                                                            bool* gc_succeeded) {
  *gc_succeeded = true; // 初始化GC是否成功的标志为true

  // 首先尝试进行分配
  HeapWord* result =
    attempt_allocation_at_safepoint(word_size,
                                    expect_null_mutator_alloc_region);
  if (result != nullptr) {
    return result; // 如果成功，返回分配的地址
  }

  // 如果分配失败，尝试扩展堆并重新分配
  result = expand_and_allocate(word_size);
  if (result != nullptr) {
    return result;
  }

  // 如果扩展失败但需要进行GC，则进行全量GC
  if (do_gc) {
    GCCauseSetter compaction(this, GCCause::_g1_compaction_pause);
    // 如果需要最大压缩，则清除所有软引用并确保堆上没有死木
    if (maximal_compaction) {
      log_info(gc, ergo)("Attempting maximal full compaction clearing soft references");
    } else {
      log_info(gc, ergo)("Attempting full compaction");
    }
    *gc_succeeded = do_full_collection(maximal_compaction /* clear_all_soft_refs */,
                                       maximal_compaction /* do_maximal_compaction */);
  }

  return nullptr; // 返回null，表示无法满足分配需求
}

// G1CollectedHeap类中处理分配失败情况的成员函数
HeapWord* G1CollectedHeap::satisfy_failed_allocation(size_t word_size,
                                                     bool* succeeded) {
  assert_at_safepoint_on_vm_thread(); // 断言当前处于安全点

  // 尝试分配，如果失败则进行全量GC
  HeapWord* result =
    satisfy_failed_allocation_helper(word_size,
                                     true,  /* do_gc */
                                     false, /* maximum_collection */
                                     false, /* expect_null_mutator_alloc_region */
                                     succeeded);

  if (result != nullptr || !*succeeded) {
    return result; // 如果成功或GC失败，返回结果
  }

  // 尝试分配，如果失败则进行清除所有软引用的全量GC
  result = satisfy_failed_allocation_helper(word_size,
                                            true, /* do_gc */
                                            true, /* maximum_collection */
                                            true, /* expect_null_mutator_alloc_region */
                                            succeeded);

  if (result != nullptr || !*succeeded) {
    return result; // 如果成功或GC失败，返回结果
  }

  // 尝试分配，不进行GC
  result = satisfy_failed_allocation_helper(word_size,
                                            false, /* do_gc */
                                            false, /* maximum_collection */
                                            true,  /* expect_null_mutator_alloc_region */
                                            succeeded);

  if (result != nullptr) {
    return result; // 如果成功，返回结果
  }

  assert(!soft_ref_policy()->should_clear_all_soft_refs(),
         "Flag should have been handled and cleared prior to this point");

  // 还有什么其他方法？我们可能会稍后尝试同步的终结化。如果堆上可用的总空间
  // 足够进行分配，那么到目前为止尝试的更完整的整理阶段可能适合。
  return nullptr; // 返回null，表示无法满足分配需求
}

// Attempting to expand the heap sufficiently
// to support an allocation of the given "word_size".  If
// successful, perform the allocation and return the address of the
// allocated block, or else null.
// G1CollectedHeap类中尝试扩展堆并分配的成员函数
HeapWord* G1CollectedHeap::expand_and_allocate(size_t word_size) {
  assert_at_safepoint_on_vm_thread(); // 断言当前处于安全点

  _verifier->verify_region_sets_optional(); // 验证区域集

  size_t expand_bytes = MAX2(word_size * HeapWordSize, MinHeapDeltaBytes); // 计算扩展大小
  log_debug(gc, ergo, heap)("Attempt heap expansion (allocation request failed). Allocation request: " SIZE_FORMAT "B",
                            word_size * HeapWordSize);

  if (expand(expand_bytes, _workers)) { // 尝试扩展堆
    _hrm.verify_optional(); // 验证区域管理器
    _verifier->verify_region_sets_optional(); // 验证区域集
    return attempt_allocation_at_safepoint(word_size, // 尝试在安全点进行分配
                                           false /* expect_null_mutator_alloc_region */);
  }
  return nullptr; // 如果扩展失败，返回null
}

// G1CollectedHeap类中用于扩展堆的成员函数
bool G1CollectedHeap::expand(size_t expand_bytes, WorkerThreads* pretouch_workers, double* expand_time_ms) {
  size_t aligned_expand_bytes = ReservedSpace::page_align_size_up(expand_bytes); // 对齐扩展大小
  aligned_expand_bytes = align_up(aligned_expand_bytes,
                                       HeapRegion::GrainBytes); // 对齐到HeapRegion的粒度

  log_debug(gc, ergo, heap)("Expand the heap. requested expansion amount: " SIZE_FORMAT "B expansion amount: " SIZE_FORMAT "B",
                            expand_bytes, aligned_expand_bytes);

  if (is_maximal_no_gc()) { // 如果堆已经完全扩展，则不扩展
    log_debug(gc, ergo, heap)("Did not expand the heap (heap already fully expanded)");
    return false;
  }

  double expand_heap_start_time_sec = os::elapsedTime(); // 获取扩展开始时间
  uint regions_to_expand = (uint)(aligned_expand_bytes / HeapRegion::GrainBytes); // 计算需要扩展的区域数
  assert(regions_to_expand > 0, "Must expand by at least one region");

  uint expanded_by = _hrm.expand_by(regions_to_expand, pretouch_workers); // 扩展堆
  if (expand_time_ms != nullptr) {
    *expand_time_ms = (os::elapsedTime() - expand_heap_start_time_sec) * MILLIUNITS; // 计算扩展时间（毫秒）
  }

  assert(expanded_by > 0, "must have failed during commit."); // 确保堆成功扩展

  size_t actual_expand_bytes = expanded_by * HeapRegion::GrainBytes; // 实际扩展的字节数
  assert(actual_expand_bytes <= aligned_expand_bytes, "post-condition"); // 确保实际扩展不超过请求大小
  policy()->record_new_heap_size(num_regions()); // 记录新的堆大小

  return true; // 返回扩展是否成功的标志
}

bool G1CollectedHeap::expand_single_region(uint node_index) {
  uint expanded_by = _hrm.expand_on_preferred_node(node_index);

  if (expanded_by == 0) {
    assert(is_maximal_no_gc(), "Should be no regions left, available: %u", _hrm.available());
    log_debug(gc, ergo, heap)("Did not expand the heap (heap already fully expanded)");
    return false;
  }

  policy()->record_new_heap_size(num_regions());
  return true;
}

void G1CollectedHeap::shrink_helper(size_t shrink_bytes) {
  size_t aligned_shrink_bytes =
    ReservedSpace::page_align_size_down(shrink_bytes);
  aligned_shrink_bytes = align_down(aligned_shrink_bytes,
                                         HeapRegion::GrainBytes);
  uint num_regions_to_remove = (uint)(shrink_bytes / HeapRegion::GrainBytes);

  uint num_regions_removed = _hrm.shrink_by(num_regions_to_remove);
  size_t shrunk_bytes = num_regions_removed * HeapRegion::GrainBytes;

  log_debug(gc, ergo, heap)("Shrink the heap. requested shrinking amount: " SIZE_FORMAT "B aligned shrinking amount: " SIZE_FORMAT "B actual amount shrunk: " SIZE_FORMAT "B",
                            shrink_bytes, aligned_shrink_bytes, shrunk_bytes);
  if (num_regions_removed > 0) {
    log_debug(gc, heap)("Uncommittable regions after shrink: %u", num_regions_removed);
    policy()->record_new_heap_size(num_regions());
  } else {
    log_debug(gc, ergo, heap)("Did not shrink the heap (heap shrinking operation failed)");
  }
}

// G1CollectedHeap类中用于缩小堆的成员函数
void G1CollectedHeap::shrink(size_t shrink_bytes) {
  _verifier->verify_region_sets_optional(); // 验证区域集

  // 只有在完整GC结束或Remark阶段才能到达这里，这意味着我们不应该持有任何GC分配区域
  _allocator->abandon_gc_alloc_regions();

  // 而不是在这里拆毁/重建自由列表，我们可以在free_list上使用remove_all_pending方法
  // 来删除我们需要的那些。
  _hrm.remove_all_free_regions();
  shrink_helper(shrink_bytes); // 调用缩小帮助器
  rebuild_region_sets(true /* free_list_only */); // 重新构建区域集

  _hrm.verify_optional(); // 验证区域管理器
  _verifier->verify_region_sets_optional(); // 验证区域集
}

// OldRegionSetChecker类，用于检查旧区域集的一致性
class OldRegionSetChecker : public HeapRegionSetChecker {
public:
  void check_mt_safety() {
    // 主旧区域集的MT安全性协议：
    // (a) 如果我们处于安全点，对主旧区域集的操作应该由以下方式调用：
    // - 由VM线程调用（它将对其进行序列化），或者
    // - 由GC工作者在持有FreeList_lock时调用，如果我们处于为了清除暂停的安全点（这个锁在分配新的GC区域时会被持有），或者
    // - 由GC工作者在持有OldSets_lock时调用，如果我们处于为了清理暂停的安全点。
    // (b) 如果我们不在安全点，对主旧区域集的操作应该在持有Heap_lock时调用。

    if (SafepointSynchronize::is_at_safepoint()) {
      guarantee(Thread::current()->is_VM_thread() ||
                FreeList_lock->owned_by_self() || OldSets_lock->owned_by_self(),
                "master old set MT safety protocol at a safepoint");
    } else {
      guarantee(Heap_lock->owned_by_self(), "master old set MT safety protocol outside a safepoint");
    }
  }
  bool is_correct_type(HeapRegion* hr) { return hr->is_old(); } // 检查区域是否为旧区域
  const char* get_description() { return "Old Regions"; } // 返回区域描述
};

// HumongousRegionSetChecker类，用于检查巨大区域集的一致性
class HumongousRegionSetChecker : public HeapRegionSetChecker {
public:
  void check_mt_safety() {
    // 巨大区域集的MT安全性协议：
    // (a) 如果我们处于安全点，对主巨大区域集的操作应该由VM线程（它将对其进行序列化）或GC工作者在持有OldSets_lock时调用。
    // (b) 如果我们不在安全点，对主巨大区域集的操作应该在持有Heap_lock时调用。

    if (SafepointSynchronize::is_at_safepoint()) {
      guarantee(Thread::current()->is_VM_thread() ||
                OldSets_lock->owned_by_self(),
                "master humongous set MT safety protocol at a safepoint");
    } else {
      guarantee(Heap_lock->owned_by_self(),
                "master humongous set MT safety protocol outside a safepoint");
    }
  }
  bool is_correct_type(HeapRegion* hr) { return hr->is_humongous(); } // 检查区域是否为巨大区域
  const char* get_description() { return "Humongous Regions"; } // 返回区域描述
};

G1CollectedHeap::G1CollectedHeap() :
  CollectedHeap(),
  _service_thread(nullptr),
  _periodic_gc_task(nullptr),
  _free_arena_memory_task(nullptr),
  _workers(nullptr),
  _card_table(nullptr),
  _collection_pause_end(Ticks::now()),
  _soft_ref_policy(),
  _old_set("Old Region Set", new OldRegionSetChecker()),
  _humongous_set("Humongous Region Set", new HumongousRegionSetChecker()),
  _bot(nullptr),
  _listener(),
  _numa(G1NUMA::create()),
  _hrm(),
  _allocator(nullptr),
  _evac_failure_injector(),
  _verifier(nullptr),
  _summary_bytes_used(0),
  _bytes_used_during_gc(0),
  _survivor_evac_stats("Young", YoungPLABSize, PLABWeight),
  _old_evac_stats("Old", OldPLABSize, PLABWeight),
  _monitoring_support(nullptr),
  _num_humongous_objects(0),
  _num_humongous_reclaim_candidates(0),
  _hr_printer(),
  _collector_state(),
  _old_marking_cycles_started(0),
  _old_marking_cycles_completed(0),
  _eden(),
  _survivor(),
  _gc_timer_stw(new STWGCTimer()),
  _gc_tracer_stw(new G1NewTracer()),
  _policy(new G1Policy(_gc_timer_stw)),
  _heap_sizing_policy(nullptr),
  _collection_set(this, _policy),
  _rem_set(nullptr),
  _card_set_config(),
  _card_set_freelist_pool(G1CardSetConfiguration::num_mem_object_types()),
  _cm(nullptr),
  _cm_thread(nullptr),
  _cr(nullptr),
  _task_queues(nullptr),
  _ref_processor_stw(nullptr),
  _is_alive_closure_stw(this),
  _is_subject_to_discovery_stw(this),
  _ref_processor_cm(nullptr),
  _is_alive_closure_cm(this),
  _is_subject_to_discovery_cm(this),
  _region_attr() {

  _verifier = new G1HeapVerifier(this);

  _allocator = new G1Allocator(this);

  _heap_sizing_policy = G1HeapSizingPolicy::create(this, _policy->analytics());

  _humongous_object_threshold_in_words = humongous_threshold_for(HeapRegion::GrainWords);

  // Override the default _filler_array_max_size so that no humongous filler
  // objects are created.
  _filler_array_max_size = _humongous_object_threshold_in_words;

  // Override the default _stack_chunk_max_size so that no humongous stack chunks are created
  _stack_chunk_max_size = _humongous_object_threshold_in_words;

  uint n_queues = ParallelGCThreads;
  _task_queues = new G1ScannerTasksQueueSet(n_queues);

  for (uint i = 0; i < n_queues; i++) {
    G1ScannerTasksQueue* q = new G1ScannerTasksQueue();
    _task_queues->register_queue(i, q);
  }

  _gc_tracer_stw->initialize();

  guarantee(_task_queues != nullptr, "task_queues allocation failure.");
}

G1RegionToSpaceMapper* G1CollectedHeap::create_aux_memory_mapper(const char* description,
                                                                 size_t size,
                                                                 size_t translation_factor) {
  size_t preferred_page_size = os::page_size_for_region_unaligned(size, 1);
  // Allocate a new reserved space, preferring to use large pages.
  ReservedSpace rs(size, preferred_page_size);
  size_t page_size = rs.page_size();
  G1RegionToSpaceMapper* result  =
    G1RegionToSpaceMapper::create_mapper(rs,
                                         size,
                                         page_size,
                                         HeapRegion::GrainBytes,
                                         translation_factor,
                                         mtGC);

  os::trace_page_sizes_for_requested_size(description,
                                          size,
                                          page_size,
                                          preferred_page_size,
                                          rs.base(),
                                          rs.size());

  return result;
}

jint G1CollectedHeap::initialize_concurrent_refinement() {
  jint ecode = JNI_OK;
  _cr = G1ConcurrentRefine::create(policy(), &ecode);
  return ecode;
}

jint G1CollectedHeap::initialize_service_thread() {
  _service_thread = new G1ServiceThread();
  if (_service_thread->osthread() == nullptr) {
    vm_shutdown_during_initialization("Could not create G1ServiceThread");
    return JNI_ENOMEM;
  }
  return JNI_OK;
}

// G1CollectedHeap类中初始化堆的成员函数
jint G1CollectedHeap::initialize() {
  MutexLocker x(Heap_lock); // 使用互斥锁保护堆的初始化

  // 确保HeapWordSize等于wordSize
  guarantee(HeapWordSize == wordSize, "HeapWordSize必须等于wordSize");

  size_t init_byte_size = InitialHeapSize; // 初始堆大小
  size_t reserved_byte_size = G1Arguments::heap_reserved_size_bytes(); // 保留的堆大小

  // 确保大小正确对齐
  Universe::check_alignment(init_byte_size, HeapRegion::GrainBytes, "g1 heap");
  Universe::check_alignment(reserved_byte_size, HeapRegion::GrainBytes, "g1 heap");
  Universe::check_alignment(reserved_byte_size, HeapAlignment, "g1 heap");

  // 保留最大堆大小
  ReservedHeapSpace heap_rs = Universe::reserve_heap(reserved_byte_size, HeapAlignment); // 保留堆空间
  initialize_reserved_region(heap_rs); // 初始化保留区域

  G1CardTable* ct = new G1CardTable(heap_rs.region()); // 创建卡表
  G1BarrierSet* bs = new G1BarrierSet(ct); // 创建屏障集
  bs->initialize(); // 初始化屏障集
  assert(bs->is_a(BarrierSet::G1BarrierSet), "sanity"); // 断言屏障集类型正确
  BarrierSet::set_barrier_set(bs); // 设置屏障集
  _card_table = ct; // 保存卡表引用

  {
    G1SATBMarkQueueSet& satbqs = bs->satb_mark_queue_set(); // 获取SATB标记队列集
    satbqs.set_process_completed_buffers_threshold(G1SATBProcessCompletedThreshold); // 设置处理完成缓冲区的阈值
    satbqs.set_buffer_enqueue_threshold_percentage(G1SATBBufferEnqueueingThresholdPercent); // 设置缓冲区入队阈值百分比
  }

  // 创建空间映射器
  size_t page_size = heap_rs.page_size(); // 获取页面大小
  G1RegionToSpaceMapper* heap_storage =
    G1RegionToSpaceMapper::create_mapper(heap_rs, // 保留区域
                                         heap_rs.size(), // 保留区域大小
                                         page_size, // 页面大小
                                         HeapRegion::GrainBytes, // 区域大小
                                         1, // 堆空间类型
                                         mtJavaHeap); // 线程类型
  if(heap_storage == nullptr) {
    vm_shutdown_during_initialization("Could not initialize G1 heap"); // 如果创建失败，则关闭虚拟机
    return JNI_ERR; // 返回错误码
  }

  os::trace_page_sizes("Heap", // 打印堆的页面大小信息
                       MinHeapSize,
                       reserved_byte_size,
                       page_size,
                       heap_rs.base(),
                       heap_rs.size());
  heap_storage->set_mapping_changed_listener(&_listener); // 设置映射变化监听器

  // 创建BOT、卡表和位图的存储空间
  G1RegionToSpaceMapper* bot_storage =
    create_aux_memory_mapper("Block Offset Table", // BOT存储
                             G1BlockOffsetTable::compute_size(heap_rs.size() / HeapWordSize),
                             G1BlockOffsetTable::heap_map_factor());

  // 创建卡表的存储空间映射器
  G1RegionToSpaceMapper* cardtable_storage =
    create_aux_memory_mapper("Card Table",
                            G1CardTable::compute_size(heap_rs.size() / HeapWordSize),
                            G1CardTable::heap_map_factor());

  // 计算位图的大小
  size_t bitmap_size = G1CMBitMap::compute_size(heap_rs.size());
  // 创建位图的存储空间映射器
  G1RegionToSpaceMapper* bitmap_storage =
    create_aux_memory_mapper("Mark Bitmap", bitmap_size, G1CMBitMap::heap_map_factor());

  // 初始化区域到空间映射器（RegionToSpaceMapper），这是G1堆内存管理的核心组件
  _hrm.initialize(heap_storage, bitmap_storage, bot_storage, cardtable_storage);
  // 初始化卡表，这是一个用于跟踪堆中对象的卡表信息的数据结构
  _card_table->initialize(cardtable_storage);

  // 确保最大区域索引可以适应remembered set结构
  const uint max_region_idx = (1U << (sizeof(RegionIdx_t)*BitsPerByte-1)) - 1;
  guarantee((max_reserved_regions() - 1) <= max_region_idx, "too many regions");

  // G1FromCardCache使用值0作为“无效”的卡，因此堆的起始地址不能位于第一个卡内
  guarantee((uintptr_t)(heap_rs.base()) >= G1CardTable::card_size(), "Java heap must not start within the first card.");
  // 初始化从卡缓存，这是一个用于优化卡表访问的数据结构
  G1FromCardCache::initialize(max_reserved_regions());
  // 创建G1 rem set，这是一个用于跟踪remembered set的数据结构
  _rem_set = new G1RemSet(this, _card_table);
  _rem_set->initialize(max_reserved_regions());

  // 设置每个区域的最大卡片数
  size_t max_cards_per_region = ((size_t)1 << (sizeof(CardIdx_t)*BitsPerByte-1)) - 1;
  guarantee(HeapRegion::CardsPerRegion > 0, "make sure it's initialized");
  guarantee(HeapRegion::CardsPerRegion < max_cards_per_region,
            "too many cards per region");

  // 初始化HeapRegionRemSet，这是一个用于跟踪remembered set的数据结构
  HeapRegionRemSet::initialize(_reserved);

  // 设置FreeRegionList的长度为最大区域数加1，这是一个用于跟踪空闲区域的数据结构
  FreeRegionList::set_unrealistically_long_length(max_regions() + 1);

  // 创建并初始化BOT（块偏移表），这是一个用于跟踪堆中每个区域的块偏移的数据结构
  _bot = new G1BlockOffsetTable(reserved(), bot_storage);

  // 设置区域属性的粒度为HeapRegion::GrainBytes
  {
    size_t granularity = HeapRegion::GrainBytes;
    _region_attr.initialize(reserved(), granularity);
  }

  // 创建并初始化工作线程，这些线程用于执行垃圾收集工作
  _workers = new WorkerThreads("GC Thread", ParallelGCThreads);
  if (_workers == nullptr) {
    return JNI_ENOMEM;
  }
  _workers->initialize_workers();

  // 设置NUMA信息，这些信息用于优化堆内存的分配和回收
  _numa->set_region_info(HeapRegion::GrainBytes, page_size);

  // 创建G1ConcurrentMark数据结构和线程，这是G1垃圾收集器中的并发标记阶段的核心
  _cm = new G1ConcurrentMark(this, bitmap_storage);
  _cm_thread = _cm->cm_thread();

  // 现在扩展堆到初始大小
  if (!expand(init_byte_size, _workers)) {
    vm_shutdown_during_initialization("Failed to allocate initial heap.");
    return JNI_ENOMEM;
  }

  // G1CollectedHeap类中初始化堆的成员函数的续写

  // 执行策略初始化
  policy()->init(this, &_collection_set);

  jint ecode = initialize_concurrent_refinement();
  if (ecode != JNI_OK) {
    return ecode;
  }

  ecode = initialize_service_thread();
  if (ecode != JNI_OK) {
    return ecode;
  }

  // 创建并安排定期GC任务
  _periodic_gc_task = new G1PeriodicGCTask("Periodic GC Task");
  _service_thread->register_task(_periodic_gc_task);

  _free_arena_memory_task = new G1MonotonicArenaFreeMemoryTask("Card Set Free Memory Task");
  _service_thread->register_task(_free_arena_memory_task);

  // 创建一个虚拟的HeapRegion，用于G1AllocRegion类
  HeapRegion* dummy_region = _hrm.get_dummy_region();

  // 将这个虚拟区域标记为Eden区域，以避免非年轻代区域无法支持分配操作的情况
  dummy_region->set_eden();
  // 确保该区域已满
  dummy_region->set_top(dummy_region->end());
  G1AllocRegion::setup(this, dummy_region);

  // 初始化用于分配的区域
  _allocator->init_mutator_alloc_regions();

  // 创建堆监控和管理支持，确保堆中的值已经正确初始化
  _monitoring_support = new G1MonitoringSupport(this);

  // 初始化集合集合，跟踪可回收的区域集合
  _collection_set.initialize(max_reserved_regions());

  // 重置疏散失败注入器
  evac_failure_injector()->reset();

  // 打印堆初始化日志
  G1InitLogger::print();

  return JNI_OK;
}

bool G1CollectedHeap::concurrent_mark_is_terminating() const {
  return _cm_thread->should_terminate();
}

void G1CollectedHeap::stop() {
  // Stop all concurrent threads. We do this to make sure these threads
  // do not continue to execute and access resources (e.g. logging)
  // that are destroyed during shutdown.
  _cr->stop();
  _service_thread->stop();
  _cm_thread->stop();
}

void G1CollectedHeap::safepoint_synchronize_begin() {
  SuspendibleThreadSet::synchronize();
}

void G1CollectedHeap::safepoint_synchronize_end() {
  SuspendibleThreadSet::desynchronize();
}

void G1CollectedHeap::post_initialize() {
  CollectedHeap::post_initialize();
  ref_processing_init();
}

// G1CollectedHeap类中初始化引用处理器（ReferenceProcessor）的成员函数
void G1CollectedHeap::ref_processing_init() {
  // G1中的引用处理目前工作方式如下：
  //
  // * 存在两个引用处理器实例。一个用于在并发标记期间记录和处理发现的引用；
  //   另一个用于在停止-开始（Stop-The-World）暂停期间记录和处理引用（包括全量和增量）。
  // * 两个引用处理器都需要“覆盖”整个堆，因为收集集合中的区域可能散布在堆中。
  //
  // * 对于并发标记引用处理器：
  //   * 引用发现是在并发开始时启用的。
  //   * 引用发现被禁用，并且发现的引用在重标记期间被处理等。
  //   * 引用发现是多线程的（见下方）。
  //   * 引用处理可能或可能不支持多线程（取决于ParallelRefProcEnabled和ParallelGCThreads的值）。
  //   * 完整垃圾回收会禁用并发标记引用处理器和引用发现，并放弃其发现列表上的任何条目。
  //
  // * 对于停止-开始引用处理器：
  //   * 非多线程的引用发现是在全量垃圾回收开始时启用的。
  //   * 全量垃圾回收期间，处理和入队是非多线程的。
  //   * 在全量垃圾回收期间，引用是在标记之后处理的。
  //
  //   * 在增量疏散暂停的开始，发现（可能支持多线程）被启用。
  //   * 在停止-开始疏散暂停的末尾处理引用。
  //   * 对于这两种GC类型：
  //     * 发现是原子的，即不是并发的。
  //     * 引用发现不需要屏障。

  // 并发标记引用处理器
  _ref_processor_cm =
    new ReferenceProcessor(&_is_subject_to_discovery_cm,
                           ParallelGCThreads,                              // 多线程处理程度
                           // 我们在重标记期间与GC工作线程一起发现，所以两个线程数都必须考虑在内
                           MAX2(ParallelGCThreads, ConcGCThreads),         // 多线程发现程度
                           true,                                           // 引用发现是并发的
                           &_is_alive_closure_cm);                         // 是否存活闭包

  // 停止-开始引用处理器
  _ref_processor_stw =
    new ReferenceProcessor(&_is_subject_to_discovery_stw,
                           ParallelGCThreads,                    // 多线程处理程度
                           ParallelGCThreads,                    // 多线程发现程度
                           false,                                // 引用发现不是并发的
                           &_is_alive_closure_stw);              // 是否存活闭包
}

SoftRefPolicy* G1CollectedHeap::soft_ref_policy() {
  return &_soft_ref_policy;
}

size_t G1CollectedHeap::capacity() const {
  return _hrm.length() * HeapRegion::GrainBytes;
}

size_t G1CollectedHeap::unused_committed_regions_in_bytes() const {
  return _hrm.total_free_bytes();
}

// Computes the sum of the storage used by the various regions.
size_t G1CollectedHeap::used() const {
  size_t result = _summary_bytes_used + _allocator->used_in_alloc_regions();
  return result;
}

size_t G1CollectedHeap::used_unlocked() const {
  return _summary_bytes_used;
}

class SumUsedClosure: public HeapRegionClosure {
  size_t _used;
public:
  SumUsedClosure() : _used(0) {}
  bool do_heap_region(HeapRegion* r) {
    _used += r->used();
    return false;
  }
  size_t result() { return _used; }
};

size_t G1CollectedHeap::recalculate_used() const {
  SumUsedClosure blk;
  heap_region_iterate(&blk);
  return blk.result();
}

bool  G1CollectedHeap::is_user_requested_concurrent_full_gc(GCCause::Cause cause) {
  return GCCause::is_user_requested_gc(cause) && ExplicitGCInvokesConcurrent;
}

bool G1CollectedHeap::should_do_concurrent_full_gc(GCCause::Cause cause) {
  switch (cause) {
    case GCCause::_g1_humongous_allocation: return true;
    case GCCause::_g1_periodic_collection:  return G1PeriodicGCInvokesConcurrent;
    case GCCause::_wb_breakpoint:           return true;
    case GCCause::_codecache_GC_aggressive: return true;
    case GCCause::_codecache_GC_threshold:  return true;
    default:                                return is_user_requested_concurrent_full_gc(cause);
  }
}

void G1CollectedHeap::increment_old_marking_cycles_started() {
  assert(_old_marking_cycles_started == _old_marking_cycles_completed ||
         _old_marking_cycles_started == _old_marking_cycles_completed + 1,
         "Wrong marking cycle count (started: %d, completed: %d)",
         _old_marking_cycles_started, _old_marking_cycles_completed);

  _old_marking_cycles_started++;
}

// G1CollectedHeap类中增加完成的老年代标记周期的成员函数
void G1CollectedHeap::increment_old_marking_cycles_completed(bool concurrent,
                                                             bool whole_heap_examined) {
  MonitorLocker ml(G1OldGCCount_lock, Mutex::_no_safepoint_check_flag); // 使用MonitorLocker保护访问

  // 我们假设如果concurrent == true，则调用者是一个并发线程，它加入了可暂停线程集合。
  // 如果将来有廉价的方式来检查这一点，我们应该在这里添加一个assert。

  // 考虑到这个方法在完整GC或并发周期结束时被调用，并且这些可以嵌套（即，一个完整GC可以打断一个并发周期），
  // 完成的完整GC数量应该要么是1（在没有嵌套的情况下），要么是2（当一个完整GC打断一个并发周期时）
  // 落后于开始的完整GC数量。

  // 对于内部调用者（完整GC），这是成立的。
  assert(concurrent ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 1) ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 2),
         "for inner caller (Full GC): _old_marking_cycles_started = %u "
         "is inconsistent with _old_marking_cycles_completed = %u",
         _old_marking_cycles_started, _old_marking_cycles_completed);

  // 对于外部调用者（并发周期），这也是成立的。
  assert(!concurrent ||
         (_old_marking_cycles_started == _old_marking_cycles_completed + 1),
         "for outer caller (concurrent cycle): "
         "_old_marking_cycles_started = %u "
         "is inconsistent with _old_marking_cycles_completed = %u",
         _old_marking_cycles_started, _old_marking_cycles_completed);

  _old_marking_cycles_completed += 1; // 增加完成的标记周期计数
  if (whole_heap_examined) {
    // 记录我们已经完成了对所有活动对象的访问的时间戳
    record_whole_heap_examined_timestamp();
  }

  // 在CM线程中清除“in_progress”标志，以便在唤醒任何等待线程（特别是当ExplicitInvokesConcurrent设置时）时，
  // 如果一个等待线程请求另一个System.gc()，它不会错误地看到一个标记周期仍在进行中。
  if (concurrent) {
    _cm_thread->set_idle();
  }

  // 通知在System.gc()中等待（带有ExplicitGCInvokesConcurrent）的线程，完整GC完成，它们的等待结束了。
  ml.notify_all();
}

// collect()方法的辅助函数
static G1GCCounters collection_counters(G1CollectedHeap* g1h) {
  MutexLocker ml(Heap_lock); // 使用互斥锁保护访问
  return G1GCCounters(g1h); // 返回G1GCCounters实例
}

void G1CollectedHeap::collect(GCCause::Cause cause) {
  try_collect(cause, collection_counters(this)); // 尝试收集，使用G1GCCounters记录收集信息
}

// Return true if (x < y) with allowance for wraparound.
static bool gc_counter_less_than(uint x, uint y) {
  return (x - y) > (UINT_MAX/2);
}

// LOG_COLLECT_CONCURRENTLY(cause, msg, args...)
// Macro so msg printing is format-checked.
#define LOG_COLLECT_CONCURRENTLY(cause, ...)                            \
  do {                                                                  \
    LogTarget(Trace, gc) LOG_COLLECT_CONCURRENTLY_lt;                   \
    if (LOG_COLLECT_CONCURRENTLY_lt.is_enabled()) {                     \
      ResourceMark rm; /* For thread name. */                           \
      LogStream LOG_COLLECT_CONCURRENTLY_s(&LOG_COLLECT_CONCURRENTLY_lt); \
      LOG_COLLECT_CONCURRENTLY_s.print("%s: Try Collect Concurrently (%s): ", \
                                       Thread::current()->name(),       \
                                       GCCause::to_string(cause));      \
      LOG_COLLECT_CONCURRENTLY_s.print(__VA_ARGS__);                    \
    }                                                                   \
  } while (0)

#define LOG_COLLECT_CONCURRENTLY_COMPLETE(cause, result) \
  LOG_COLLECT_CONCURRENTLY(cause, "complete %s", BOOL_TO_STR(result))

bool G1CollectedHeap::try_collect_concurrently(GCCause::Cause cause,
                                               uint gc_counter,
                                               uint old_marking_started_before) {
  assert_heap_not_locked();
  assert(should_do_concurrent_full_gc(cause),
         "Non-concurrent cause %s", GCCause::to_string(cause));

  for (uint i = 1; true; ++i) {
    // Try to schedule concurrent start evacuation pause that will
    // start a concurrent cycle.
    LOG_COLLECT_CONCURRENTLY(cause, "attempt %u", i);
    VM_G1TryInitiateConcMark op(gc_counter, cause);
    VMThread::execute(&op);

    // Request is trivially finished.
    if (cause == GCCause::_g1_periodic_collection) {
      LOG_COLLECT_CONCURRENTLY_COMPLETE(cause, op.gc_succeeded());
      return op.gc_succeeded();
    }

    // If VMOp skipped initiating concurrent marking cycle because
    // we're terminating, then we're done.
    if (op.terminating()) {
      LOG_COLLECT_CONCURRENTLY(cause, "skipped: terminating");
      return false;
    }

    // Lock to get consistent set of values.
    uint old_marking_started_after;
    uint old_marking_completed_after;
    {
      MutexLocker ml(Heap_lock);
      // Update gc_counter for retrying VMOp if needed. Captured here to be
      // consistent with the values we use below for termination tests.  If
      // a retry is needed after a possible wait, and another collection
      // occurs in the meantime, it will cause our retry to be skipped and
      // we'll recheck for termination with updated conditions from that
      // more recent collection.  That's what we want, rather than having
      // our retry possibly perform an unnecessary collection.
      gc_counter = total_collections();
      old_marking_started_after = _old_marking_cycles_started;
      old_marking_completed_after = _old_marking_cycles_completed;
    }

    if (cause == GCCause::_wb_breakpoint) {
      if (op.gc_succeeded()) {
        LOG_COLLECT_CONCURRENTLY_COMPLETE(cause, true);
        return true;
      }
      // When _wb_breakpoint there can't be another cycle or deferred.
      assert(!op.cycle_already_in_progress(), "invariant");
      assert(!op.whitebox_attached(), "invariant");
      // Concurrent cycle attempt might have been cancelled by some other
      // collection, so retry.  Unlike other cases below, we want to retry
      // even if cancelled by a STW full collection, because we really want
      // to start a concurrent cycle.
      if (old_marking_started_before != old_marking_started_after) {
        LOG_COLLECT_CONCURRENTLY(cause, "ignoring STW full GC");
        old_marking_started_before = old_marking_started_after;
      }
    } else if (!GCCause::is_user_requested_gc(cause)) {
      // For an "automatic" (not user-requested) collection, we just need to
      // ensure that progress is made.
      //
      // Request is finished if any of
      // (1) the VMOp successfully performed a GC,
      // (2) a concurrent cycle was already in progress,
      // (3) whitebox is controlling concurrent cycles,
      // (4) a new cycle was started (by this thread or some other), or
      // (5) a Full GC was performed.
      // Cases (4) and (5) are detected together by a change to
      // _old_marking_cycles_started.
      //
      // Note that (1) does not imply (4).  If we're still in the mixed
      // phase of an earlier concurrent collection, the request to make the
      // collection a concurrent start won't be honored.  If we don't check for
      // both conditions we'll spin doing back-to-back collections.
      if (op.gc_succeeded() ||
          op.cycle_already_in_progress() ||
          op.whitebox_attached() ||
          (old_marking_started_before != old_marking_started_after)) {
        LOG_COLLECT_CONCURRENTLY_COMPLETE(cause, true);
        return true;
      }
    } else {                    // User-requested GC.
      // For a user-requested collection, we want to ensure that a complete
      // full collection has been performed before returning, but without
      // waiting for more than needed.

      // For user-requested GCs (unlike non-UR), a successful VMOp implies a
      // new cycle was started.  That's good, because it's not clear what we
      // should do otherwise.  Trying again just does back to back GCs.
      // Can't wait for someone else to start a cycle.  And returning fails
      // to meet the goal of ensuring a full collection was performed.
      assert(!op.gc_succeeded() ||
             (old_marking_started_before != old_marking_started_after),
             "invariant: succeeded %s, started before %u, started after %u",
             BOOL_TO_STR(op.gc_succeeded()),
             old_marking_started_before, old_marking_started_after);

      // Request is finished if a full collection (concurrent or stw)
      // was started after this request and has completed, e.g.
      // started_before < completed_after.
      if (gc_counter_less_than(old_marking_started_before,
                               old_marking_completed_after)) {
        LOG_COLLECT_CONCURRENTLY_COMPLETE(cause, true);
        return true;
      }

      if (old_marking_started_after != old_marking_completed_after) {
        // If there is an in-progress cycle (possibly started by us), then
        // wait for that cycle to complete, e.g.
        // while completed_now < started_after.
        LOG_COLLECT_CONCURRENTLY(cause, "wait");
        MonitorLocker ml(G1OldGCCount_lock);
        while (gc_counter_less_than(_old_marking_cycles_completed,
                                    old_marking_started_after)) {
          ml.wait();
        }
        // Request is finished if the collection we just waited for was
        // started after this request.
        if (old_marking_started_before != old_marking_started_after) {
          LOG_COLLECT_CONCURRENTLY(cause, "complete after wait");
          return true;
        }
      }

      // If VMOp was successful then it started a new cycle that the above
      // wait &etc should have recognized as finishing this request.  This
      // differs from a non-user-request, where gc_succeeded does not imply
      // a new cycle was started.
      assert(!op.gc_succeeded(), "invariant");

      if (op.cycle_already_in_progress()) {
        // If VMOp failed because a cycle was already in progress, it
        // is now complete.  But it didn't finish this user-requested
        // GC, so try again.
        LOG_COLLECT_CONCURRENTLY(cause, "retry after in-progress");
        continue;
      } else if (op.whitebox_attached()) {
        // If WhiteBox wants control, wait for notification of a state
        // change in the controller, then try again.  Don't wait for
        // release of control, since collections may complete while in
        // control.  Note: This won't recognize a STW full collection
        // while waiting; we can't wait on multiple monitors.
        LOG_COLLECT_CONCURRENTLY(cause, "whitebox control stall");
        MonitorLocker ml(ConcurrentGCBreakpoints::monitor());
        if (ConcurrentGCBreakpoints::is_controlled()) {
          ml.wait();
        }
        continue;
      }
    }

    // Collection failed and should be retried.
    assert(op.transient_failure(), "invariant");

    if (GCLocker::is_active_and_needs_gc()) {
      // If GCLocker is active, wait until clear before retrying.
      LOG_COLLECT_CONCURRENTLY(cause, "gc-locker stall");
      GCLocker::stall_until_clear();
    }

    LOG_COLLECT_CONCURRENTLY(cause, "retry");
  }
}

bool G1CollectedHeap::try_collect_fullgc(GCCause::Cause cause,
                                         const G1GCCounters& counters_before) {
  assert_heap_not_locked();

  while(true) {
    VM_G1CollectFull op(counters_before.total_collections(),
                        counters_before.total_full_collections(),
                        cause);
    VMThread::execute(&op);

    // Request is trivially finished.
    if (!GCCause::is_explicit_full_gc(cause) || op.gc_succeeded()) {
      return op.gc_succeeded();
    }

    {
      MutexLocker ml(Heap_lock);
      if (counters_before.total_full_collections() != total_full_collections()) {
        return true;
      }
    }

    if (GCLocker::is_active_and_needs_gc()) {
      // If GCLocker is active, wait until clear before retrying.
      GCLocker::stall_until_clear();
    }
  }
}

bool G1CollectedHeap::try_collect(GCCause::Cause cause,
                                  const G1GCCounters& counters_before) {
  if (should_do_concurrent_full_gc(cause)) {
    return try_collect_concurrently(cause,
                                    counters_before.total_collections(),
                                    counters_before.old_marking_cycles_started());
  } else if (GCLocker::should_discard(cause, counters_before.total_collections())) {
    // Indicate failure to be consistent with VMOp failure due to
    // another collection slipping in after our gc_count but before
    // our request is processed.
    return false;
  } else if (cause == GCCause::_gc_locker || cause == GCCause::_wb_young_gc
             DEBUG_ONLY(|| cause == GCCause::_scavenge_alot)) {

    // Schedule a standard evacuation pause. We're setting word_size
    // to 0 which means that we are not requesting a post-GC allocation.
    VM_G1CollectForAllocation op(0,     /* word_size */
                                 counters_before.total_collections(),
                                 cause);
    VMThread::execute(&op);
    return op.gc_succeeded();
  } else {
    // Schedule a Full GC.
    return try_collect_fullgc(cause, counters_before);
  }
}

void G1CollectedHeap::start_concurrent_gc_for_metadata_allocation(GCCause::Cause gc_cause) {
  GCCauseSetter x(this, gc_cause);

  // At this point we are supposed to start a concurrent cycle. We
  // will do so if one is not already in progress.
  bool should_start = policy()->force_concurrent_start_if_outside_cycle(gc_cause);
  if (should_start) {
    do_collection_pause_at_safepoint();
  }
}

bool G1CollectedHeap::is_in(const void* p) const {
  return is_in_reserved(p) && _hrm.is_available(addr_to_region(p));
}

// Iteration functions.

// Iterates an ObjectClosure over all objects within a HeapRegion.

class IterateObjectClosureRegionClosure: public HeapRegionClosure {
  ObjectClosure* _cl;
public:
  IterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool do_heap_region(HeapRegion* r) {
    if (!r->is_continues_humongous()) {
      r->object_iterate(_cl);
    }
    return false;
  }
};

void G1CollectedHeap::object_iterate(ObjectClosure* cl) {
  IterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

class G1ParallelObjectIterator : public ParallelObjectIteratorImpl {
private:
  G1CollectedHeap*  _heap;
  HeapRegionClaimer _claimer;

public:
  G1ParallelObjectIterator(uint thread_num) :
      _heap(G1CollectedHeap::heap()),
      _claimer(thread_num == 0 ? G1CollectedHeap::heap()->workers()->active_workers() : thread_num) {}

  virtual void object_iterate(ObjectClosure* cl, uint worker_id) {
    _heap->object_iterate_parallel(cl, worker_id, &_claimer);
  }
};

ParallelObjectIteratorImpl* G1CollectedHeap::parallel_object_iterator(uint thread_num) {
  return new G1ParallelObjectIterator(thread_num);
}

void G1CollectedHeap::object_iterate_parallel(ObjectClosure* cl, uint worker_id, HeapRegionClaimer* claimer) {
  IterateObjectClosureRegionClosure blk(cl);
  heap_region_par_iterate_from_worker_offset(&blk, claimer, worker_id);
}

void G1CollectedHeap::keep_alive(oop obj) {
  G1BarrierSet::enqueue_preloaded(obj);
}

void G1CollectedHeap::heap_region_iterate(HeapRegionClosure* cl) const {
  _hrm.iterate(cl);
}

void G1CollectedHeap::heap_region_iterate(HeapRegionIndexClosure* cl) const {
  _hrm.iterate(cl);
}

void G1CollectedHeap::heap_region_par_iterate_from_worker_offset(HeapRegionClosure* cl,
                                                                 HeapRegionClaimer *hrclaimer,
                                                                 uint worker_id) const {
  _hrm.par_iterate(cl, hrclaimer, hrclaimer->offset_for_worker(worker_id));
}

void G1CollectedHeap::heap_region_par_iterate_from_start(HeapRegionClosure* cl,
                                                         HeapRegionClaimer *hrclaimer) const {
  _hrm.par_iterate(cl, hrclaimer, 0);
}

void G1CollectedHeap::collection_set_iterate_all(HeapRegionClosure* cl) {
  _collection_set.iterate(cl);
}

void G1CollectedHeap::collection_set_par_iterate_all(HeapRegionClosure* cl,
                                                     HeapRegionClaimer* hr_claimer,
                                                     uint worker_id) {
  _collection_set.par_iterate(cl, hr_claimer, worker_id);
}

void G1CollectedHeap::collection_set_iterate_increment_from(HeapRegionClosure *cl,
                                                            HeapRegionClaimer* hr_claimer,
                                                            uint worker_id) {
  _collection_set.iterate_incremental_part_from(cl, hr_claimer, worker_id);
}

void G1CollectedHeap::par_iterate_regions_array(HeapRegionClosure* cl,
                                                HeapRegionClaimer* hr_claimer,
                                                const uint regions[],
                                                size_t length,
                                                uint worker_id) const {
  assert_at_safepoint();
  if (length == 0) {
    return;
  }
  uint total_workers = workers()->active_workers();

  size_t start_pos = (worker_id * length) / total_workers;
  size_t cur_pos = start_pos;

  do {
    uint region_idx = regions[cur_pos];
    if (hr_claimer == nullptr || hr_claimer->claim_region(region_idx)) {
      HeapRegion* r = region_at(region_idx);
      bool result = cl->do_heap_region(r);
      guarantee(!result, "Must not cancel iteration");
    }

    cur_pos++;
    if (cur_pos == length) {
      cur_pos = 0;
    }
  } while (cur_pos != start_pos);
}

HeapWord* G1CollectedHeap::block_start(const void* addr) const {
  HeapRegion* hr = heap_region_containing(addr);
  // The CollectedHeap API requires us to not fail for any given address within
  // the heap. HeapRegion::block_start() has been optimized to not accept addresses
  // outside of the allocated area.
  if (addr >= hr->top()) {
    return nullptr;
  }
  return hr->block_start(addr);
}

bool G1CollectedHeap::block_is_obj(const HeapWord* addr) const {
  HeapRegion* hr = heap_region_containing(addr);
  return hr->block_is_obj(addr, hr->parsable_bottom_acquire());
}

size_t G1CollectedHeap::tlab_capacity(Thread* ignored) const {
  return (_policy->young_list_target_length() - _survivor.length()) * HeapRegion::GrainBytes;
}

size_t G1CollectedHeap::tlab_used(Thread* ignored) const {
  return _eden.length() * HeapRegion::GrainBytes;
}

// For G1 TLABs should not contain humongous objects, so the maximum TLAB size
// must be equal to the humongous object limit.
size_t G1CollectedHeap::max_tlab_size() const {
  return align_down(_humongous_object_threshold_in_words, MinObjAlignment);
}

size_t G1CollectedHeap::unsafe_max_tlab_alloc(Thread* ignored) const {
  return _allocator->unsafe_max_tlab_alloc();
}

size_t G1CollectedHeap::max_capacity() const {
  return max_regions() * HeapRegion::GrainBytes;
}

void G1CollectedHeap::prepare_for_verify() {
  _verifier->prepare_for_verify();
}

void G1CollectedHeap::verify(VerifyOption vo) {
  _verifier->verify(vo);
}

bool G1CollectedHeap::supports_concurrent_gc_breakpoints() const {
  return true;
}

class PrintRegionClosure: public HeapRegionClosure {
  outputStream* _st;
public:
  PrintRegionClosure(outputStream* st) : _st(st) {}
  bool do_heap_region(HeapRegion* r) {
    r->print_on(_st);
    return false;
  }
};

bool G1CollectedHeap::is_obj_dead_cond(const oop obj,
                                       const HeapRegion* hr,
                                       const VerifyOption vo) const {
  switch (vo) {
    case VerifyOption::G1UseConcMarking: return is_obj_dead(obj, hr);
    case VerifyOption::G1UseFullMarking: return is_obj_dead_full(obj, hr);
    default:                             ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

bool G1CollectedHeap::is_obj_dead_cond(const oop obj,
                                       const VerifyOption vo) const {
  switch (vo) {
    case VerifyOption::G1UseConcMarking: return is_obj_dead(obj);
    case VerifyOption::G1UseFullMarking: return is_obj_dead_full(obj);
    default:                             ShouldNotReachHere();
  }
  return false; // keep some compilers happy
}

void G1CollectedHeap::pin_object(JavaThread* thread, oop obj) {
  GCLocker::lock_critical(thread);
}

void G1CollectedHeap::unpin_object(JavaThread* thread, oop obj) {
  GCLocker::unlock_critical(thread);
}

void G1CollectedHeap::print_heap_regions() const {
  LogTarget(Trace, gc, heap, region) lt;
  if (lt.is_enabled()) {
    LogStream ls(lt);
    print_regions_on(&ls);
  }
}

void G1CollectedHeap::print_on(outputStream* st) const {
  size_t heap_used = Heap_lock->owned_by_self() ? used() : used_unlocked();
  st->print(" %-20s", "garbage-first heap");
  st->print(" total " SIZE_FORMAT "K, used " SIZE_FORMAT "K",
            capacity()/K, heap_used/K);
  st->print(" [" PTR_FORMAT ", " PTR_FORMAT ")",
            p2i(_hrm.reserved().start()),
            p2i(_hrm.reserved().end()));
  st->cr();
  st->print("  region size " SIZE_FORMAT "K, ", HeapRegion::GrainBytes / K);
  uint young_regions = young_regions_count();
  st->print("%u young (" SIZE_FORMAT "K), ", young_regions,
            (size_t) young_regions * HeapRegion::GrainBytes / K);
  uint survivor_regions = survivor_regions_count();
  st->print("%u survivors (" SIZE_FORMAT "K)", survivor_regions,
            (size_t) survivor_regions * HeapRegion::GrainBytes / K);
  st->cr();
  if (_numa->is_enabled()) {
    uint num_nodes = _numa->num_active_nodes();
    st->print("  remaining free region(s) on each NUMA node: ");
    const int* node_ids = _numa->node_ids();
    for (uint node_index = 0; node_index < num_nodes; node_index++) {
      uint num_free_regions = _hrm.num_free_regions(node_index);
      st->print("%d=%u ", node_ids[node_index], num_free_regions);
    }
    st->cr();
  }
  MetaspaceUtils::print_on(st);
}

void G1CollectedHeap::print_regions_on(outputStream* st) const {
  st->print_cr("Heap Regions: E=young(eden), S=young(survivor), O=old, "
               "HS=humongous(starts), HC=humongous(continues), "
               "CS=collection set, F=free, "
               "TAMS=top-at-mark-start, "
               "PB=parsable bottom");
  PrintRegionClosure blk(st);
  heap_region_iterate(&blk);
}

void G1CollectedHeap::print_extended_on(outputStream* st) const {
  print_on(st);

  // Print the per-region information.
  st->cr();
  print_regions_on(st);
}

void G1CollectedHeap::print_on_error(outputStream* st) const {
  this->CollectedHeap::print_on_error(st);

  if (_cm != nullptr) {
    st->cr();
    _cm->print_on_error(st);
  }
}

void G1CollectedHeap::gc_threads_do(ThreadClosure* tc) const {
  workers()->threads_do(tc);
  tc->do_thread(_cm_thread);
  _cm->threads_do(tc);
  _cr->threads_do(tc);
  tc->do_thread(_service_thread);
}

void G1CollectedHeap::print_tracing_info() const {
  rem_set()->print_summary_info();
  concurrent_mark()->print_summary_info();
}

bool G1CollectedHeap::print_location(outputStream* st, void* addr) const {
  return BlockLocationPrinter<G1CollectedHeap>::print_location(st, addr);
}

G1HeapSummary G1CollectedHeap::create_g1_heap_summary() {

  size_t eden_used_bytes = _monitoring_support->eden_space_used();
  size_t survivor_used_bytes = _monitoring_support->survivor_space_used();
  size_t old_gen_used_bytes = _monitoring_support->old_gen_used();
  size_t heap_used = Heap_lock->owned_by_self() ? used() : used_unlocked();

  size_t eden_capacity_bytes =
    (policy()->young_list_target_length() * HeapRegion::GrainBytes) - survivor_used_bytes;

  VirtualSpaceSummary heap_summary = create_heap_space_summary();
  return G1HeapSummary(heap_summary, heap_used, eden_used_bytes, eden_capacity_bytes,
                       survivor_used_bytes, old_gen_used_bytes, num_regions());
}

G1EvacSummary G1CollectedHeap::create_g1_evac_summary(G1EvacStats* stats) {
  return G1EvacSummary(stats->allocated(), stats->wasted(), stats->undo_wasted(),
                       stats->unused(), stats->used(), stats->region_end_waste(),
                       stats->regions_filled(), stats->num_plab_filled(),
                       stats->direct_allocated(), stats->num_direct_allocated(),
                       stats->failure_used(), stats->failure_waste());
}

void G1CollectedHeap::trace_heap(GCWhen::Type when, const GCTracer* gc_tracer) {
  const G1HeapSummary& heap_summary = create_g1_heap_summary();
  gc_tracer->report_gc_heap_summary(when, heap_summary);

  const MetaspaceSummary& metaspace_summary = create_metaspace_summary();
  gc_tracer->report_metaspace_summary(when, metaspace_summary);
}

void G1CollectedHeap::gc_prologue(bool full) {
  assert(InlineCacheBuffer::is_empty(), "should have cleaned up ICBuffer");

  // Update common counters.
  increment_total_collections(full /* full gc */);
  if (full || collector_state()->in_concurrent_start_gc()) {
    increment_old_marking_cycles_started();
  }
}

void G1CollectedHeap::gc_epilogue(bool full) {
  // Update common counters.
  if (full) {
    // Update the number of full collections that have been completed.
    increment_old_marking_cycles_completed(false /* concurrent */, true /* liveness_completed */);
  }

#if COMPILER2_OR_JVMCI
  assert(DerivedPointerTable::is_empty(), "derived pointer present");
#endif

  // We have just completed a GC. Update the soft reference
  // policy with the new heap occupancy
  Universe::heap()->update_capacity_and_used_at_gc();

  _collection_pause_end = Ticks::now();

  _free_arena_memory_task->notify_new_stats(&_young_gen_card_set_stats,
                                            &_collection_set_candidates_card_set_stats);
}

uint G1CollectedHeap::uncommit_regions(uint region_limit) {
  return _hrm.uncommit_inactive_regions(region_limit);
}

bool G1CollectedHeap::has_uncommittable_regions() {
  return _hrm.has_inactive_regions();
}

void G1CollectedHeap::uncommit_regions_if_necessary() {
  if (has_uncommittable_regions()) {
    G1UncommitRegionTask::enqueue();
  }
}

void G1CollectedHeap::verify_numa_regions(const char* desc) {
  LogTarget(Trace, gc, heap, verify) lt;

  if (lt.is_enabled()) {
    LogStream ls(lt);
    // Iterate all heap regions to print matching between preferred numa id and actual numa id.
    G1NodeIndexCheckClosure cl(desc, _numa, &ls);
    heap_region_iterate(&cl);
  }
}

HeapWord* G1CollectedHeap::do_collection_pause(size_t word_size,
                                               uint gc_count_before,
                                               bool* succeeded,
                                               GCCause::Cause gc_cause) {
  assert_heap_not_locked_and_not_at_safepoint();
  VM_G1CollectForAllocation op(word_size, gc_count_before, gc_cause);
  VMThread::execute(&op);

  HeapWord* result = op.result();
  bool ret_succeeded = op.prologue_succeeded() && op.gc_succeeded();
  assert(result == nullptr || ret_succeeded,
         "the result should be null if the VM did not succeed");
  *succeeded = ret_succeeded;

  assert_heap_not_locked();
  return result;
}

void G1CollectedHeap::start_concurrent_cycle(bool concurrent_operation_is_full_mark) {
  assert(!_cm_thread->in_progress(), "Can not start concurrent operation while in progress");

  MutexLocker x(CGC_lock, Mutex::_no_safepoint_check_flag);
  if (concurrent_operation_is_full_mark) {
    _cm->post_concurrent_mark_start();
    _cm_thread->start_full_mark();
  } else {
    _cm->post_concurrent_undo_start();
    _cm_thread->start_undo_mark();
  }
  CGC_lock->notify();
}

bool G1CollectedHeap::is_potential_eager_reclaim_candidate(HeapRegion* r) const {
  // We don't nominate objects with many remembered set entries, on
  // the assumption that such objects are likely still live.
  HeapRegionRemSet* rem_set = r->rem_set();

  return rem_set->occupancy_less_or_equal_than(G1EagerReclaimRemSetThreshold);
}

#ifndef PRODUCT
void G1CollectedHeap::verify_region_attr_remset_is_tracked() {
  class VerifyRegionAttrRemSet : public HeapRegionClosure {
  public:
    virtual bool do_heap_region(HeapRegion* r) {
      G1CollectedHeap* g1h = G1CollectedHeap::heap();
      bool const remset_is_tracked = g1h->region_attr(r->bottom()).remset_is_tracked();
      assert(r->rem_set()->is_tracked() == remset_is_tracked,
             "Region %u remset tracking status (%s) different to region attribute (%s)",
             r->hrm_index(), BOOL_TO_STR(r->rem_set()->is_tracked()), BOOL_TO_STR(remset_is_tracked));
      return false;
    }
  } cl;
  heap_region_iterate(&cl);
}
#endif

void G1CollectedHeap::start_new_collection_set() {
  collection_set()->start_incremental_building();

  clear_region_attr();

  guarantee(_eden.length() == 0, "eden should have been cleared");
  policy()->transfer_survivors_to_cset(survivor());

  // We redo the verification but now wrt to the new CSet which
  // has just got initialized after the previous CSet was freed.
  _cm->verify_no_collection_set_oops();
}

G1HeapVerifier::G1VerifyType G1CollectedHeap::young_collection_verify_type() const {
  if (collector_state()->in_concurrent_start_gc()) {
    return G1HeapVerifier::G1VerifyConcurrentStart;
  } else if (collector_state()->in_young_only_phase()) {
    return G1HeapVerifier::G1VerifyYoungNormal;
  } else {
    return G1HeapVerifier::G1VerifyMixed;
  }
}

void G1CollectedHeap::verify_before_young_collection(G1HeapVerifier::G1VerifyType type) {
  if (!VerifyBeforeGC) {
    return;
  }
  if (!G1HeapVerifier::should_verify(type)) {
    return;
  }
  Ticks start = Ticks::now();
  _verifier->prepare_for_verify();
  _verifier->verify_region_sets_optional();
  _verifier->verify_dirty_young_regions();
  _verifier->verify_before_gc();
  verify_numa_regions("GC Start");
  phase_times()->record_verify_before_time_ms((Ticks::now() - start).seconds() * MILLIUNITS);
}

void G1CollectedHeap::verify_after_young_collection(G1HeapVerifier::G1VerifyType type) {
  if (!VerifyAfterGC) {
    return;
  }
  if (!G1HeapVerifier::should_verify(type)) {
    return;
  }
  Ticks start = Ticks::now();
  _verifier->verify_after_gc();
  verify_numa_regions("GC End");
  _verifier->verify_region_sets_optional();
  phase_times()->record_verify_after_time_ms((Ticks::now() - start).seconds() * MILLIUNITS);
}

void G1CollectedHeap::expand_heap_after_young_collection(){
  size_t expand_bytes = _heap_sizing_policy->young_collection_expansion_amount();
  if (expand_bytes > 0) {
    // No need for an ergo logging here,
    // expansion_amount() does this when it returns a value > 0.
    double expand_ms = 0.0;
    if (!expand(expand_bytes, _workers, &expand_ms)) {
      // We failed to expand the heap. Cannot do anything about it.
    }
    phase_times()->record_expand_heap_time(expand_ms);
  }
}

bool G1CollectedHeap::do_collection_pause_at_safepoint() {
  assert_at_safepoint_on_vm_thread();
  guarantee(!is_gc_active(), "collection is not reentrant");

  if (GCLocker::check_active_before_gc()) {
    return false;
  }

  do_collection_pause_at_safepoint_helper();
  return true;
}

G1HeapPrinterMark::G1HeapPrinterMark(G1CollectedHeap* g1h) : _g1h(g1h), _heap_transition(g1h) {
  // This summary needs to be printed before incrementing total collections.
  _g1h->rem_set()->print_periodic_summary_info("Before GC RS summary",
                                               _g1h->total_collections(),
                                               true /* show_thread_times */);
  _g1h->print_heap_before_gc();
  _g1h->print_heap_regions();
}

G1HeapPrinterMark::~G1HeapPrinterMark() {
  _g1h->policy()->print_age_table();
  _g1h->rem_set()->print_coarsen_stats();
  // We are at the end of the GC. Total collections has already been increased.
  _g1h->rem_set()->print_periodic_summary_info("After GC RS summary",
                                               _g1h->total_collections() - 1,
                                               false /* show_thread_times */);

  _heap_transition.print();
  _g1h->print_heap_regions();
  _g1h->print_heap_after_gc();
  // Print NUMA statistics.
  _g1h->numa()->print_statistics();
}

G1JFRTracerMark::G1JFRTracerMark(STWGCTimer* timer, GCTracer* tracer) :
  _timer(timer), _tracer(tracer) {

  _timer->register_gc_start();
  _tracer->report_gc_start(G1CollectedHeap::heap()->gc_cause(), _timer->gc_start());
  G1CollectedHeap::heap()->trace_heap_before_gc(_tracer);
}

G1JFRTracerMark::~G1JFRTracerMark() {
  G1CollectedHeap::heap()->trace_heap_after_gc(_tracer);
  _timer->register_gc_end();
  _tracer->report_gc_end(_timer->gc_end(), _timer->time_partitions());
}

void G1CollectedHeap::prepare_for_mutator_after_young_collection() {
  Ticks start = Ticks::now();

  _survivor_evac_stats.adjust_desired_plab_size();
  _old_evac_stats.adjust_desired_plab_size();

  // Start a new incremental collection set for the mutator phase.
  start_new_collection_set();
  _allocator->init_mutator_alloc_regions();

  phase_times()->record_prepare_for_mutator_time_ms((Ticks::now() - start).seconds() * 1000.0);
}

void G1CollectedHeap::retire_tlabs() {
  ensure_parsability(true);
}

void G1CollectedHeap::do_collection_pause_at_safepoint_helper() {
  ResourceMark rm;

  IsGCActiveMark active_gc_mark;
  GCIdMark gc_id_mark;
  SvcGCMarker sgcm(SvcGCMarker::MINOR);

  GCTraceCPUTime tcpu(_gc_tracer_stw);

  _bytes_used_during_gc = 0;

  policy()->decide_on_concurrent_start_pause();
  // Record whether this pause may need to trigger a concurrent operation. Later,
  // when we signal the G1ConcurrentMarkThread, the collector state has already
  // been reset for the next pause.
  bool should_start_concurrent_mark_operation = collector_state()->in_concurrent_start_gc();

  // Perform the collection.
  G1YoungCollector collector(gc_cause());
  collector.collect();

  // It should now be safe to tell the concurrent mark thread to start
  // without its logging output interfering with the logging output
  // that came from the pause.
  if (should_start_concurrent_mark_operation) {
    verifier()->verify_bitmap_clear(true /* above_tams_only */);
    // CAUTION: after the start_concurrent_cycle() call below, the concurrent marking
    // thread(s) could be running concurrently with us. Make sure that anything
    // after this point does not assume that we are the only GC thread running.
    // Note: of course, the actual marking work will not start until the safepoint
    // itself is released in SuspendibleThreadSet::desynchronize().
    start_concurrent_cycle(collector.concurrent_operation_is_full_mark());
    ConcurrentGCBreakpoints::notify_idle_to_active();
  }
}

void G1CollectedHeap::complete_cleaning(bool class_unloading_occurred) {
  uint num_workers = workers()->active_workers();
  G1ParallelCleaningTask unlink_task(num_workers, class_unloading_occurred);
  workers()->run_task(&unlink_task);
}

bool G1STWSubjectToDiscoveryClosure::do_object_b(oop obj) {
  assert(obj != nullptr, "must not be null");
  assert(_g1h->is_in_reserved(obj), "Trying to discover obj " PTR_FORMAT " not in heap", p2i(obj));
  // The areas the CM and STW ref processor manage must be disjoint. The is_in_cset() below
  // may falsely indicate that this is not the case here: however the collection set only
  // contains old regions when concurrent mark is not running.
  return _g1h->is_in_cset(obj) || _g1h->heap_region_containing(obj)->is_survivor();
}

void G1CollectedHeap::make_pending_list_reachable() {
  if (collector_state()->in_concurrent_start_gc()) {
    oop pll_head = Universe::reference_pending_list();
    if (pll_head != nullptr) {
      // Any valid worker id is fine here as we are in the VM thread and single-threaded.
      _cm->mark_in_bitmap(0 /* worker_id */, pll_head);
    }
  }
}

void G1CollectedHeap::set_humongous_stats(uint num_humongous_total, uint num_humongous_candidates) {
  _num_humongous_objects = num_humongous_total;
  _num_humongous_reclaim_candidates = num_humongous_candidates;
}

bool G1CollectedHeap::should_sample_collection_set_candidates() const {
  const G1CollectionSetCandidates* candidates = collection_set()->candidates();
  return !candidates->is_empty();
}

void G1CollectedHeap::set_collection_set_candidates_stats(G1MonotonicArenaMemoryStats& stats) {
  _collection_set_candidates_card_set_stats = stats;
}

void G1CollectedHeap::set_young_gen_card_set_stats(const G1MonotonicArenaMemoryStats& stats) {
  _young_gen_card_set_stats = stats;
}

void G1CollectedHeap::record_obj_copy_mem_stats() {
  policy()->old_gen_alloc_tracker()->
    add_allocated_bytes_since_last_gc(_old_evac_stats.allocated() * HeapWordSize);

  _gc_tracer_stw->report_evacuation_statistics(create_g1_evac_summary(&_survivor_evac_stats),
                                               create_g1_evac_summary(&_old_evac_stats));
}

void G1CollectedHeap::clear_bitmap_for_region(HeapRegion* hr) {
  concurrent_mark()->clear_bitmap_for_region(hr);
}

void G1CollectedHeap::free_region(HeapRegion* hr, FreeRegionList* free_list) {
  assert(!hr->is_free(), "the region should not be free");
  assert(!hr->is_empty(), "the region should not be empty");
  assert(_hrm.is_available(hr->hrm_index()), "region should be committed");

  // Reset region metadata to allow reuse.
  hr->hr_clear(true /* clear_space */);
  _policy->remset_tracker()->update_at_free(hr);

  if (free_list != nullptr) {
    free_list->add_ordered(hr);
  }
}

void G1CollectedHeap::free_humongous_region(HeapRegion* hr,
                                            FreeRegionList* free_list) {
  assert(hr->is_humongous(), "this is only for humongous regions");
  hr->clear_humongous();
  free_region(hr, free_list);
}

void G1CollectedHeap::remove_from_old_gen_sets(const uint old_regions_removed,
                                               const uint humongous_regions_removed) {
  if (old_regions_removed > 0 || humongous_regions_removed > 0) {
    MutexLocker x(OldSets_lock, Mutex::_no_safepoint_check_flag);
    _old_set.bulk_remove(old_regions_removed);
    _humongous_set.bulk_remove(humongous_regions_removed);
  }

}

void G1CollectedHeap::prepend_to_freelist(FreeRegionList* list) {
  assert(list != nullptr, "list can't be null");
  if (!list->is_empty()) {
    MutexLocker x(FreeList_lock, Mutex::_no_safepoint_check_flag);
    _hrm.insert_list_into_free_list(list);
  }
}

void G1CollectedHeap::decrement_summary_bytes(size_t bytes) {
  decrease_used(bytes);
}

void G1CollectedHeap::clear_eden() {
  _eden.clear();
}

void G1CollectedHeap::clear_collection_set() {
  collection_set()->clear();
}

void G1CollectedHeap::rebuild_free_region_list() {
  Ticks start = Ticks::now();
  _hrm.rebuild_free_list(workers());
  phase_times()->record_total_rebuild_freelist_time_ms((Ticks::now() - start).seconds() * 1000.0);
}

class G1AbandonCollectionSetClosure : public HeapRegionClosure {
public:
  virtual bool do_heap_region(HeapRegion* r) {
    assert(r->in_collection_set(), "Region %u must have been in collection set", r->hrm_index());
    G1CollectedHeap::heap()->clear_region_attr(r);
    r->clear_young_index_in_cset();
    return false;
  }
};

void G1CollectedHeap::abandon_collection_set(G1CollectionSet* collection_set) {
  G1AbandonCollectionSetClosure cl;
  collection_set_iterate_all(&cl);

  collection_set->clear();
  collection_set->stop_incremental_building();
}

bool G1CollectedHeap::is_old_gc_alloc_region(HeapRegion* hr) {
  return _allocator->is_retained_old_region(hr);
}

void G1CollectedHeap::set_region_short_lived_locked(HeapRegion* hr) {
  _eden.add(hr);
  _policy->set_region_eden(hr);
}

#ifdef ASSERT

class NoYoungRegionsClosure: public HeapRegionClosure {
private:
  bool _success;
public:
  NoYoungRegionsClosure() : _success(true) { }
  bool do_heap_region(HeapRegion* r) {
    if (r->is_young()) {
      log_error(gc, verify)("Region [" PTR_FORMAT ", " PTR_FORMAT ") tagged as young",
                            p2i(r->bottom()), p2i(r->end()));
      _success = false;
    }
    return false;
  }
  bool success() { return _success; }
};

bool G1CollectedHeap::check_young_list_empty() {
  bool ret = (young_regions_count() == 0);

  NoYoungRegionsClosure closure;
  heap_region_iterate(&closure);
  ret = ret && closure.success();

  return ret;
}

#endif // ASSERT

// Remove the given HeapRegion from the appropriate region set.
void G1CollectedHeap::prepare_region_for_full_compaction(HeapRegion* hr) {
   if (hr->is_humongous()) {
    _humongous_set.remove(hr);
  } else if (hr->is_old()) {
    _old_set.remove(hr);
  } else if (hr->is_young()) {
    // Note that emptying the eden and survivor lists is postponed and instead
    // done as the first step when rebuilding the regions sets again. The reason
    // for this is that during a full GC string deduplication needs to know if
    // a collected region was young or old when the full GC was initiated.
    hr->uninstall_surv_rate_group();
  } else {
    // We ignore free regions, we'll empty the free list afterwards.
    assert(hr->is_free(), "it cannot be another type");
  }
}

void G1CollectedHeap::increase_used(size_t bytes) {
  _summary_bytes_used += bytes;
}

void G1CollectedHeap::decrease_used(size_t bytes) {
  assert(_summary_bytes_used >= bytes,
         "invariant: _summary_bytes_used: " SIZE_FORMAT " should be >= bytes: " SIZE_FORMAT,
         _summary_bytes_used, bytes);
  _summary_bytes_used -= bytes;
}

void G1CollectedHeap::set_used(size_t bytes) {
  _summary_bytes_used = bytes;
}

class RebuildRegionSetsClosure : public HeapRegionClosure {
private:
  bool _free_list_only;

  HeapRegionSet* _old_set;
  HeapRegionSet* _humongous_set;

  HeapRegionManager* _hrm;

  size_t _total_used;

public:
  RebuildRegionSetsClosure(bool free_list_only,
                           HeapRegionSet* old_set,
                           HeapRegionSet* humongous_set,
                           HeapRegionManager* hrm) :
    _free_list_only(free_list_only), _old_set(old_set),
    _humongous_set(humongous_set), _hrm(hrm), _total_used(0) {
    assert(_hrm->num_free_regions() == 0, "pre-condition");
    if (!free_list_only) {
      assert(_old_set->is_empty(), "pre-condition");
      assert(_humongous_set->is_empty(), "pre-condition");
    }
  }

  bool do_heap_region(HeapRegion* r) {
    if (r->is_empty()) {
      assert(r->rem_set()->is_empty(), "Empty regions should have empty remembered sets.");
      // Add free regions to the free list
      r->set_free();
      _hrm->insert_into_free_list(r);
    } else if (!_free_list_only) {
      assert(r->rem_set()->is_empty(), "At this point remembered sets must have been cleared.");

      if (r->is_humongous()) {
        _humongous_set->add(r);
      } else {
        assert(r->is_young() || r->is_free() || r->is_old(), "invariant");
        // We now move all (non-humongous, non-old) regions to old gen,
        // and register them as such.
        r->move_to_old();
        _old_set->add(r);
      }
      _total_used += r->used();
    }

    return false;
  }

  size_t total_used() {
    return _total_used;
  }
};

void G1CollectedHeap::rebuild_region_sets(bool free_list_only) {
  assert_at_safepoint_on_vm_thread();

  if (!free_list_only) {
    _eden.clear();
    _survivor.clear();
  }

  RebuildRegionSetsClosure cl(free_list_only,
                              &_old_set, &_humongous_set,
                              &_hrm);
  heap_region_iterate(&cl);

  if (!free_list_only) {
    set_used(cl.total_used());
  }
  assert_used_and_recalculate_used_equal(this);
}

// Methods for the mutator alloc region

HeapRegion* G1CollectedHeap::new_mutator_alloc_region(size_t word_size,
                                                      bool force,
                                                      uint node_index) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  bool should_allocate = policy()->should_allocate_mutator_region();
  if (force || should_allocate) {
    HeapRegion* new_alloc_region = new_region(word_size,
                                              HeapRegionType::Eden,
                                              false /* do_expand */,
                                              node_index);
    if (new_alloc_region != nullptr) {
      set_region_short_lived_locked(new_alloc_region);
      _hr_printer.alloc(new_alloc_region, !should_allocate);
      _policy->remset_tracker()->update_at_allocate(new_alloc_region);
      return new_alloc_region;
    }
  }
  return nullptr;
}

void G1CollectedHeap::retire_mutator_alloc_region(HeapRegion* alloc_region,
                                                  size_t allocated_bytes) {
  assert_heap_locked_or_at_safepoint(true /* should_be_vm_thread */);
  assert(alloc_region->is_eden(), "all mutator alloc regions should be eden");

  collection_set()->add_eden_region(alloc_region);
  increase_used(allocated_bytes);
  _eden.add_used_bytes(allocated_bytes);
  _hr_printer.retire(alloc_region);

  // We update the eden sizes here, when the region is retired,
  // instead of when it's allocated, since this is the point that its
  // used space has been recorded in _summary_bytes_used.
  monitoring_support()->update_eden_size();
}

// Methods for the GC alloc regions

bool G1CollectedHeap::has_more_regions(G1HeapRegionAttr dest) {
  if (dest.is_old()) {
    return true;
  } else {
    return survivor_regions_count() < policy()->max_survivor_regions();
  }
}

HeapRegion* G1CollectedHeap::new_gc_alloc_region(size_t word_size, G1HeapRegionAttr dest, uint node_index) {
  assert(FreeList_lock->owned_by_self(), "pre-condition");

  if (!has_more_regions(dest)) {
    return nullptr;
  }

  HeapRegionType type;
  if (dest.is_young()) {
    type = HeapRegionType::Survivor;
  } else {
    type = HeapRegionType::Old;
  }

  HeapRegion* new_alloc_region = new_region(word_size,
                                            type,
                                            true /* do_expand */,
                                            node_index);

  if (new_alloc_region != nullptr) {
    if (type.is_survivor()) {
      new_alloc_region->set_survivor();
      _survivor.add(new_alloc_region);
      register_new_survivor_region_with_region_attr(new_alloc_region);
    } else {
      new_alloc_region->set_old();
    }
    _policy->remset_tracker()->update_at_allocate(new_alloc_region);
    register_region_with_region_attr(new_alloc_region);
    _hr_printer.alloc(new_alloc_region);
    return new_alloc_region;
  }
  return nullptr;
}

void G1CollectedHeap::retire_gc_alloc_region(HeapRegion* alloc_region,
                                             size_t allocated_bytes,
                                             G1HeapRegionAttr dest) {
  _bytes_used_during_gc += allocated_bytes;
  if (dest.is_old()) {
    old_set_add(alloc_region);
  } else {
    assert(dest.is_young(), "Retiring alloc region should be young (%d)", dest.type());
    _survivor.add_used_bytes(allocated_bytes);
  }

  bool const during_im = collector_state()->in_concurrent_start_gc();
  if (during_im && allocated_bytes > 0) {
    _cm->add_root_region(alloc_region);
  }
  _hr_printer.retire(alloc_region);
}

HeapRegion* G1CollectedHeap::alloc_highest_free_region() {
  bool expanded = false;
  uint index = _hrm.find_highest_free(&expanded);

  if (index != G1_NO_HRM_INDEX) {
    if (expanded) {
      log_debug(gc, ergo, heap)("Attempt heap expansion (requested address range outside heap bounds). region size: " SIZE_FORMAT "B",
                                HeapRegion::GrainWords * HeapWordSize);
    }
    return _hrm.allocate_free_regions_starting_at(index, 1);
  }
  return nullptr;
}

void G1CollectedHeap::mark_evac_failure_object(uint worker_id, const oop obj, size_t obj_size) const {
  assert(!_cm->is_marked_in_bitmap(obj), "must be");

  _cm->raw_mark_in_bitmap(obj);
  if (collector_state()->in_concurrent_start_gc()) {
    _cm->add_to_liveness(worker_id, obj, obj_size);
  }
}

// Optimized nmethod scanning
class RegisterNMethodOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  nmethod* _nm;

public:
  RegisterNMethodOopClosure(G1CollectedHeap* g1h, nmethod* nm) :
    _g1h(g1h), _nm(nm) {}

  void do_oop(oop* p) {
    oop heap_oop = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);
      HeapRegion* hr = _g1h->heap_region_containing(obj);
      assert(!hr->is_continues_humongous(),
             "trying to add code root " PTR_FORMAT " in continuation of humongous region " HR_FORMAT
             " starting at " HR_FORMAT,
             p2i(_nm), HR_FORMAT_PARAMS(hr), HR_FORMAT_PARAMS(hr->humongous_start_region()));

      // HeapRegion::add_code_root_locked() avoids adding duplicate entries.
      hr->add_code_root_locked(_nm);
    }
  }

  void do_oop(narrowOop* p) { ShouldNotReachHere(); }
};

class UnregisterNMethodOopClosure: public OopClosure {
  G1CollectedHeap* _g1h;
  nmethod* _nm;

public:
  UnregisterNMethodOopClosure(G1CollectedHeap* g1h, nmethod* nm) :
    _g1h(g1h), _nm(nm) {}

  void do_oop(oop* p) {
    oop heap_oop = RawAccess<>::oop_load(p);
    if (!CompressedOops::is_null(heap_oop)) {
      oop obj = CompressedOops::decode_not_null(heap_oop);
      HeapRegion* hr = _g1h->heap_region_containing(obj);
      assert(!hr->is_continues_humongous(),
             "trying to remove code root " PTR_FORMAT " in continuation of humongous region " HR_FORMAT
             " starting at " HR_FORMAT,
             p2i(_nm), HR_FORMAT_PARAMS(hr), HR_FORMAT_PARAMS(hr->humongous_start_region()));

      hr->remove_code_root(_nm);
    }
  }

  void do_oop(narrowOop* p) { ShouldNotReachHere(); }
};

void G1CollectedHeap::register_nmethod(nmethod* nm) {
  guarantee(nm != nullptr, "sanity");
  RegisterNMethodOopClosure reg_cl(this, nm);
  nm->oops_do(&reg_cl);
}

void G1CollectedHeap::unregister_nmethod(nmethod* nm) {
  guarantee(nm != nullptr, "sanity");
  UnregisterNMethodOopClosure reg_cl(this, nm);
  nm->oops_do(&reg_cl, true);
}

void G1CollectedHeap::update_used_after_gc(bool evacuation_failed) {
  if (evacuation_failed) {
    // Reset the G1EvacuationFailureALot counters and flags
    evac_failure_injector()->reset();

    set_used(recalculate_used());
  } else {
    // The "used" of the collection set have already been subtracted
    // when they were freed.  Add in the bytes used.
    increase_used(_bytes_used_during_gc);
  }
}

class RebuildCodeRootClosure: public CodeBlobClosure {
  G1CollectedHeap* _g1h;

public:
  RebuildCodeRootClosure(G1CollectedHeap* g1h) :
    _g1h(g1h) {}

  void do_code_blob(CodeBlob* cb) {
    nmethod* nm = cb->as_nmethod_or_null();
    if (nm != nullptr) {
      _g1h->register_nmethod(nm);
    }
  }
};

void G1CollectedHeap::rebuild_code_roots() {
  RebuildCodeRootClosure blob_cl(this);
  CodeCache::blobs_do(&blob_cl);
}

void G1CollectedHeap::initialize_serviceability() {
  _monitoring_support->initialize_serviceability();
}

MemoryUsage G1CollectedHeap::memory_usage() {
  return _monitoring_support->memory_usage();
}

GrowableArray<GCMemoryManager*> G1CollectedHeap::memory_managers() {
  return _monitoring_support->memory_managers();
}

GrowableArray<MemoryPool*> G1CollectedHeap::memory_pools() {
  return _monitoring_support->memory_pools();
}

void G1CollectedHeap::fill_with_dummy_object(HeapWord* start, HeapWord* end, bool zap) {
  HeapRegion* region = heap_region_containing(start);
  region->fill_with_dummy_object(start, pointer_delta(end, start), zap);
}

void G1CollectedHeap::start_codecache_marking_cycle_if_inactive(bool concurrent_mark_start) {
  // We can reach here with an active code cache marking cycle either because the
  // previous G1 concurrent marking cycle was undone (if heap occupancy after the
  // concurrent start young collection was below the threshold) or aborted. See
  // CodeCache::on_gc_marking_cycle_finish() why this is.  We must not start a new code
  // cache cycle then. If we are about to start a new g1 concurrent marking cycle we
  // still have to arm all nmethod entry barriers. They are needed for adding oop
  // constants to the SATB snapshot. Full GC does not need nmethods to be armed.
  if (!CodeCache::is_gc_marking_cycle_active()) {
    CodeCache::on_gc_marking_cycle_start();
  }
  if (concurrent_mark_start) {
    CodeCache::arm_all_nmethods();
  }
}

void G1CollectedHeap::finish_codecache_marking_cycle() {
  CodeCache::on_gc_marking_cycle_finish();
  CodeCache::arm_all_nmethods();
}
