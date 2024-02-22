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

#include "precompiled.hpp"
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zFuture.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageAllocator.inline.hpp"
#include "gc/z/zPageCache.hpp"
#include "gc/z/zSafeDelete.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncommitter.hpp"
#include "gc/z/zUnmapper.hpp"
#include "gc/z/zWorkers.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"

static const ZStatCounter       ZCounterMutatorAllocationRate("Memory", "Allocation Rate", ZStatUnitBytesPerSecond);
static const ZStatCounter       ZCounterPageCacheFlush("Memory", "Page Cache Flush", ZStatUnitBytesPerSecond);
static const ZStatCounter       ZCounterDefragment("Memory", "Defragment", ZStatUnitOpsPerSecond);
static const ZStatCriticalPhase ZCriticalPhaseAllocationStall("Allocation Stall");

ZSafePageRecycle::ZSafePageRecycle(ZPageAllocator* page_allocator)
  : _page_allocator(page_allocator),
    _unsafe_to_recycle() {}

void ZSafePageRecycle::activate() {
  _unsafe_to_recycle.activate();
}

void ZSafePageRecycle::deactivate() {
  auto delete_function = [&](ZPage* page) {
    _page_allocator->safe_destroy_page(page);
  };

  _unsafe_to_recycle.deactivate_and_apply(delete_function);
}

ZPage* ZSafePageRecycle::register_and_clone_if_activated(ZPage* page) {
  if (!_unsafe_to_recycle.is_activated()) {
    // The page has no concurrent readers.
    // Recycle original page.
    return page;
  }

  // The page could have concurrent readers.
  // It would be unsafe to recycle this page at this point.

  // As soon as the page is added to _unsafe_to_recycle, it
  // must not be used again. Hence, the extra double-checked
  // locking to only clone the page if it is believed to be
  // unsafe to recycle the page.
  ZPage* const cloned_page = page->clone_limited();
  if (!_unsafe_to_recycle.add_if_activated(page)) {
    // It became safe to recycle the page after the is_activated check
    delete cloned_page;
    return page;
  }

  // The original page has been registered to be deleted by another thread.
  // Recycle the cloned page.
  return cloned_page;
}

class ZPageAllocation : public StackObj {
  friend class ZList<ZPageAllocation>;

private:
  const ZPageType            _type;
  const size_t               _size;
  const ZAllocationFlags     _flags;
  const uint32_t             _young_seqnum;
  const uint32_t             _old_seqnum;
  size_t                     _flushed;
  size_t                     _committed;
  ZList<ZPage>               _pages;
  ZListNode<ZPageAllocation> _node;
  ZFuture<bool>              _stall_result;

public:
  ZPageAllocation(ZPageType type, size_t size, ZAllocationFlags flags)
    : _type(type),
      _size(size),
      _flags(flags),
      _young_seqnum(ZGeneration::young()->seqnum()),
      _old_seqnum(ZGeneration::old()->seqnum()),
      _flushed(0),
      _committed(0),
      _pages(),
      _node(),
      _stall_result() {}

  ZPageType type() const {
    return _type;
  }

  size_t size() const {
    return _size;
  }

  ZAllocationFlags flags() const {
    return _flags;
  }

  uint32_t young_seqnum() const {
    return _young_seqnum;
  }

  uint32_t old_seqnum() const {
    return _old_seqnum;
  }

  size_t flushed() const {
    return _flushed;
  }

  void set_flushed(size_t flushed) {
    _flushed = flushed;
  }

  size_t committed() const {
    return _committed;
  }

  void set_committed(size_t committed) {
    _committed = committed;
  }

  bool wait() {
    return _stall_result.get();
  }

  ZList<ZPage>* pages() {
    return &_pages;
  }

  void satisfy(bool result) {
    _stall_result.set(result);
  }

  bool gc_relocation() const {
    return _flags.gc_relocation();
  }
};

