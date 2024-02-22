/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "gc/z/zAddress.hpp"
#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "gc/shared/gcHeapSummary.hpp"
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zAbort.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zAllocator.inline.hpp"
#include "gc/z/zCollectedHeap.hpp"
#include "gc/z/zContinuation.inline.hpp"
#include "gc/z/zDirector.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zJNICritical.hpp"
#include "gc/z/zNMethod.hpp"
#include "gc/z/zObjArrayAllocator.hpp"
#include "gc/z/zServiceability.hpp"
#include "gc/z/zStackChunkGCData.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "memory/classLoaderMetaspace.hpp"
#include "memory/iterator.hpp"
#include "memory/metaspaceCriticalAllocation.hpp"
#include "memory/universe.hpp"
#include "oops/stackChunkOop.hpp"
#include "runtime/continuationJavaClasses.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "services/memoryUsage.hpp"
#include "utilities/align.hpp"

ZCollectedHeap* ZCollectedHeap::heap() {
  return named_heap<ZCollectedHeap>(CollectedHeap::Z);
}

ZCollectedHeap::ZCollectedHeap()
  : _soft_ref_policy(),
    _barrier_set(),
    _initialize(&_barrier_set),
    _heap(),
    _driver_minor(new ZDriverMinor()),
    _driver_major(new ZDriverMajor()),
    _director(new ZDirector()),
    _stat(new ZStat()),
    _runtime_workers() {}

CollectedHeap::Name ZCollectedHeap::kind() const {
  return CollectedHeap::Z;
}

const char* ZCollectedHeap::name() const {
  return ZName;
}

jint ZCollectedHeap::initialize() {
  if (!_heap.is_initialized()) {
    return JNI_ENOMEM;
  }

  Universe::set_verify_data(~(ZAddressHeapBase - 1) | 0x7, ZAddressHeapBase);

  return JNI_OK;
}

void ZCollectedHeap::initialize_serviceability() {
  _heap.serviceability_initialize();
}

class ZStopConcurrentGCThreadClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) {
    if (thread->is_ConcurrentGC_thread()) {
      ConcurrentGCThread::cast(thread)->stop();
    }
  }
};

void ZCollectedHeap::stop() {
  log_info_p(gc, exit)("Stopping ZGC");
  ZAbort::abort();
  ZStopConcurrentGCThreadClosure cl;
  gc_threads_do(&cl);
}

SoftRefPolicy* ZCollectedHeap::soft_ref_policy() {
  return &_soft_ref_policy;
}

size_t ZCollectedHeap::max_capacity() const {
  return _heap.max_capacity();
}

size_t ZCollectedHeap::capacity() const {
  return _heap.capacity();
}

size_t ZCollectedHeap::used() const {
  return _heap.used();
}

size_t ZCollectedHeap::unused() const {
  return _heap.unused();
}

bool ZCollectedHeap::is_maximal_no_gc() const {
  // Not supported
  ShouldNotReachHere();
  return false;
}

bool ZCollectedHeap::is_in(const void* p) const {
  return _heap.is_in((uintptr_t)p);
}

bool ZCollectedHeap::requires_barriers(stackChunkOop obj) const {
  return ZContinuation::requires_barriers(&_heap, obj);
}

HeapWord* ZCollectedHeap::allocate_new_tlab(size_t min_size, size_t requested_size, size_t* actual_size) {
  const size_t size_in_bytes = ZUtils::words_to_bytes(align_object_size(requested_size));
  const zaddress addr = ZAllocator::eden()->alloc_tlab(size_in_bytes);

  if (!is_null(addr)) {
    *actual_size = requested_size;
  }

  return (HeapWord*)untype(addr);
}

oop ZCollectedHeap::array_allocate(Klass* klass, size_t size, int length, bool do_zero, TRAPS) {
  const ZObjArrayAllocator allocator(klass, size, length, do_zero, THREAD);
  return allocator.allocate();
}

// ZCollectedHeap类的成员函数，用于分配内存。
HeapWord* ZCollectedHeap::mem_allocate(size_t size, bool* gc_overhead_limit_was_exceeded) {
  // 将请求的大小转换为字节，并按照对象大小对齐。
  const size_t size_in_bytes = ZUtils::words_to_bytes(align_object_size(size));
  // 使用ZAllocator的eden空间分配对象，并返回分配的内存地址。
  return (HeapWord*)ZAllocator::eden()->alloc_object(size_in_bytes);
}

