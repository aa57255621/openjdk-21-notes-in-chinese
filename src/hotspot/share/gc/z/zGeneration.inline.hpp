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

#ifndef SHARE_GC_Z_ZGENERATION_INLINE_HPP
#define SHARE_GC_Z_ZGENERATION_INLINE_HPP

#include "gc/z/zGeneration.hpp"

#include "gc/z/zAbort.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zWorkers.inline.hpp"
#include "utilities/debug.hpp"

// 检查当前阶段是否为重定位阶段
inline bool ZGeneration::is_phase_relocate() const {
  return _phase == Phase::Relocate;
}

// 检查当前阶段是否为标记阶段
inline bool ZGeneration::is_phase_mark() const {
  return _phase == Phase::Mark;
}

// 检查当前阶段是否为标记完成阶段
inline bool ZGeneration::is_phase_mark_complete() const {
  return _phase == Phase::MarkComplete;
}

// 获取当前代的序列号
inline uint32_t ZGeneration::seqnum() const {
  return _seqnum;
}

// 获取当前代的ID
inline ZGenerationId ZGeneration::id() const {
  return _id;
}

// 获取当前代的ID，以可选类型返回
inline ZGenerationIdOptional ZGeneration::id_optional() const {
  return static_cast<ZGenerationIdOptional>(_id);
}

// 检查当前代是否为年轻代
inline bool ZGeneration::is_young() const {
  return _id == ZGenerationId::young;
}

// 检查当前代是否为老年代
inline bool ZGeneration::is_old() const {
  return _id == ZGenerationId::old;
}

// 获取年轻代的指针
inline ZGenerationYoung* ZGeneration::young() {
  return _young;
}

// 获取老年代的指针
inline ZGenerationOld* ZGeneration::old() {
  return _old;
}

// 根据代ID获取对应的代
inline ZGeneration* ZGeneration::generation(ZGenerationId id) {
  if (id == ZGenerationId::young) {
    return _young;
  } else {
    return _old;
  }
}

// 获取给定地址的转发信息
inline ZForwarding* ZGeneration::forwarding(zaddress_unsafe addr) const {
  return _forwarding_table.get(addr);
}

// 检查是否应该调整工作线程的大小
inline bool ZGeneration::should_worker_resize() {
  return _workers.should_worker_resize();
}

// 获取堆的统计信息
inline ZStatHeap* ZGeneration::stat_heap() {
  return &_stat_heap;
}

// 获取当前代的统计周期信息
inline ZStatCycle* ZGeneration::stat_cycle() {
  return &_stat_cycle;
}

// 获取当前代的统计工作线程信息
inline ZStatWorkers* ZGeneration::stat_workers() {
  return &_stat_workers;
}

// 获取当前代的统计标记信息
inline ZStatMark* ZGeneration::stat_mark() {
  return &_stat_mark;
}

// 获取当前代的统计重定位信息
inline ZStatRelocation* ZGeneration::stat_relocation() {
  return &_stat_relocation;
}

// 获取当前代的页面表
inline ZPageTable* ZGeneration::page_table() const {
  return _page_table;
}

// 获取当前代的转发表
inline const ZForwardingTable* ZGeneration::forwarding_table() const {
  return &_forwarding_table;
}

// 标记对象的模板内联函数
template <bool resurrect, bool gc_thread, bool follow, bool finalizable>
inline void ZGeneration::mark_object(zaddress addr) {
  assert(is_phase_mark(), "Should be marking");
  _mark.mark_object<resurrect, gc_thread, follow, finalizable>(addr);
}

// 如果当前处于标记阶段，则标记对象
template <bool resurrect, bool gc_thread, bool follow, bool finalizable>
inline void ZGeneration::mark_object_if_active(zaddress addr) {
  if (is_phase_mark()) {
    mark_object<resurrect, gc_thread, follow, finalizable>(addr);
  }
}

// 重定位或重映射对象的内联函数
inline zaddress ZGeneration::relocate_or_remap_object(zaddress_unsafe addr) {
  ZForwarding* const forwarding = _forwarding_table.get(addr);
  if (forwarding == nullptr) {
    // 如果没有转发信息，直接返回安全的地址
    return safe(addr);
  }

  // 如果有转发信息，则重定位对象
  return _relocate.relocate_object(forwarding, addr);
}

// 重映射对象的内联函数
inline zaddress ZGeneration::remap_object(zaddress_unsafe addr) {
  ZForwarding* const forwarding = _forwarding_table.get(addr);
  if (forwarding == nullptr) {
    // 如果没有转发，直接返回安全的地址
    return safe(addr);
  }

  // 如果有转发，则重映射对象
  return _relocate.forward_object(forwarding, addr);
}

// 获取年轻代的类型
inline ZYoungType ZGenerationYoung::type() const {
  assert(_active_type != ZYoungType::none, "Invalid type");
  return _active_type;
}

// 在年轻代记忆集中记录一个指针
inline void ZGenerationYoung::remember(volatile zpointer* p) {
  _remembered.remember(p);
}

// 扫描年轻代记忆集中的字段
inline void ZGenerationYoung::scan_remembered_field(volatile zpointer* p) {
  _remembered.scan_field(p);
}

// 检查一个指针是否在年轻代记忆集中被记录
inline bool ZGenerationYoung::is_remembered(volatile zpointer* p) const {
  return _remembered.is_remembered(p);
}

// 获取老年代引用发现器
inline ReferenceDiscoverer* ZGenerationOld::reference_discoverer() {
  return &_reference_processor;
}

// 检查老年代的活动记忆集是否是当前的
inline bool ZGenerationOld::active_remset_is_current() const {
  assert(_young_seqnum_at_reloc_start != 0, "Must be set before used");

  // 记忆集位在每个年轻标记阶段都会翻转
  const uint32_t seqnum = ZGeneration::young()->seqnum();
  const uint32_t seqnum_diff = seqnum - _young_seqnum_at_reloc_start;
  const bool in_current = (seqnum_diff & 1u) == 0u;
  return in_current;
}

// 获取老年代的重定位队列
inline ZRelocateQueue* ZGenerationOld::relocate_queue() {
  return _relocate.queue();
}

#endif // SHARE_GC_Z_ZGENERATION_INLINE_HPP