ZPageAllocator::ZPageAllocator(size_t min_capacity,
                               size_t initial_capacity,
                               size_t soft_max_capacity,
                               size_t max_capacity)
  : _lock(),
    _cache(),
    _virtual(max_capacity),
    _physical(max_capacity),
    _min_capacity(min_capacity),
    _initial_capacity(initial_capacity),
    _max_capacity(max_capacity),
    _current_max_capacity(max_capacity),
    _capacity(0),
    _claimed(0),
    _used(0),
    _used_generations{0, 0},
    _collection_stats{{0, 0}, {0, 0}},
    _stalled(),
    _unmapper(new ZUnmapper(this)),
    _uncommitter(new ZUncommitter(this)),
    _safe_destroy(),
    _safe_recycle(this),
    _initialized(false) {

  if (!_virtual.is_initialized() || !_physical.is_initialized()) {
    return;
  }

  log_info_p(gc, init)("Min Capacity: " SIZE_FORMAT "M", min_capacity / M);
  log_info_p(gc, init)("Initial Capacity: " SIZE_FORMAT "M", initial_capacity / M);
  log_info_p(gc, init)("Max Capacity: " SIZE_FORMAT "M", max_capacity / M);
  log_info_p(gc, init)("Soft Max Capacity: " SIZE_FORMAT "M", soft_max_capacity / M);
  if (ZPageSizeMedium > 0) {
    log_info_p(gc, init)("Medium Page Size: " SIZE_FORMAT "M", ZPageSizeMedium / M);
  } else {
    log_info_p(gc, init)("Medium Page Size: N/A");
  }
  log_info_p(gc, init)("Pre-touch: %s", AlwaysPreTouch ? "Enabled" : "Disabled");

  // Warn if system limits could stop us from reaching max capacity
  _physical.warn_commit_limits(max_capacity);

  // Check if uncommit should and can be enabled
  _physical.try_enable_uncommit(min_capacity, max_capacity);

  // Successfully initialized
  _initialized = true;
}

bool ZPageAllocator::is_initialized() const {
  return _initialized;
}

class ZPreTouchTask : public ZTask {
private:
  const ZPhysicalMemoryManager* const _physical;
  volatile zoffset                    _start;
  const zoffset_end                   _end;

public:
  ZPreTouchTask(const ZPhysicalMemoryManager* physical, zoffset start, zoffset_end end)
    : ZTask("ZPreTouchTask"),
      _physical(physical),
      _start(start),
      _end(end) {}

  virtual void work() {
    for (;;) {
      // Get granule offset
      const size_t size = ZGranuleSize;
      const zoffset offset = to_zoffset(Atomic::fetch_then_add((uintptr_t*)&_start, size));
      if (offset >= _end) {
        // Done
        break;
      }

      // Pre-touch granule
      _physical->pretouch(offset, size);
    }
  }
};

bool ZPageAllocator::prime_cache(ZWorkers* workers, size_t size) {
  ZAllocationFlags flags;
  flags.set_non_blocking();
  flags.set_low_address();

  ZPage* const page = alloc_page(ZPageType::large, size, flags, ZPageAge::eden);
  if (page == nullptr) {
    return false;
  }

  if (AlwaysPreTouch) {
    // Pre-touch page
    ZPreTouchTask task(&_physical, page->start(), page->end());
    workers->run_all(&task);
  }

  free_page(page);

  return true;
}

size_t ZPageAllocator::initial_capacity() const {
  return _initial_capacity;
}

size_t ZPageAllocator::min_capacity() const {
  return _min_capacity;
}

size_t ZPageAllocator::max_capacity() const {
  return _max_capacity;
}

size_t ZPageAllocator::soft_max_capacity() const {
  // Note that SoftMaxHeapSize is a manageable flag
  const size_t soft_max_capacity = Atomic::load(&SoftMaxHeapSize);
  const size_t current_max_capacity = Atomic::load(&_current_max_capacity);
  return MIN2(soft_max_capacity, current_max_capacity);
}

size_t ZPageAllocator::capacity() const {
  return Atomic::load(&_capacity);
}

size_t ZPageAllocator::used() const {
  return Atomic::load(&_used);
}

size_t ZPageAllocator::used_generation(ZGenerationId id) const {
  return Atomic::load(&_used_generations[(int)id]);
}