// ZCollectedHeap类中处理元数据分配失败情况的成员函数
MetaWord* ZCollectedHeap::satisfy_failed_metadata_allocation(ClassLoaderData* loader_data,
                                                             size_t size,
                                                             Metaspace::MetadataType mdtype) {
  // 启动异步GC
  collect(GCCause::_metadata_GC_threshold);

  // 扩展并重试分配
  MetaWord* const result = loader_data->metaspace_non_null()->expand_and_allocate(size, mdtype);
  if (result != nullptr) {
    return result; // 如果成功，返回分配的元数据地址
  }

  // 作为最后的手段，尝试紧急的同步全GC分配
  return MetaspaceCriticalAllocation::allocate(loader_data, size, mdtype);
}

// ZCollectedHeap类中启动GC的成员函数
void ZCollectedHeap::collect(GCCause::Cause cause) {
  // 处理外部收集请求
  switch (cause) {
  case GCCause::_wb_young_gc:
  case GCCause::_scavenge_alot:
    // 启动紧急的年轻代GC
    _driver_minor->collect(ZDriverRequest(cause, ZYoungGCThreads, 0));
    break;

  case GCCause::_heap_dump:
  case GCCause::_heap_inspection:
  case GCCause::_wb_full_gc:
  case GCCause::_wb_breakpoint:
  case GCCause::_dcmd_gc_run:
  case GCCause::_java_lang_system_gc:
  case GCCause::_full_gc_alot:
  case GCCause::_jvmti_force_gc:
  case GCCause::_metadata_GC_clear_soft_refs:
  case GCCause::_codecache_GC_aggressive:
    // 启动紧急的主要GC
    _driver_major->collect(ZDriverRequest(cause, ZYoungGCThreads, ZOldGCThreads));
    break;

  case GCCause::_metadata_GC_threshold:
  case GCCause::_codecache_GC_threshold:
    // 启动非紧急的主要GC
    _driver_major->collect(ZDriverRequest(cause, 1, 1));
    break;

  default:
    fatal("Unsupported GC cause (%s)", GCCause::to_string(cause));
    break;
  }
}

// ZCollectedHeap类中与VM线程相关的GC操作的成员函数
void ZCollectedHeap::collect_as_vm_thread(GCCause::Cause cause) {
  // 这些收集请求被忽略，因为ZGC不能从VM线程内部运行一个同步的GC周期。
  // 认为这是无害的，因为这里唯一可能到来的GC原因应该是堆转储和堆检查。
  // 如果堆转储或堆检查明确请求了一个gc，并且调用者不是VM线程，
  // 在序言中会从调用者线程执行一个同步的GC周期。
  assert(Thread::current()->is_VM_thread(), "Should be the VM thread");
  guarantee(cause == GCCause::_heap_dump ||
            cause == GCCause::_heap_inspection, "Invalid cause");
}

// ZCollectedHeap类中执行全量垃圾收集的成员函数
void ZCollectedHeap::do_full_collection(bool clear_all_soft_refs) {
  // 不支持
  ShouldNotReachHere();
}

// ZCollectedHeap类中获取TLAB容量的成员函数
size_t ZCollectedHeap::tlab_capacity(Thread* ignored) const {
  return _heap.tlab_capacity();
}

// ZCollectedHeap类中获取TLAB使用的成员函数
size_t ZCollectedHeap::tlab_used(Thread* ignored) const {
  return _heap.tlab_used();
}

// ZCollectedHeap类中获取TLAB最大大小的成员函数
size_t ZCollectedHeap::max_tlab_size() const {
  return _heap.max_tlab_size();
}

// ZCollectedHeap类中获取TLAB不安全最大分配的成员函数
size_t ZCollectedHeap::unsafe_max_tlab_alloc(Thread* ignored) const {
  return _heap.unsafe_max_tlab_alloc();
}

// ZCollectedHeap类中判断是否使用栈水位标记屏障的成员函数
bool ZCollectedHeap::uses_stack_watermark_barrier() const {
  return true;
}

// ZCollectedHeap类中获取堆内存使用情况的成员函数
MemoryUsage ZCollectedHeap::memory_usage() {
  const size_t initial_size = ZHeap::heap()->initial_capacity();
  const size_t committed    = ZHeap::heap()->capacity();
  const size_t used         = MIN2(ZHeap::heap()->used(), committed);
  const size_t max_size     = ZHeap::heap()->max_capacity();

  return MemoryUsage(initial_size, used, committed, max_size);
}

