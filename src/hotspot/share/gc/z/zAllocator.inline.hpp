/*
 * Copyright (c) 2021, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZALLOCATOR_INLINE_HPP
#define SHARE_GC_Z_ZALLOCATOR_INLINE_HPP

#include "gc/z/zAllocator.hpp"

#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zHeap.hpp"

inline ZAllocatorEden* ZAllocator::eden() {
  return _eden;
}

inline ZAllocatorForRelocation* ZAllocator::relocation(ZPageAge page_age) {
  return _relocation[static_cast<uint>(page_age) - 1];
}

inline ZAllocatorForRelocation* ZAllocator::old() {
  return relocation(ZPageAge::old);
}

inline zaddress ZAllocatorEden::alloc_tlab(size_t size) {
  guarantee(size <= ZHeap::heap()->max_tlab_size(), "TLAB too large");
  return _object_allocator.alloc_object(size);
}

// 定义一个内联函数，用于在Eden空间分配对象
inline zaddress ZAllocatorEden::alloc_object(size_t size) {
  // 调用_object_allocator的alloc_object方法分配指定大小的对象
  const zaddress addr = _object_allocator.alloc_object(size);

  // 检查分配的地址是否为空
  if (is_null(addr)) {
    // 如果为空，则调用ZHeap的heap方法获取堆实例，然后调用out_of_memory方法处理内存不足的情况
    ZHeap::heap()->out_of_memory();
  }

  // 返回分配的对象地址
  return addr;
}


#endif // SHARE_GC_Z_ZALLOCATOR_INLINE_HPP