size_t ZPageAllocator::unused() const {
  const ssize_t capacity = (ssize_t)Atomic::load(&_capacity);
  const ssize_t used = (ssize_t)Atomic::load(&_used);
  const ssize_t claimed = (ssize_t)Atomic::load(&_claimed);
  const ssize_t unused = capacity - used - claimed;
  return unused > 0 ? (size_t)unused : 0;
}

ZPageAllocatorStats ZPageAllocator::stats(ZGeneration* generation) const {
  ZLocker<ZLock> locker(&_lock);
  return ZPageAllocatorStats(_min_capacity,
                             _max_capacity,
                             soft_max_capacity(),
                             _capacity,
                             _used,
                             _collection_stats[(int)generation->id()]._used_high,
                             _collection_stats[(int)generation->id()]._used_low,
                             used_generation(generation->id()),
                             generation->freed(),
                             generation->promoted(),
                             generation->compacted(),
                             _stalled.size());
}

void ZPageAllocator::reset_statistics(ZGenerationId id) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");
  _collection_stats[(int)id]._used_high = _used;
  _collection_stats[(int)id]._used_low = _used;
}

size_t ZPageAllocator::increase_capacity(size_t size) {
  const size_t increased = MIN2(size, _current_max_capacity - _capacity);

  if (increased > 0) {
    // Update atomically since we have concurrent readers
    Atomic::add(&_capacity, increased);

    // Record time of last commit. When allocation, we prefer increasing
    // the capacity over flushing the cache. That means there could be
    // expired pages in the cache at this time. However, since we are
    // increasing the capacity we are obviously in need of committed
    // memory and should therefore not be uncommitting memory.
    _cache.set_last_commit();
  }

  return increased;
}

void ZPageAllocator::decrease_capacity(size_t size, bool set_max_capacity) {
  // Update atomically since we have concurrent readers
  Atomic::sub(&_capacity, size);

  if (set_max_capacity) {
    // Adjust current max capacity to avoid further attempts to increase capacity
    log_error_p(gc)("Forced to lower max Java heap size from "
                    SIZE_FORMAT "M(%.0f%%) to " SIZE_FORMAT "M(%.0f%%)",
                    _current_max_capacity / M, percent_of(_current_max_capacity, _max_capacity),
                    _capacity / M, percent_of(_capacity, _max_capacity));

    // Update atomically since we have concurrent readers
    Atomic::store(&_current_max_capacity, _capacity);
  }
}

void ZPageAllocator::increase_used(size_t size) {
  // We don't track generation usage here because this page
  // could be allocated by a thread that satisfies a stalling
  // allocation. The stalled thread can wake up and potentially
  // realize that the page alloc should be undone. If the alloc
  // and the undo gets separated by a safepoint, the generation
  // statistics could se a decreasing used value between mark
  // start and mark end.

  // Update atomically since we have concurrent readers
  const size_t used = Atomic::add(&_used, size);

  // Update used high
  for (auto& stats : _collection_stats) {
    if (used > stats._used_high) {
      stats._used_high = used;
    }
  }
}

void ZPageAllocator::decrease_used(size_t size) {
  // Update atomically since we have concurrent readers
  const size_t used = Atomic::sub(&_used, size);

  // Update used low
  for (auto& stats : _collection_stats) {
    if (used < stats._used_low) {
      stats._used_low = used;
    }
  }
}