// ZCollectedHeap类中获取所有内存管理器的成员函数
GrowableArray<GCMemoryManager*> ZCollectedHeap::memory_managers() {
  GrowableArray<GCMemoryManager*> memory_managers(4);
  memory_managers.append(_heap.serviceability_cycle_memory_manager(true /* minor */));
  memory_managers.append(_heap.serviceability_cycle_memory_manager(false /* minor */));
  memory_managers.append(_heap.serviceability_pause_memory_manager(true /* minor */));
  memory_managers.append(_heap.serviceability_pause_memory_manager(false /* minor */));
  return memory_managers;
}
// ZCollectedHeap类中与内存池和对象迭代相关的成员函数
GrowableArray<MemoryPool*> ZCollectedHeap::memory_pools() {
  GrowableArray<MemoryPool*> memory_pools(2);
  memory_pools.append(_heap.serviceability_memory_pool(ZGenerationId::young));
  memory_pools.append(_heap.serviceability_memory_pool(ZGenerationId::old));
  return memory_pools;
}

// 遍历堆中的所有对象
void ZCollectedHeap::object_iterate(ObjectClosure* cl) {
  _heap.object_iterate(cl, true /* visit_weaks */);
}

// 创建并返回一个并行对象迭代器
ParallelObjectIteratorImpl* ZCollectedHeap::parallel_object_iterator(uint nworkers) {
  return _heap.parallel_object_iterator(nworkers, true /* visit_weaks */);
}

// 使对象在当前Java线程中保持活动状态
void ZCollectedHeap::pin_object(JavaThread* thread, oop obj) {
  ZJNICritical::enter(thread);
}

// 使对象在当前Java线程中不再活动
void ZCollectedHeap::unpin_object(JavaThread* thread, oop obj) {
  ZJNICritical::exit(thread);
}

// 使对象保持活动状态，直到调用者释放它
void ZCollectedHeap::keep_alive(oop obj) {
  _heap.keep_alive(obj);
}

// 注册nmethod
void ZCollectedHeap::register_nmethod(nmethod* nm) {
  ZNMethod::register_nmethod(nm);
}

// 注销nmethod，在ZGC中，这需要在清理阶段执行
void ZCollectedHeap::unregister_nmethod(nmethod* nm) {
  ZNMethod::purge_nmethod(nm);
}

// 验证nmethod，ZGC中不做任何操作
void ZCollectedHeap::verify_nmethod(nmethod* nm) {
  // Does nothing
}

// 获取安全点工作的线程
WorkerThreads* ZCollectedHeap::safepoint_workers() {
  return _runtime_workers.workers();
}

// 执行GC线程的关闭
void ZCollectedHeap::gc_threads_do(ThreadClosure* tc) const {
  tc->do_thread(_director);
  tc->do_thread(_driver_major);
  tc->do_thread(_driver_minor);
  tc->do_thread(_stat);
  _heap.threads_do(tc);
  _runtime_workers.threads_do(tc);
}

// 创建堆空间摘要
VirtualSpaceSummary ZCollectedHeap::create_heap_space_summary() {
  const uintptr_t start = ZAddressHeapBase;

  // 伪造的值。ZGC不会在保留的地址空间中连续承诺内存，并且保留空间大于MaxHeapSize。
  const uintptr_t committed_end = ZAddressHeapBase + capacity();
  const uintptr_t reserved_end = ZAddressHeapBase + max_capacity();

  return VirtualSpaceSummary((HeapWord*)start, (HeapWord*)committed_end, (HeapWord*)reserved_end);
}

// 检查给定的oop指针是否包含null
bool ZCollectedHeap::contains_null(const oop* p) const {
  const zpointer* const ptr = (const zpointer*)p;
  return is_null_any(*ptr);
}

// ZCollectedHeap类中与安全点同步和验证相关的成员函数
void ZCollectedHeap::safepoint_synchronize_begin() {
  // 同步年轻代和老年代的重定位
  ZGeneration::young()->synchronize_relocation();
  ZGeneration::old()->synchronize_relocation();
  // 同步可暂停线程集合
  SuspendibleThreadSet::synchronize();
}

