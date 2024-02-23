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

#ifndef SHARE_GC_Z_ZGLOBALS_HPP
#define SHARE_GC_Z_ZGLOBALS_HPP

#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"
#include CPU_HEADER(gc/z/zGlobals)

// Collector name
const char* const ZName                         = "The Z Garbage Collector"; // ZGC的名称

// Granule shift/size
const size_t      ZGranuleSizeShift             = 21; // 2^21 字节， 2MB的粒度大小位移
const size_t      ZGranuleSize                  = (size_t)1 << ZGranuleSizeShift; // 内存管理和垃圾收集操作的一个基本大小单位 2MB

// Virtual memory to physical memory ratio
const size_t      ZVirtualToPhysicalRatio       = 16; // 虚拟内存到物理内存的比例，16:1

// Max virtual memory ranges
const size_t      ZMaxVirtualReservations       = 100; // 每个保留区域至少占总内存的1%，最大虚拟内存保留区域数量

// Page size shifts
const size_t      ZPageSizeSmallShift           = ZGranuleSizeShift; // 小页的大小位移，与粒度大小相同
extern size_t     ZPageSizeMediumShift; // 中等页的大小位移，外部定义

// Page sizes
const size_t      ZPageSizeSmall                = (size_t)1 << ZPageSizeSmallShift; // 小页的大小，2MB
extern size_t     ZPageSizeMedium; // 中等页的大小，外部定义

// Object size limits
const size_t      ZObjectSizeLimitSmall         = ZPageSizeSmall / 8; // 小对象的大小限制，256KB，12.5%的最大浪费
extern size_t     ZObjectSizeLimitMedium; // 中等对象的大小限制，外部定义

// Object alignment shifts
extern const int& ZObjectAlignmentSmallShift; // 小对象对齐位移，外部定义
extern int        ZObjectAlignmentMediumShift; // 中等对象对齐位移，外部定义
const int         ZObjectAlignmentLargeShift    = ZGranuleSizeShift; // 大对象对齐位移，与粒度大小相同

// Object alignments
extern const int& ZObjectAlignmentSmall; // 小对象对齐大小，外部定义
extern int        ZObjectAlignmentMedium; // 中等对象对齐大小，外部定义
const int         ZObjectAlignmentLarge         = 1 << ZObjectAlignmentLargeShift; // 大对象对齐大小，2MB

// Cache line size
const size_t      ZCacheLineSize                = ZPlatformCacheLineSize; // 缓存行大小，与平台相关
#define           ZCACHE_ALIGNED                ATTRIBUTE_ALIGNED(ZCacheLineSize) // 缓存行对齐的宏定义

// Mark stack space
const size_t      ZMarkStackSpaceExpandSize     = (size_t)1 << 25; // 标记栈空间的扩展大小，32MB

// Mark stack and magazine sizes
const size_t      ZMarkStackSizeShift           = 11; // 标记栈的大小位移，2KB
const size_t      ZMarkStackSize                = (size_t)1 << ZMarkStackSizeShift; // 每个标记栈的大小
const size_t      ZMarkStackHeaderSize          = (size_t)1 << 4; // 标记栈的头部大小，16B
const size_t      ZMarkStackSlots               = (ZMarkStackSize - ZMarkStackHeaderSize) / sizeof(uintptr_t); // 标记栈中的槽位数
const size_t      ZMarkStackMagazineSize        = (size_t)1 << 15; // 标记栈杂志的大小，32KB
const size_t      ZMarkStackMagazineSlots       = (ZMarkStackMagazineSize / ZMarkStackSize) - 1; // 标记栈杂志的槽位数

// Mark stripe size
const size_t      ZMarkStripeShift              = ZGranuleSizeShift; // 标记条带的大小位移，与粒度大小相同

// Max number of mark stripes
const size_t      ZMarkStripesMax               = 16; // 标记条带的最大数量，必须是2的幂次方

// Mark cache size
const size_t      ZMarkCacheSize                = 1024; // 标记缓存的大小，必须是2的幂次方

// Partial array minimum size
const size_t      ZMarkPartialArrayMinSizeShift = 12; // 部分数组的 minimum size 位移，4KB
const size_t      ZMarkPartialArrayMinSize      = (size_t)1 << ZMarkPartialArrayMinSizeShift; // 部分数组的 minimum size，4KB
const size_t      ZMarkPartialArrayMinLength    = ZMarkPartialArrayMinSize / oopSize; // 部分数组的 minimum length，基于对象大小计算

// Max number of proactive/terminate flush attempts
const size_t      ZMarkProactiveFlushMax        = 10; // 主动刷新/终止尝试的最大次数

// Try complete mark timeout
const uint64_t    ZMarkCompleteTimeout          = 200; // 尝试完成标记的超时时间，单位为微秒 (us)

#endif // SHARE_GC_Z_ZGLOBALS_HPP