void ZPageAllocator::increase_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::add(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZPageAllocator::decrease_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers
  Atomic::sub(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZPageAllocator::promote_used(size_t size) {
  decrease_used_generation(ZGenerationId::young, size);
  increase_used_generation(ZGenerationId::old, size);
}

bool ZPageAllocator::commit_page(ZPage* page) {
  // Commit physical memory
  return _physical.commit(page->physical_memory());
}

void ZPageAllocator::uncommit_page(ZPage* page) {
  if (!ZUncommit) {
    return;
  }

  // Uncommit physical memory
  _physical.uncommit(page->physical_memory());
}

void ZPageAllocator::map_page(const ZPage* page) const {
  // Map physical memory
  _physical.map(page->start(), page->physical_memory());
}

void ZPageAllocator::unmap_page(const ZPage* page) const {
  // Unmap physical memory
  _physical.unmap(page->start(), page->size());
}

// ZPageAllocator类中安全销毁页面的成员函数
void ZPageAllocator::safe_destroy_page(ZPage* page) {
  // 安全地销毁页面
  _safe_destroy.schedule_delete(page);
}

// ZPageAllocator类中销毁页面的成员函数
void ZPageAllocator::destroy_page(ZPage* page) {
  // 释放虚拟内存
  _virtual.free(page->virtual_memory());

  // 释放物理内存
  _physical.free(page->physical_memory());

  // 安全地销毁页面
  safe_destroy_page(page);
}

// ZPageAllocator类中判断是否允许分配的成员函数
bool ZPageAllocator::is_alloc_allowed(size_t size) const {
  const size_t available = _current_max_capacity - _used - _claimed; // 计算可用空间
  return available >= size; // 如果可用空间大于或等于请求的大小，则允许分配
}

// ZPageAllocator类中执行页面分配核心部分的成员函数
bool ZPageAllocator::alloc_page_common_inner(ZPageType type, size_t size, ZList<ZPage>* pages) {
  if (!is_alloc_allowed(size)) {
    // 内存不足
    return false;
  }

  // 尝试从页面缓存中分配
  ZPage* const page = _cache.alloc_page(type, size);
  if (page != nullptr) {
    // 成功
    pages->insert_last(page);
    return true;
  }

  // 尝试增加容量
  const size_t increased = increase_capacity(size);
  if (increased < size) {
    // 无法增加足够的容量来完全满足分配
    // 完全。刷新页面缓存以满足剩余部分。
    const size_t remaining = size - increased;
    _cache.flush_for_allocation(remaining, pages);
  }

  // 成功
  return true;
}

// ZPageAllocator类中执行页面分配公共部分的成员函数
bool ZPageAllocator::alloc_page_common(ZPageAllocation* allocation) {
  const ZPageType type = allocation->type(); // 获取页面类型
  const size_t size = allocation->size(); // 获取请求的大小
  const ZAllocationFlags flags = allocation->flags(); // 获取分配标志
  ZList<ZPage>* const pages = allocation->pages(); // 获取页面列表

  if (!alloc_page_common_inner(type, size, pages)) {
    // 内存不足
    return false;
  }

  // 更新已使用统计信息
  increase_used(size);

  // 成功
  return true;
}

// 检查初始化期间是否出现内存不足的静态函数
static void check_out_of_memory_during_initialization() {
  if (!is_init_completed()) {
    vm_exit_during_initialization("java.lang.OutOfMemoryError", "Java heap too small");
  }
}

// ZPageAllocator类中处理停止分配的成员函数
bool ZPageAllocator::alloc_page_stall(ZPageAllocation* allocation) {
  ZStatTimer timer(ZCriticalPhaseAllocationStall); // 开始计时
  EventZAllocationStall event; // 创建一个事件用于记录分配停止

  // 我们只能在VM完全初始化后停止
  check_out_of_memory_during_initialization();

  // 启动异步的年轻代GC
  const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, 0);
  ZDriver::minor()->collect(request);

  // 等待分配完成或失败
  const bool result = allocation->wait();

  {
    // 保护底层信号量的删除。这是一个解决glibc < 2.21中sem_post()的bug的变通方法，
    // 其中在从sem_wait()返回后立即销毁信号量是不安全的。原因是sem_post()可以在等待线程
    // 从sem_wait()返回后触摸信号量。为了避免这个竞争，我们强制等待线程获取/释放由发布线程持有的锁。
    ZLocker<ZLock> locker(&_lock);
  }

  // 发送事件
  event.commit((u8)allocation->type(), allocation->size());

  return result;
}

// ZPageAllocator类中尝试分配页面或停止的成员函数
bool ZPageAllocator::alloc_page_or_stall(ZPageAllocation* allocation) {
  {
    ZLocker<ZLock> locker(&_lock); // 使用互斥锁保护页面分配

    if (alloc_page_common(allocation)) {
      // 成功
      return true;
    }

    // 失败
    if (allocation->flags().non_blocking()) {
      // 不停止
      return false;
    }

    // 加入分配请求队列
    _stalled.insert_last(allocation);
  }

  // 停止
  return alloc_page_stall(allocation);
}

// ZPageAllocator类中创建新页面的成员函数
ZPage* ZPageAllocator::alloc_page_create(ZPageAllocation* allocation) {
  const size_t size = allocation->size(); // 获取请求的大小

  // 分配虚拟内存。为了使错误处理更加直接，我们在销毁刷新页面之前分配虚拟内存。
  // 刷新页面也异步地未映射和销毁，所以无论如何我们也不能立即重用地址空间的一部分。
  const ZVirtualMemory vmem = _virtual.alloc(size, allocation->flags().low_address());
  if (vmem.is_null()) {
    log_error(gc)("Out of address space"); // 打印地址空间不足的日志
    return nullptr; // 返回nullptr表示分配失败
  }

  ZPhysicalMemory pmem; // 初始化物理内存
  size_t flushed = 0; // 初始化已刷新的大小

  // 从刷新页面收集物理内存
  ZListRemoveIterator<ZPage> iter(allocation->pages());
  for (ZPage* page; iter.next(&page);) {
    flushed += page->size(); // 累加已刷新的大小

    // 收集已刷新的物理内存
    ZPhysicalMemory& fmem = page->physical_memory();
    pmem.add_segments(fmem); // 添加到物理内存
    fmem.remove_segments(); // 移除已添加的段

    // 未映射和销毁页面
    _unmapper->unmap_and_destroy_page(page);
  }

  if (flushed > 0) {
    allocation->set_flushed(flushed); // 设置已刷新的大小

    // 更新统计信息
    ZStatInc(ZCounterPageCacheFlush, flushed);
    log_debug(gc, heap)("Page Cache Flushed: " SIZE_FORMAT "M", flushed / M);
  }

  // 分配任何剩余的物理内存。容量和已使用量已经调整，我们只需要获取内存，这可以保证成功。
  if (flushed < size) {
    const size_t remaining = size - flushed;
    allocation->set_committed(remaining); // 设置已承诺的大小
    _physical.alloc(pmem, remaining); // 分配物理内存
  }

  // 创建新页面
  return new ZPage(allocation->type(), vmem, pmem); // 返回新创建的页面
}

// ZPageAllocator类中判断是否应该进行整理的成员函数
bool ZPageAllocator::should_defragment(const ZPage* page) const {
  // 如果一个小的页面出现在较高的地址（地址空间的第二半部分），
  // 如果我们已经分割了一个较大的页面，或者我们有一个受限制的地址空间。
  // 为了帮助解决地址空间碎片化问题，如果有一个较低的地址可用，
  // 我们会重映射这样的页面到较低的地址。
  return page->type() == ZPageType::small &&
         page->start() >= to_zoffset(_virtual.reserved() / 2) &&
         page->start() > _virtual.lowest_available_address();
}

// ZPageAllocator类中判断分配是否立即满足的成员函数
bool ZPageAllocator::is_alloc_satisfied(ZPageAllocation* allocation) const {
  // 如果页面列表包含恰好一个页面，并且该页面具有请求的类型和大小，
  // 则分配立即满足。但是，即使分配立即满足，我们在这里可能仍然希望返回false，
  // 以强制页面进行重映射，以帮助解决地址空间碎片化问题。

  if (allocation->pages()->size() != 1) {
    // 不是单个页面
    return false;
  }

  const ZPage* const page = allocation->pages()->first();
  if (page->type() != allocation->type() ||
      page->size() != allocation->size()) {
    // 类型或大小错误
    return false;
  }

  if (should_defragment(page)) {
    // 整理地址空间
    ZStatInc(ZCounterDefragment);
    return false;
  }

  // 分配立即满足
  return true;
}


// ZPageAllocator类中用于最终化页面分配的成员函数
ZPage* ZPageAllocator::alloc_page_finalize(ZPageAllocation* allocation) {
  // 快速路径
  if (is_alloc_satisfied(allocation)) {
    return allocation->pages()->remove_first();
  }
  // 慢速路径
  ZPage* const page = alloc_page_create(allocation);
  if (page == nullptr) {
    // 地址空间不足
    return nullptr;
  }
  // 提交页面
  if (commit_page(page)) {
    // 成功
    map_page(page);
    return page;
  }
  // 失败或部分失败。将任何成功提交的页面部分分割成新的页面，并将其插入页面列表中，
  // 以便它可以被重新插入到页面缓存中。
  ZPage* const committed_page = page->split_committed();
  destroy_page(page);
  if (committed_page != nullptr) {
    map_page(committed_page);
    allocation->pages()->insert_last(committed_page);
  }
  return nullptr;
}

// ZPageAllocator类中分配页面的成员函数
ZPage* ZPageAllocator::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags, ZPageAge age) {
  EventZPageAllocation event; // 创建一个事件用于记录页面分配

retry: // 定义一个标签，用于在分配失败时重试
  ZPageAllocation allocation(type, size, flags); // 创建一个页面分配对象

  // 从页面缓存中分配一个或多个页面。如果分配成功但返回的页面没有覆盖完整的分配，
  // 则允许最终化阶段直接从物理内存管理器分配剩余的内存。注意，如果未设置非阻塞标志，
  // 此调用可能会在安全点阻塞。
  if (!alloc_page_or_stall(&allocation)) {
    // 内存不足
    return nullptr;
  }

  ZPage* const page = alloc_page_finalize(&allocation); // 调用最终化方法分配页面
  if (page == nullptr) {
    // 提交或映射失败。清理并重试，希望我们仍然可以通过更积极地刷新页面缓存来分配内存。
    free_pages_alloc_failed(&allocation);
    goto retry; // 跳转到retry标签，重新尝试分配
  }

  // 当页面被分配给分配线程时，在这里跟踪代的已使用量。整体堆的“已使用”量在较低级别的分配代码中跟踪。
  const ZGenerationId id = age == ZPageAge::old ? ZGenerationId::old : ZGenerationId::young;
  increase_used_generation(id, size);

  // 重置页面。这更新了页面的序列号，并且必须在潜在的阻塞安全点（停止）之后执行，
  // 此时全局序列号可能已更新。
  page->reset(age, ZPageResetType::Allocation);

  // 更新分配统计信息。排除GC重定位以避免在重定位期间人为膨胀分配率。
  if (!flags.gc_relocation() && is_init_completed()) {
    // 注意，有两个分配率计数器，它们有不同的目的，并且以不同的频率进行采样。
    ZStatInc(ZCounterMutatorAllocationRate, size);
    ZStatMutatorAllocRate::sample_allocation(size);
  }

  // 发送事件
  event.commit((u8)type, size, allocation.flushed(), allocation.committed(),
               page->physical_memory().nsegments(), flags.non_blocking());

  return page; // 返回分配的页面
}