void ZCollectedHeap::safepoint_synchronize_end() {
  // 取消同步可暂停线程集合
  SuspendibleThreadSet::desynchronize();
  // 取消同步老年代的重定位
  ZGeneration::old()->desynchronize_relocation();
  // 取消同步年轻代的重定位
  ZGeneration::young()->desynchronize_relocation();
}

void ZCollectedHeap::prepare_for_verify() {
  // 什么都不做
}

void ZCollectedHeap::print_on(outputStream* st) const {
  _heap.print_on(st);
}

void ZCollectedHeap::print_on_error(outputStream* st) const {
  st->print_cr("ZGC Globals:"); // 打印ZGC全局信息
  st->print_cr(" Young Collection:   %s/%u", ZGeneration::young()->phase_to_string(), ZGeneration::young()->seqnum()); // 输出年轻代GC阶段和序列号
  st->print_cr(" Old Collection:     %s/%u", ZGeneration::old()->phase_to_string(), ZGeneration::old()->seqnum()); // 输出老年代GC阶段和序列号
  st->print_cr(" Offset Max:         " SIZE_FORMAT "%s (" PTR_FORMAT ")", // 输出最大地址偏移量及其对应的大小单位和实际值
               byte_size_in_exact_unit(ZAddressOffsetMax),
               exact_unit_for_byte_size(ZAddressOffsetMax),
               ZAddressOffsetMax);
  st->print_cr(" Page Size Small:    " SIZE_FORMAT "M", ZPageSizeSmall / M); // 输出小页面的大小（以兆字节为单位）
  st->print_cr(" Page Size Medium:   " SIZE_FORMAT "M", ZPageSizeMedium / M); // 输出中页面的大小（以兆字节为单位）
  st->cr(); // 换行
  st->print_cr("ZGC Metadata Bits:"); // 打印ZGC元数据位信息
  st->print_cr(" LoadGood:           " PTR_FORMAT, ZPointerLoadGoodMask); // 输出表示有效地址加载的元数据位
  st->print_cr(" LoadBad:            " PTR_FORMAT, ZPointerLoadBadMask); // 输出表示无效地址加载的元数据位
  st->print_cr(" MarkGood:           " PTR_FORMAT, ZPointerMarkGoodMask); // 输出表示有效标记的元数据位
  st->print_cr(" MarkBad:            " PTR_FORMAT, ZPointerMarkBadMask); // 输出表示无效标记的元数据位
  st->print_cr(" StoreGood:          " PTR_FORMAT, ZPointerStoreGoodMask); // 输出表示有效地址存储的元数据位
  st->print_cr(" StoreBad:           " PTR_FORMAT, ZPointerStoreBadMask); // 输出表示无效地址存储的元数据位
  st->print_cr(" ------------------- "); // 分隔线
  st->print_cr(" Remapped:           " PTR_FORMAT, ZPointerRemapped); // 输出表示重映射的元数据位
  st->print_cr(" RemappedYoung:      " PTR_FORMAT, ZPointerRemappedYoungMask); // 输出表示年轻代重映射的元数据位
  st->print_cr(" RemappedOld:        " PTR_FORMAT, ZPointerRemappedOldMask); // 输出表示老年代重映射的元数据位
  st->print_cr(" MarkedYoung:        " PTR_FORMAT, ZPointerMarkedYoung); // 输出表示年轻代标记的元数据位
  st->print_cr(" MarkedOld:          " PTR_FORMAT, ZPointerMarkedOld); // 输出表示老年代标记的元数据位
  st->print_cr(" Remembered:         " PTR_FORMAT, ZPointerRemembered); // 输出表示被记住的元数据位
  st->cr(); // 换行
  CollectedHeap::print_on_error(st); // 调用基类的print_on_error方法输出更多信息
}

void ZCollectedHeap::print_extended_on(outputStream* st) const {
  _heap.print_extended_on(st);
}

void ZCollectedHeap::print_tracing_info() const {
  // 什么都不做
}

bool ZCollectedHeap::print_location(outputStream* st, void* addr) const {
  return _heap.print_location(st, (uintptr_t)addr);
}

void ZCollectedHeap::verify(VerifyOption option /* ignored */) {
  fatal("Externally triggered verification not supported");
}

bool ZCollectedHeap::is_oop(oop object) const {
  return _heap.is_oop(cast_from_oop<uintptr_t>(object));
}

bool ZCollectedHeap::supports_concurrent_gc_breakpoints() const {
  return true;
}
