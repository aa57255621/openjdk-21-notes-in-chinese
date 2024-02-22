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
#include "gc/z/zGlobals.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zHeuristics.hpp"
#include "gc/z/zObjectAllocator.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageTable.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zValue.inline.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"

static const ZStatCounter ZCounterUndoObjectAllocationSucceeded("Memory", "Undo Object Allocation Succeeded", ZStatUnitOpsPerSecond);
static const ZStatCounter ZCounterUndoObjectAllocationFailed("Memory", "Undo Object Allocation Failed", ZStatUnitOpsPerSecond);

// ZObjectAllocator的构造函数
ZObjectAllocator::ZObjectAllocator(ZPageAge age)
  : _age(age), // 初始化页的年龄
    _use_per_cpu_shared_small_pages(ZHeuristics::use_per_cpu_shared_small_pages()), // 根据启发式判断是否使用每个CPU的共享小型页
    _used(0), // 初始化已使用字节为0
    _undone(0), // 初始化已撤销字节为0
    _shared_medium_page(nullptr), // 初始化共享中型页为nullptr
    _shared_small_page(nullptr) { // 初始化共享小型页为nullptr
}

// ZObjectAllocator的非常成员函数，用于获取共享小型页的地址
ZPage** ZObjectAllocator::shared_small_page_addr() {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

// ZObjectAllocator的常成员函数，用于获取共享小型页的地址
ZPage* const* ZObjectAllocator::shared_small_page_addr() const {
  return _use_per_cpu_shared_small_pages ? _shared_small_page.addr() : _shared_small_page.addr(0);
}

// ZObjectAllocator的成员函数，用于分配新页
ZPage* ZObjectAllocator::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags) {
  ZPage* const page = ZHeap::heap()->alloc_page(type, size, flags, _age);
  // 分配新页，并检查是否成功
  if (page != nullptr) {
    // 如果成功，增加已使用字节数
    Atomic::add(_used.addr(), size);
  }
  return page;
}

// ZObjectAllocator的成员函数，用于为重定位分配新页
ZPage* ZObjectAllocator::alloc_page_for_relocation(ZPageType type, size_t size, ZAllocationFlags flags) {
  return ZHeap::heap()->alloc_page(type, size, flags, _age);
}

// ZObjectAllocator的成员函数，用于撤销页的分配
void ZObjectAllocator::undo_alloc_page(ZPage* page) {
  // 增加已撤销字节数
  Atomic::add(_undone.addr(), page->size());
  // 调用ZHeap的方法来撤销页的分配
  ZHeap::heap()->undo_alloc_page(page);
}

// ZObjectAllocator类中在共享页上分配对象的函数
zaddress ZObjectAllocator::alloc_object_in_shared_page(ZPage** shared_page,
                                                       ZPageType page_type,
                                                       size_t page_size,
                                                       size_t size,
                                                       ZAllocationFlags flags) {
  zaddress addr = zaddress::null; // 初始化地址为null
  ZPage* page = Atomic::load_acquire(shared_page); // 使用原子操作读取共享页的指针

  if (page != nullptr) {
    // 如果共享页不为空，尝试在共享页上原子地分配对象
    addr = page->alloc_object_atomic(size);
  }

  if (is_null(addr)) {
    // 如果分配失败（地址为null），则分配新的页
    ZPage* const new_page = alloc_page(page_type, page_size, flags);
    if (new_page != nullptr) {
      // 在安装新页之前在新页上分配对象
      addr = new_page->alloc_object(size);

    retry:
      // 尝试安装新页
      ZPage* const prev_page = Atomic::cmpxchg(shared_page, page, new_page);
      if (prev_page != page) {
        if (prev_page == nullptr) {
          // 如果之前的页已经被淘汰，重试安装新页
          page = prev_page;
          goto retry;
        }

        // 如果另一个页已经安装，首先尝试在那里分配
        const zaddress prev_addr = prev_page->alloc_object_atomic(size);
        if (is_null(prev_addr)) {
          // 如果分配失败，重试安装新页
          page = prev_page;
          goto retry;
        }

        // 如果在已安装的页上分配成功，更新地址
        addr = prev_addr;

        // 撤销新页的分配
        undo_alloc_page(new_page);
      }
    }
  }

  // 返回分配的对象地址
  return addr;
}

// ZObjectAllocator类中分配大对象的函数
zaddress ZObjectAllocator::alloc_large_object(size_t size, ZAllocationFlags flags) {
  zaddress addr = zaddress::null; // 初始化地址为null

  // 分配新的large page，对齐到ZGranuleSize
  const size_t page_size = align_up(size, ZGranuleSize);
  ZPage* const page = alloc_page(ZPageType::large, page_size, flags);
  if (page != nullptr) {
    // 如果page分配成功，则在page上分配对象
    addr = page->alloc_object(size);
  }

  // 返回分配的对象地址
  return addr;
}