// ZPageAllocator类中处理停止分配的成员函数
void ZPageAllocator::satisfy_stalled() {
  for (;;) { // 无限循环，直到停止队列空为止
    ZPageAllocation* const allocation = _stalled.first(); // 获取停止队列的第一个分配请求
    if (allocation == nullptr) {
      // 分配请求队列为空
      return;
    }

    if (!alloc_page_common(allocation)) {
      // 分配无法满足，放弃
      return;
    }

    // 分配成功，移除并满足分配请求。注意，我们必须首先移除分配请求，因为一旦满足，它就会立即被释放。
    _stalled.remove(allocation);
    allocation->satisfy(true);
  }
}

// ZPageAllocator类中回收页面的成员函数
void ZPageAllocator::recycle_page(ZPage* page) {
  // 设置最后使用时间
  page->set_last_used();

  // 缓存页面
  _cache.free_page(page);
}

// ZPageAllocator类中释放页面的成员函数
void ZPageAllocator::free_page(ZPage* page) {
  const ZGenerationId generation_id = page->generation_id(); // 获取页面所属的代
  ZPage* const to_recycle = _safe_recycle.register_and_clone_if_activated(page); // 注册并克隆页面（如果激活）

  ZLocker<ZLock> locker(&_lock); // 使用互斥锁保护页面释放

  // 更新已使用统计信息
  const size_t size = to_recycle->size();
  decrease_used(size); // 减少整体已使用量
  decrease_used_generation(generation_id, size); // 减少指定代的已使用量

  // 回收页面
  recycle_page(to_recycle);

  // 尝试满足停止的分配
  satisfy_stalled();
}

// ZPageAllocator类中批量释放页面的成员函数
void ZPageAllocator::free_pages(const ZArray<ZPage*>* pages) {
  ZArray<ZPage*> to_recycle; // 初始化要回收的页面数组

  size_t young_size = 0; // 初始化年轻代大小
  size_t old_size = 0; // 初始化老年代大小

  ZArrayIterator<ZPage*> pages_iter(pages);
  for (ZPage* page; pages_iter.next(&page);) {
    if (page->is_young()) {
      young_size += page->size();
    } else {
      old_size += page->size();
    }
    to_recycle.push(_safe_recycle.register_and_clone_if_activated(page));
  }

  ZLocker<ZLock> locker(&_lock);

  // 更新已使用统计信息
  decrease_used(young_size + old_size);
  decrease_used_generation(ZGenerationId::young, young_size);
  decrease_used_generation(ZGenerationId::old, old_size);

  // 释放页面
  ZArrayIterator<ZPage*> iter(&to_recycle);
  for (ZPage* page; iter.next(&page);) {
    recycle_page(page);
  }

  // 尝试满足停止的分配
  satisfy_stalled();
}

// ZPageAllocator类中处理分配失败的成员函数
void ZPageAllocator::free_pages_alloc_failed(ZPageAllocation* allocation) {
  ZArray<ZPage*> to_recycle; // 初始化要回收的页面数组

  ZListRemoveIterator<ZPage> allocation_pages_iter(allocation->pages());
  for (ZPage* page; allocation_pages_iter.next(&page);) {
    to_recycle.push(_safe_recycle.register_and_clone_if_activated(page));
  }

  ZLocker<ZLock> locker(&_lock); // 使用互斥锁保护页面释放

  // 只减少整体已使用量，而不减少代已使用量，
  // 因为分配失败，代已使用量没有被增加。
  decrease_used(allocation->size());

  size_t freed = 0; // 初始化已释放的字节数为0

  // 释放任何已分配/已刷新的页面
  ZArrayIterator<ZPage*> iter(&to_recycle);
  for (ZPage* page; iter.next(&page);) {
    freed += page->size(); // 累加已释放的字节数
    recycle_page(page); // 回收页面
  }

  // 调整容量和已使用量以反映失败的容量增加
  const size_t remaining = allocation->size() - freed;
  decrease_capacity(remaining, true /* set_max_capacity */);

  // 尝试满足停止的分配
  satisfy_stalled();
}