// ZObjectAllocator类中分配中等对象的函数
zaddress ZObjectAllocator::alloc_medium_object(size_t size, ZAllocationFlags flags) {
  // 在共享的中等page上分配对象
  return alloc_object_in_shared_page(_shared_medium_page.addr(), ZPageType::medium, ZPageSizeMedium, size, flags);
}

// ZObjectAllocator类中分配小对象的函数
zaddress ZObjectAllocator::alloc_small_object(size_t size, ZAllocationFlags flags) {
  // 在共享的小page上分配对象
  return alloc_object_in_shared_page(shared_small_page_addr(), ZPageType::small, ZPageSizeSmall, size, flags);
}

// ZObjectAllocator类中根据对象大小分配对象的函数
zaddress ZObjectAllocator::alloc_object(size_t size, ZAllocationFlags flags) {
  if (size <= ZObjectSizeLimitSmall) {
    // 如果对象大小为小对象，则调用alloc_small_object分配小对象
    return alloc_small_object(size, flags);
  } else if (size <= ZObjectSizeLimitMedium) {
    // 如果对象大小为中等对象，则调用alloc_medium_object分配中等对象
    return alloc_medium_object(size, flags);
  } else {
    // 如果对象大小为大对象，则调用alloc_large_object分配大对象
    return alloc_large_object(size, flags);
  }
}
// ZObjectAllocator类中分配对象的简化函数，使用默认的分配标志
zaddress ZObjectAllocator::alloc_object(size_t size) {
  const ZAllocationFlags flags; // 创建一个默认的ZAllocationFlags实例
  return alloc_object(size, flags); // 调用主要的分配函数
}

// ZObjectAllocator类中为对象重定位分配内存的函数
zaddress ZObjectAllocator::alloc_object_for_relocation(size_t size) {
  ZAllocationFlags flags; // 创建一个ZAllocationFlags实例
  flags.set_non_blocking(); // 设置非阻塞标志，用于重定位时可能不需要等待

  return alloc_object(size, flags); // 调用主要的分配函数
}

// ZObjectAllocator类中撤销为对象重定位分配的内存的函数
void ZObjectAllocator::undo_alloc_object_for_relocation(zaddress addr, size_t size) {
  ZPage* const page = ZHeap::heap()->page(addr); // 获取地址对应的ZPage

  if (page->is_large()) { // 如果是large page
    undo_alloc_page(page); // 撤销分配的page
    ZStatInc(ZCounterUndoObjectAllocationSucceeded); // 更新统计信息，表示撤销成功
  } else {
    if (page->undo_alloc_object_atomic(addr, size)) { // 尝试原子方式撤销对象分配
      ZStatInc(ZCounterUndoObjectAllocationSucceeded); // 如果成功，更新统计信息
    } else {
      ZStatInc(ZCounterUndoObjectAllocationFailed); // 如果失败，更新统计信息
    }
  }
}

// ZObjectAllocator类中获取年龄的常成员函数
ZPageAge ZObjectAllocator::age() const {
  return _age; // 返回页的年龄
}

// ZObjectAllocator类中获取已使用内存大小的常成员函数
size_t ZObjectAllocator::used() const {
  size_t total_used = 0; // 初始化总已使用字节数为0
  size_t total_undone = 0; // 初始化总已撤销字节数为0

  // 使用ZPerCPUConstIterator迭代器遍历每个CPU的已使用字节数
  ZPerCPUConstIterator<size_t> iter_used(&_used);
  for (const size_t* cpu_used; iter_used.next(&cpu_used);) {
    total_used += *cpu_used; // 累加每个CPU的已使用字节数
  }

  // 使用ZPerCPUConstIterator迭代器遍历每个CPU的已撤销字节数
  ZPerCPUConstIterator<size_t> iter_undone(&_undone);
  for (const size_t* cpu_undone; iter_undone.next(&cpu_undone);) {
    total_undone += *cpu_undone; // 累加每个CPU的已撤销字节数
  }

  // 返回净已使用字节数（已使用字节数减去已撤销字节数）
  return total_used - total_undone;
}

// ZObjectAllocator类中获取剩余内存大小的常成员函数
size_t ZObjectAllocator::remaining() const {
  assert(Thread::current()->is_Java_thread(), "Should be a Java thread"); // 断言当前线程是Java线程

  // 使用原子操作读取共享小型页的地址
  const ZPage* const page = Atomic::load_acquire(shared_small_page_addr());
  if (page != nullptr) {
    // 如果共享小型页不为空，返回页的剩余空间大小
    return page->remaining();
  }

  return 0; // 如果共享小型页为空，返回0
}

// ZObjectAllocator类中退役页面的成员函数
void ZObjectAllocator::retire_pages() {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint"); // 断言当前处于安全点

  // 重置已使用字节数和已撤销字节数
  _used.set_all(0);
  _undone.set_all(0);

  // 重置共享中型页和共享小型页
  _shared_medium_page.set(nullptr);
  _shared_small_page.set_all(nullptr);
}