// ZPageAllocator类中撤销承诺的成员函数
size_t ZPageAllocator::uncommit(uint64_t* timeout) {
  // 在操作容量和已使用量时，我们需要加入可暂停线程集合，以确保GC安全点具有一致的视图。
  ZList<ZPage> pages; // 初始化页面列表
  size_t flushed; // 初始化已刷新的大小

  {
    SuspendibleThreadSetJoiner sts_joiner; // 加入可暂停线程集合
    ZLocker<ZLock> locker(&_lock); // 使用互斥锁保护页面释放

    // 永远不会撤销承诺低于最小容量。我们一次刷新和撤销承诺一块内存（~0.8%的最大容量，但至少一个粒度且不超过256M），
    // 以免在撤销承诺期间内存需求增加。
    const size_t retain = MAX2(_used, _min_capacity);
    const size_t release = _capacity - retain;
    const size_t limit = MIN2(align_up(_current_max_capacity >> 7, ZGranuleSize), 256 * M);
    const size_t flush = MIN2(release, limit);

    // 刷新页面以撤销承诺
    flushed = _cache.flush_for_uncommit(flush, &pages, timeout);
    if (flushed == 0) {
      // 没有刷新任何内容
      return 0;
    }

    // 将刷新页面记录为已承诺
    Atomic::add(&_claimed, flushed);
  }

  // 取消映射、撤销承诺并销毁已刷新的页面
  ZListRemoveIterator<ZPage> iter(&pages);
  for (ZPage* page; iter.next(&page);) {
    unmap_page(page); // 取消映射页面
    uncommit_page(page); // 撤销承诺页面
    destroy_page(page); // 销毁页面
  }

  {
    SuspendibleThreadSetJoiner sts_joiner; // 加入可暂停线程集合
    ZLocker<ZLock> locker(&_lock); // 使用互斥锁保护页面释放

    // 调整已承诺和容量以反映撤销承诺
    Atomic::sub(&_claimed, flushed);
    decrease_capacity(flushed, false /* set_max_capacity */);
  }

  return flushed;
}

// ZPageAllocator类中启用安全销毁的常成员函数
void ZPageAllocator::enable_safe_destroy() const {
  _safe_destroy.enable_deferred_delete();
}

// ZPageAllocator类中禁用安全销毁的常成员函数
void ZPageAllocator::disable_safe_destroy() const {
  _safe_destroy.disable_deferred_delete();
}

// ZPageAllocator类中启用和禁用安全回收的常成员函数
void ZPageAllocator::enable_safe_recycle() const {
  _safe_recycle.activate();
}

void ZPageAllocator::disable_safe_recycle() const {
  _safe_recycle.deactivate();
}

// 静态函数，用于检查分配是否见过年轻代
static bool has_alloc_seen_young(const ZPageAllocation* allocation) {
  return allocation->young_seqnum() != ZGeneration::young()->seqnum();
}

// 静态函数，用于检查分配是否见过老年代
static bool has_alloc_seen_old(const ZPageAllocation* allocation) {
  return allocation->old_seqnum() != ZGeneration::old()->seqnum();
}

// ZPageAllocator类中判断是否正在停止分配的常成员函数
bool ZPageAllocator::is_alloc_stalling() const {
  ZLocker<ZLock> locker(&_lock);
  return _stalled.first() != nullptr;
}

// ZPageAllocator类中判断是否正在为老年代停止分配的常成员函数
bool ZPageAllocator::is_alloc_stalling_for_old() const {
  ZLocker<ZLock> locker(&_lock);

  ZPageAllocation* const allocation = _stalled.first();
  if (allocation == nullptr) {
    // 没有停止的分配
    return false;
  }

  return has_alloc_seen_young(allocation) && !has_alloc_seen_old(allocation);
}

// ZPageAllocator类中处理内存不足通知和重试GC的成员函数
void ZPageAllocator::notify_out_of_memory() {
  // 失败在最后一次主要GC开始之前排队分配请求
  for (ZPageAllocation* allocation = _stalled.first(); allocation != nullptr; allocation = _stalled.first()) {
    if (!has_alloc_seen_old(allocation)) {
      // 没有内存不足，保留剩余的分配请求排队
      return;
    }

    // 内存不足，移除并失败分配请求
    _stalled.remove(allocation);
    allocation->satisfy(false);
  }
}

// ZPageAllocator类中重启GC的常成员函数
void ZPageAllocator::restart_gc() const {
  ZPageAllocation* const allocation = _stalled.first();
  if (allocation == nullptr) {
    // 没有停止的分配
    return;
  }

  if (!has_alloc_seen_young(allocation)) {
    // 启动异步的年轻代GC，保留分配请求排队
    const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, 0);
    ZDriver::minor()->collect(request);
  } else {
    // 启动异步的主要GC，保留分配请求排队
    const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, ZOldGCThreads);
    ZDriver::major()->collect(request);
  }
}

// ZPageAllocator类中处理年轻代停止分配的成员函数
void ZPageAllocator::handle_alloc_stalling_for_young() {
  ZLocker<ZLock> locker(&_lock); // 使用互斥锁保护页面释放
  restart_gc(); // 重启GC
}

// ZPageAllocator类中处理老年代停止分配的成员函数
void ZPageAllocator::handle_alloc_stalling_for_old(bool cleared_soft_refs) {
  ZLocker<ZLock> locker(&_lock); // 使用互斥锁保护页面释放
  if (cleared_soft_refs) {
    notify_out_of_memory(); // 通知内存不足
  }
  restart_gc(); // 重启GC
}

// ZPageAllocator类中处理所有线程的成员函数
void ZPageAllocator::threads_do(ThreadClosure* tc) const {
  tc->do_thread(_unmapper); // 处理unmapper线程
  tc->do_thread(_uncommitter); // 处理uncommitter线程
}
