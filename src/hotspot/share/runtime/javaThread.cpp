/*
 * Copyright (c) 1997, 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2021, Azul Systems, Inc. All rights reserved.
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
#include "cds/dynamicArchive.hpp"
#include "ci/ciEnv.hpp"
#include "classfile/javaClasses.inline.hpp"
#include "classfile/javaThreadStatus.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmClasses.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/codeCache.hpp"
#include "code/scopeDesc.hpp"
#include "compiler/compileTask.hpp"
#include "compiler/compilerThread.hpp"
#include "gc/shared/oopStorage.hpp"
#include "gc/shared/oopStorageSet.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "jfr/jfrEvents.hpp"
#include "jvm.h"
#include "jvmtifiles/jvmtiEnv.hpp"
#include "logging/log.hpp"
#include "logging/logAsyncWriter.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "oops/access.inline.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oopHandle.inline.hpp"
#include "oops/verifyOopClosure.hpp"
#include "prims/jvm_misc.hpp"
#include "prims/jvmtiDeferredUpdates.hpp"
#include "prims/jvmtiExport.hpp"
#include "prims/jvmtiThreadState.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/continuation.hpp"
#include "runtime/continuationEntry.inline.hpp"
#include "runtime/continuationHelper.inline.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/handshake.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/lockStack.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/orderAccess.hpp"
#include "runtime/osThread.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/safepointMechanism.inline.hpp"
#include "runtime/safepointVerifiers.hpp"
#include "runtime/serviceThread.hpp"
#include "runtime/stackFrameStream.inline.hpp"
#include "runtime/stackWatermarkSet.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/threadCritical.hpp"
#include "runtime/threadSMR.inline.hpp"
#include "runtime/threadStatisticalInfo.hpp"
#include "runtime/threadWXSetters.inline.hpp"
#include "runtime/timer.hpp"
#include "runtime/timerTrace.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vframeArray.hpp"
#include "runtime/vframe_hp.hpp"
#include "runtime/vmThread.hpp"
#include "runtime/vmOperations.hpp"
#include "services/threadService.hpp"
#include "utilities/copy.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/dtrace.hpp"
#include "utilities/events.hpp"
#include "utilities/macros.hpp"
#include "utilities/preserveException.hpp"
#include "utilities/spinYield.hpp"
#include "utilities/vmError.hpp"
#if INCLUDE_JVMCI
#include "jvmci/jvmci.hpp"
#include "jvmci/jvmciEnv.hpp"
#endif
#if INCLUDE_JFR
#include "jfr/jfr.hpp"
#endif

// Set by os layer.
size_t      JavaThread::_stack_size_at_create = 0;

#ifdef DTRACE_ENABLED

// Only bother with this argument setup if dtrace is available

  #define HOTSPOT_THREAD_PROBE_start HOTSPOT_THREAD_START
  #define HOTSPOT_THREAD_PROBE_stop HOTSPOT_THREAD_STOP

  #define DTRACE_THREAD_PROBE(probe, javathread)                           \
    {                                                                      \
      ResourceMark rm(this);                                               \
      int len = 0;                                                         \
      const char* name = (javathread)->name();                             \
      len = strlen(name);                                                  \
      HOTSPOT_THREAD_PROBE_##probe(/* probe = start, stop */               \
        (char *) name, len,                                                \
        java_lang_Thread::thread_id((javathread)->threadObj()),            \
        (uintptr_t) (javathread)->osthread()->thread_id(),                 \
        java_lang_Thread::is_daemon((javathread)->threadObj()));           \
    }

#else //  ndef DTRACE_ENABLED

  #define DTRACE_THREAD_PROBE(probe, javathread)

#endif // ndef DTRACE_ENABLED

void JavaThread::smr_delete() {
  if (_on_thread_list) {
    ThreadsSMRSupport::smr_delete(this);
  } else {
    delete this;
  }
}

// Initialized by VMThread at vm_global_init
OopStorage* JavaThread::_thread_oop_storage = nullptr;

OopStorage* JavaThread::thread_oop_storage() {
  assert(_thread_oop_storage != nullptr, "not yet initialized");
  return _thread_oop_storage;
}

void JavaThread::set_threadOopHandles(oop p) {
  assert(_thread_oop_storage != nullptr, "not yet initialized");
  _threadObj   = OopHandle(_thread_oop_storage, p);
  _vthread     = OopHandle(_thread_oop_storage, p);
  _jvmti_vthread = OopHandle(_thread_oop_storage, p->is_a(vmClasses::BoundVirtualThread_klass()) ? p : nullptr);
  _scopedValueCache = OopHandle(_thread_oop_storage, nullptr);
}

oop JavaThread::threadObj() const {
  // Ideally we would verify the current thread is oop_safe when this is called, but as we can
  // be called from a signal handler we would have to use Thread::current_or_null_safe(). That
  // has overhead and also interacts poorly with GetLastError on Windows due to the use of TLS.
  // Instead callers must verify oop safe access.
  return _threadObj.resolve();
}

oop JavaThread::vthread() const {
  return _vthread.resolve();
}

void JavaThread::set_vthread(oop p) {
  assert(_thread_oop_storage != nullptr, "not yet initialized");
  _vthread.replace(p);
}

oop JavaThread::jvmti_vthread() const {
  return _jvmti_vthread.resolve();
}

void JavaThread::set_jvmti_vthread(oop p) {
  assert(_thread_oop_storage != nullptr, "not yet initialized");
  _jvmti_vthread.replace(p);
}

oop JavaThread::scopedValueCache() const {
  return _scopedValueCache.resolve();
}

void JavaThread::set_scopedValueCache(oop p) {
  if (_scopedValueCache.ptr_raw() != nullptr) { // i.e. if the OopHandle has been allocated
    _scopedValueCache.replace(p);
  } else {
    assert(p == nullptr, "not yet initialized");
  }
}

void JavaThread::clear_scopedValueBindings() {
  set_scopedValueCache(nullptr);
  oop vthread_oop = vthread();
  // vthread may be null here if we get a VM error during startup,
  // before the java.lang.Thread instance has been created.
  if (vthread_oop != nullptr) {
    java_lang_Thread::clear_scopedValueBindings(vthread_oop);
  }
}

// Java虚拟机（JVM）中分配和初始化Java线程对象的方法。这个方法定义在JavaThread类中，用于创建一个新的Java线程对象，并将其与当前的JavaThread对象关联起来
void JavaThread::allocate_threadObj(Handle thread_group, const char* thread_name,
                                    bool daemon, TRAPS) {
  // 确保传入的线程组对象不为空
  assert(thread_group.not_null(), "thread group should be specified");
  // 确保Java线程对象还未被创建
  assert(threadObj() == nullptr, "should only create Java thread object once");

  // 获取Thread类的InstanceKlass对象
  InstanceKlass* ik = vmClasses::Thread_klass();
  // 确保Thread类已经被初始化
  assert(ik->is_initialized(), "must be");
  // 分配一个新的Java线程对象实例
  instanceHandle thread_oop = ik->allocate_instance_handle(CHECK);

  // 设置当前Java线程对象与JavaThread的关联
  java_lang_Thread::set_thread(thread_oop(), this);
  // 设置线程对象的句柄
  set_threadOopHandles(thread_oop());

  JavaValue result(T_VOID);
  // 如果指定了线程名称
  if (thread_name != nullptr) {
    // 创建一个新的字符串对象，作为线程的名称
    Handle name = java_lang_String::create_from_str(thread_name, CHECK);
    // 调用线程对象的初始化方法，传入线程组和线程名称
    JavaCalls::call_special(&result,
                            thread_oop,
                            ik,
                            vmSymbols::object_initializer_name(),
                            vmSymbols::threadgroup_string_void_signature(),
                            thread_group,
                            name,
                            CHECK);
  } else {
    // 如果没有指定线程名称，使用默认的名称"Thread-nnn"
    JavaCalls::call_special(&result,
                            thread_oop,
                            ik,
                            vmSymbols::object_initializer_name(),
                            vmSymbols::threadgroup_runnable_void_signature(),
                            thread_group,
                            Handle(),
                            CHECK);
  }
  // 设置线程的优先级为默认优先级
  os::set_priority(this, NormPriority);

  // 如果是守护线程，设置线程对象为守护线程
  if (daemon) {
    java_lang_Thread::set_daemon(thread_oop());
  }
}


// ======= JavaThread ========

#if INCLUDE_JVMCI

jlong* JavaThread::_jvmci_old_thread_counters;

bool jvmci_counters_include(JavaThread* thread) {
  return !JVMCICountersExcludeCompiler || !thread->is_Compiler_thread();
}

void JavaThread::collect_counters(jlong* array, int length) {
  // 确认传入的数组长度等于JVMCI计数器的大小
  assert(length == JVMCICounterSize, "wrong value");
  // 将当前线程的旧计数器值复制到传入的数组中
  for (int i = 0; i < length; i++) {
    array[i] = _jvmci_old_thread_counters[i];
  }
  // 遍历所有线程
  for (JavaThread* tp : ThreadsListHandle()) {
    // 如果应该包括这个线程的计数器
    if (jvmci_counters_include(tp)) {
      // 将这个线程的计数器值累加到传入的数组中
      for (int i = 0; i < length; i++) {
        array[i] += tp->_jvmci_counters[i];
      }
    }
  }
}


// Attempt to enlarge the array for per thread counters.
jlong* resize_counters_array(jlong* old_counters, int current_size, int new_size) {
  // 分配一个新的计数器数组
  jlong* new_counters = NEW_C_HEAP_ARRAY_RETURN_NULL(jlong, new_size, mtJVMCI);
  if (new_counters == nullptr) {
    // 如果分配失败，返回nullptr
    return nullptr;
  }
  if (old_counters == nullptr) {
    // 如果旧的计数器数组为空，使用新分配的数组，并将其初始化为0
    old_counters = new_counters;
    memset(old_counters, 0, sizeof(jlong) * new_size);
  } else {
    // 如果旧的计数器数组不为空，复制旧数组的内容到新数组
    for (int i = 0; i < MIN2((int) current_size, new_size); i++) {
      new_counters[i] = old_counters[i];
    }
    // 如果新数组的尺寸大于旧数组，将新数组的剩余部分初始化为0
    if (new_size > current_size) {
      memset(new_counters + current_size, 0, sizeof(jlong) * (new_size - current_size));
    }
    // 释放旧的计数器数组
    FREE_C_HEAP_ARRAY(jlong, old_counters);
  }
  // 返回新的计数器数组
  return new_counters;
}


// Attempt to enlarge the array for per thread counters.
bool JavaThread::resize_counters(int current_size, int new_size) {
  jlong* new_counters = resize_counters_array(_jvmci_counters, current_size, new_size);
  if (new_counters == nullptr) {
    return false;
  } else {
    _jvmci_counters = new_counters;
    return true;
  }
}

class VM_JVMCIResizeCounters : public VM_Operation {
 private:
  int _new_size;
  bool _failed;

 public:
  VM_JVMCIResizeCounters(int new_size) : _new_size(new_size), _failed(false) { }
  VMOp_Type type()                  const        { return VMOp_JVMCIResizeCounters; }
  bool allow_nested_vm_operations() const        { return true; }
  void doit() {
    // Resize the old thread counters array
    jlong* new_counters = resize_counters_array(JavaThread::_jvmci_old_thread_counters, JVMCICounterSize, _new_size);
    if (new_counters == nullptr) {
      _failed = true;
      return;
    } else {
      JavaThread::_jvmci_old_thread_counters = new_counters;
    }

    // Now resize each threads array
    for (JavaThread* tp : ThreadsListHandle()) {
      if (!tp->resize_counters(JVMCICounterSize, _new_size)) {
        _failed = true;
        break;
      }
    }
    if (!_failed) {
      JVMCICounterSize = _new_size;
    }
  }

  bool failed() { return _failed; }
};

bool JavaThread::resize_all_jvmci_counters(int new_size) {
  VM_JVMCIResizeCounters op(new_size);
  VMThread::execute(&op);
  return !op.failed();
}

#endif // INCLUDE_JVMCI

#ifdef ASSERT
// Checks safepoint allowed and clears unhandled oops at potential safepoints.
void JavaThread::check_possible_safepoint() {
  if (_no_safepoint_count > 0) {
    print_owned_locks();
    assert(false, "Possible safepoint reached by thread that does not allow it");
  }
#ifdef CHECK_UNHANDLED_OOPS
  // Clear unhandled oops in JavaThreads so we get a crash right away.
  clear_unhandled_oops();
#endif // CHECK_UNHANDLED_OOPS

  // Macos/aarch64 should be in the right state for safepoint (e.g.
  // deoptimization needs WXWrite).  Crashes caused by the wrong state rarely
  // happens in practice, making such issues hard to find and reproduce.
#if defined(__APPLE__) && defined(AARCH64)
  if (AssertWXAtThreadSync) {
    assert_wx_state(WXWrite);
  }
#endif
}

void JavaThread::check_for_valid_safepoint_state() {
  // Don't complain if running a debugging command.
  if (DebuggingContext::is_enabled()) return;

  // Check NoSafepointVerifier, which is implied by locks taken that can be
  // shared with the VM thread.  This makes sure that no locks with allow_vm_block
  // are held.
  check_possible_safepoint();

  if (thread_state() != _thread_in_vm) {
    fatal("LEAF method calling lock?");
  }

  if (GCALotAtAllSafepoints) {
    // We could enter a safepoint here and thus have a gc
    InterfaceSupport::check_gc_alot();
  }
}
#endif // ASSERT

// A JavaThread is a normal Java thread

JavaThread::JavaThread() :
  // Initialize fields

  _on_thread_list(false),
  DEBUG_ONLY(_java_call_counter(0) COMMA)
  _entry_point(nullptr),
  _deopt_mark(nullptr),
  _deopt_nmethod(nullptr),
  _vframe_array_head(nullptr),
  _vframe_array_last(nullptr),
  _jvmti_deferred_updates(nullptr),
  _callee_target(nullptr),
  _vm_result(nullptr),
  _vm_result_2(nullptr),

  _current_pending_monitor(nullptr),
  _current_pending_monitor_is_from_java(true),
  _current_waiting_monitor(nullptr),
  _active_handles(nullptr),
  _free_handle_block(nullptr),
  _Stalled(0),

  _monitor_chunks(nullptr),

  _suspend_flags(0),

  _thread_state(_thread_new),
  _saved_exception_pc(nullptr),
#ifdef ASSERT
  _no_safepoint_count(0),
  _visited_for_critical_count(false),
#endif

  _terminated(_not_terminated),
  _in_deopt_handler(0),
  _doing_unsafe_access(false),
  _do_not_unlock_if_synchronized(false),
#if INCLUDE_JVMTI
  _carrier_thread_suspended(false),
  _is_in_VTMS_transition(false),
  _is_in_tmp_VTMS_transition(false),
#ifdef ASSERT
  _is_VTMS_transition_disabler(false),
#endif
#endif
  _jni_attach_state(_not_attaching_via_jni),
#if INCLUDE_JVMCI
  _pending_deoptimization(-1),
  _pending_monitorenter(false),
  _pending_transfer_to_interpreter(false),
  _in_retryable_allocation(false),
  _pending_failed_speculation(0),
  _jvmci{nullptr},
  _libjvmci_runtime(nullptr),
  _jvmci_counters(nullptr),
  _jvmci_reserved0(0),
  _jvmci_reserved1(0),
  _jvmci_reserved_oop0(nullptr),
#endif // INCLUDE_JVMCI

  _exception_oop(oop()),
  _exception_pc(0),
  _exception_handler_pc(0),
  _is_method_handle_return(0),

  _jni_active_critical(0),
  _pending_jni_exception_check_fn(nullptr),
  _depth_first_number(0),

  // JVMTI PopFrame support
  _popframe_condition(popframe_inactive),
  _frames_to_pop_failed_realloc(0),

  _cont_entry(nullptr),
  _cont_fastpath(0),
  _cont_fastpath_thread_state(1),
  _held_monitor_count(0),
  _jni_monitor_count(0),

  _handshake(this),

  _popframe_preserved_args(nullptr),
  _popframe_preserved_args_size(0),

  _jvmti_thread_state(nullptr),
  _interp_only_mode(0),
  _should_post_on_exceptions_flag(JNI_FALSE),
  _thread_stat(new ThreadStatistics()),

  _parker(),

  _class_to_be_initialized(nullptr),

  _SleepEvent(ParkEvent::Allocate(this)),

  _lock_stack(this) {
  set_jni_functions(jni_functions());

#if INCLUDE_JVMCI
  assert(_jvmci._implicit_exception_pc == nullptr, "must be");
  if (JVMCICounterSize > 0) {
    resize_counters(0, (int) JVMCICounterSize);
  }
#endif // INCLUDE_JVMCI

  // Setup safepoint state info for this thread
  ThreadSafepointState::create(this);

  SafepointMechanism::initialize_header(this);

  set_requires_cross_modify_fence(false);

  pd_initialize();
  assert(deferred_card_mark().is_empty(), "Default MemRegion ctor");
}

JavaThread::JavaThread(bool is_attaching_via_jni) : JavaThread() {
  if (is_attaching_via_jni) {
    _jni_attach_state = _attaching_via_jni;
  }
}


// interrupt support
// 确保调用线程的指针没有被丢弃，即线程仍然有效。
// 对于Windows平台，设置线程的中断状态为true。
// 对于Thread.sleep，JSR166 LockSupport.park，ObjectMonitor和JvmtiRawMonitor，唤醒线程。
void JavaThread::interrupt() {
  // 确保调用线程的指针没有被丢弃，即线程仍然有效
  debug_only(check_for_dangling_thread_pointer(this););
  // 对于Windows平台，设置线程的中断状态为true
  WINDOWS_ONLY(osthread()->set_interrupted(true););
  // 对于Thread.sleep，唤醒线程
  _SleepEvent->unpark();
  // 对于JSR166 LockSupport.park，唤醒线程
  parker()->unpark();
  // 对于ObjectMonitor和JvmtiRawMonitor，唤醒线程
  _ParkEvent->unpark();
}

// 确保调用线程的指针没有被丢弃，即线程仍然有效。
// 如果线程对象为null，则不可能被中断，返回false。
// 检查线程是否被中断。
// 如果需要清除中断状态，并且线程确实被中断了，设置线程的中断状态为false。
// 返回线程的中断状态。
bool JavaThread::is_interrupted(bool clear_interrupted) {
  // 确保调用线程的指针没有被丢弃，即线程仍然有效
  debug_only(check_for_dangling_thread_pointer(this););
  // 如果线程对象为null，则不可能被中断
  if (_threadObj.peek() == nullptr) {
    // 断言当前线程是调用线程
    assert(this == Thread::current(), "invariant");
    return false;
  }

  // 检查线程是否被中断
  bool interrupted = java_lang_Thread::interrupted(threadObj());

  // 如果需要清除中断状态，并且线程确实被中断了
  if (interrupted && clear_interrupted) {
    // 断言只有当前线程才能清除中断状态
    assert(this == Thread::current(), "only the current thread can clear");
    // 设置线程的中断状态为false
    java_lang_Thread::set_interrupted(threadObj(), false);
    // 对于Windows平台，清除线程的中断状态
    WINDOWS_ONLY(osthread()->set_interrupted(false););
  }

  // 返回线程的中断状态
  return interrupted;
}

// 检查线程是否已经终止并且虚拟机已经退出。
// 如果是，则设置线程状态为在虚拟机中，并尝试获取线程锁。
// 通常情况下，这应该不会被执行到，因为这是一个不应该发生的状态。
void JavaThread::block_if_vm_exited() {
  if (_terminated == _vm_exited) {
    // 如果线程已经终止并且虚拟机已经退出，则永久阻塞
    // _vm_exited在安全点上设置，并且Threads_lock永远不会释放，所以我们将永远阻塞在这里。
    // 在这里，我们可以从安全状态跳转到不安全状态而没有适当的过渡，但它发生在最终安全点开始之后，
    // 所以这个跳跃不会导致任何安全点问题。
    set_thread_state(_thread_in_vm);
    Threads_lock->lock();
    ShouldNotReachHere(); // 这应该不会被执行到
  }
}

// 设置JNI附加状态为非JNI附加。
// 设置线程的入口点。
// 根据入口点类型设置线程类型。
// 创建本地线程。
// 检查是否由于内存不足而导致线程创建失败。
// 线程创建后，它仍然是被挂起的，需要由创建者显式启动，并添加到线程列表中。
JavaThread::JavaThread(ThreadFunction entry_point, size_t stack_sz) : JavaThread() {
  _jni_attach_state = _not_attaching_via_jni;
  set_entry_point(entry_point);
  // 创建本地线程本身。
  // %note runtime_23
  os::ThreadType thr_type = os::java_thread;
  // 如果是编译器线程，则设置线程类型为编译器线程
  thr_type = entry_point == &CompilerThread::thread_entry ? os::compiler_thread : os::java_thread;
  os::create_thread(this, thr_type, stack_sz);
  // _osthread可能在这里是null，因为可能内存不足（太多活跃线程）。
  // 我们需要抛出一个OutOfMemoryError，但是我们不能在这里这样做，因为调用者可能持有一个锁，
  // 在抛出异常之前所有锁必须被释放（抛出异常包括创建异常对象&初始化它，初始化将离开VM通过JavaCall，
  // 然后所有锁必须被解锁）。
  //
  // 当我们到达这里时，线程仍然是被挂起的。线程必须由创建者显式启动！
  // 此外，线程还必须通过调用Threads:add显式添加到线程列表中。
  // 这里的原因是因为线程对象必须完全初始化（查看JVM_Start）
}

// JavaThread类的析构函数。析构函数在对象被销毁时自动调用，用于清理对象所占用的资源
JavaThread::~JavaThread() {
  // 将OopHandles（即Java对象句柄）加入释放队列，由服务线程释放
  add_oop_handles_for_release();

  // 将睡眠事件返回到空闲列表，以供后续重用
  ParkEvent::Release(_SleepEvent);
  _SleepEvent = nullptr;

  // 释放任何剩余的之前的UnrollBlock
  vframeArray* old_array = vframe_array_last();

  if (old_array != nullptr) {
    Deoptimization::UnrollBlock* old_info = old_array->unroll_block();
    old_array->set_unroll_block(nullptr);
    delete old_info;
    delete old_array;
  }

  // 获取延迟更新对象
  JvmtiDeferredUpdates* updates = deferred_updates();
  if (updates != nullptr) {
    // 如果线程在去优化之前被销毁，这将发生
    assert(updates->count() > 0, "Updates holder not deleted");
    // 释放延迟更新
    delete updates;
    set_deferred_updates(nullptr);
  }

  // 所有与Java相关的清理工作在exit中发生
  ThreadSafepointState::destroy(this);
  if (_thread_stat != nullptr) delete _thread_stat;

  // 如果包括JVMCI支持，释放JVMCI计数器数组
#if INCLUDE_JVMCI
  if (JVMCICounterSize > 0) {
    FREE_C_HEAP_ARRAY(jlong, _jvmci_counters);
  }
#endif // INCLUDE_JVMCI
}

// First JavaThread specific code executed by a new Java thread.
void JavaThread::pre_run() {
  // empty - see comments in run()
}

// The main routine called by a new Java thread. This isn't overridden
// by subclasses, instead different subclasses define a different "entry_point"
// which defines the actual logic for that kind of thread.
void JavaThread::run() {
  // 初始化与线程本地分配缓冲区相关的字段
  initialize_tlab();

  // 创建栈溢出保护页
  _stack_overflow_state.create_stack_guard_pages();

  // 缓存全局变量
  cache_global_variables();

  // 线程现在已足够初始化，可以被安全点代码处理为在虚拟机中。
  // 将线程状态从_thread_new更改为_thread_in_vm
  assert(this->thread_state() == _thread_new, "wrong thread state");
  set_thread_state(_thread_in_vm);

  // 在线程进入线程列表之前，它始终是安全的，因此在离开_thread_new之后，我们应该发出一个指令屏障。
  // 从这里到修改过的代码的距离可能足够远，但这是一致的且安全的。
  OrderAccess::cross_modify_fence();

  // 确保当前线程是this，并且不持有任何锁
  assert(JavaThread::current() == this, "sanity check");
  assert(!Thread::current()->owns_locks(), "sanity check");

  // DTRACE是一个用于调试和性能监控的工具，这里是一个探针，用于标记线程的开始
  DTRACE_THREAD_PROBE(start, this);

  // 这个操作可能会阻塞。我们在完成所有新线程的安全点检查之后调用它。
  set_active_handles(JNIHandleBlock::allocate_block());

  // 如果JvmtiExport指示应该发布线程生命周期事件，则发布线程开始事件
  if (JvmtiExport::should_post_thread_life()) {
    JvmtiExport::post_thread_start(this);
  }

  // 如果设置了AlwaysPreTouchStacks，则预触摸堆栈
  if (AlwaysPreTouchStacks) {
    pretouch_stack();
  }

  // 我们调用另一个函数来完成剩余的工作，以确保从那里使用的堆栈地址将低于刚刚计算的堆栈基址。
  thread_main_inner();
}

void JavaThread::thread_main_inner() {
  assert(JavaThread::current() == this, "sanity check");
  assert(_threadObj.peek() != nullptr, "just checking");

  // 除非该线程有一个待定的异常，否则执行线程入口点
  // 注意：由于JVMTI StopThread，我们可能已经有了待定的异常！
  if (!this->has_pending_exception()) {
    {
      ResourceMark rm(this);
      this->set_native_thread_name(this->name());
    }
    HandleMark hm(this);
    this->entry_point()(this, this);
  }

  DTRACE_THREAD_PROBE(stop, this);

  // 清理工作在post_run()中处理
}

// Shared teardown for all JavaThreads
void JavaThread::post_run() {
  this->exit(false);
  this->unregister_thread_stack_with_NMT();
  // 延迟删除到此处，以确保在call_run中的任何共享清理中'this'仍然可引用
  this->smr_delete();
}

static void ensure_join(JavaThread* thread) {
  // 无需获取Threads_lock，因为我们正在操作我们自己。
  Handle threadObj(thread, thread->threadObj());
  assert(threadObj.not_null(), "java thread object must exist");
  ObjectLocker lock(threadObj, thread);
  // 线程正在退出。因此，在java.lang.Thread类中设置thread_status字段为TERMINATED。
  java_lang_Thread::set_thread_status(threadObj(), JavaThreadStatus::TERMINATED);
  // 清除本地线程实例 - 这使得isAlive返回false，并允许join()在下面的notify_all完成。
  // 需要遵守Java内存模型要求的一个释放()。
  java_lang_Thread::release_set_thread(threadObj(), nullptr);
  lock.notify_all(thread);
  // 忽略待定的异常，因为我们无论如何都要退出
  thread->clear_pending_exception();
}

static bool is_daemon(oop threadObj) {
  return (threadObj != nullptr && java_lang_Thread::is_daemon(threadObj));
}

// For any new cleanup additions, please check to see if they need to be applied to
// cleanup_failed_attach_current_thread as well.
void JavaThread::exit(bool destroy_vm, ExitType exit_type) {
  assert(this == JavaThread::current(), "thread consistency check");
  assert(!is_exiting(), "should not be exiting or terminated already");

  // 初始化退出阶段的计时器
  elapsedTimer _timer_exit_phase1;
  elapsedTimer _timer_exit_phase2;
  elapsedTimer _timer_exit_phase3;
  elapsedTimer _timer_exit_phase4;

  if (log_is_enabled(Debug, os, thread, timer)) {
    _timer_exit_phase1.start();
  }

  // 创建处理标记，用于处理异常
  HandleMark hm(this);
  // 获取待定的异常
  Handle uncaught_exception(this, this->pending_exception());
  // 清除待定的异常
  this->clear_pending_exception();
  // 获取线程对象句柄
  Handle threadObj(this, this->threadObj());
  // 确保线程对象不为空
  assert(threadObj.not_null(), "Java thread object should be created");

  if (!destroy_vm) {
    if (uncaught_exception.not_null()) {
      EXCEPTION_MARK;
      // 如果有未捕获的异常，则调用Thread.dispatchUncaughtException()
      Klass* thread_klass = vmClasses::Thread_klass();
      JavaValue result(T_VOID);
      JavaCalls::call_virtual(&result,
                              threadObj, thread_klass,
                              vmSymbols::dispatchUncaughtException_name(),
                              vmSymbols::throwable_void_signature(),
                              uncaught_exception,
                              THREAD);
      // 如果调用后仍有待定的异常，则处理异常
      if (HAS_PENDING_EXCEPTION) {
        ResourceMark rm(this);
        jio_fprintf(defaultStream::error_stream(),
                    "\nException: %s thrown from the UncaughtExceptionHandler"
                    " in thread \"%s\"\n",
                    pending_exception()->klass()->external_name(),
                    name());
        CLEAR_PENDING_EXCEPTION;
      }
    }

    if (!is_Compiler_thread()) {
      // 如果是用户定义的Java代码执行完毕，调用Thread.exit()进行特定清理
      NoAsyncExceptionDeliveryMark _no_async(this);

      EXCEPTION_MARK;
      JavaValue result(T_VOID);
      Klass* thread_klass = vmClasses::Thread_klass();
      JavaCalls::call_virtual(&result,
                              threadObj, thread_klass,
                              vmSymbols::exit_method_name(),
                              vmSymbols::void_method_signature(),
                              THREAD);
      CLEAR_PENDING_EXCEPTION;
    }

    // 通知JVMTI线程生命周期事件
    if (JvmtiExport::should_post_thread_life()) {
      JvmtiExport::post_thread_end(this);
    }
  } else {
    // 如果destroy_vm为true，则跳过上述步骤
  }

  // 清理任何待定的异步异常
  if (has_async_exception_condition()) {
    handshake_state()->clean_async_exception_operation();
  }

  // 设置线程状态为_thread_exiting，表示正在退出
  set_terminated(_thread_exiting);
  ThreadService::current_thread_exiting(this, is_daemon(threadObj()));

  if (log_is_enabled(Debug, os, thread, timer)) {
    _timer_exit_phase1.stop();
    _timer_exit_phase2.start();
  }

  // 在线程被标记为终止之前，获取守护线程状态
  bool daemon = is_daemon(threadObj());

  // Notify waiters on thread object. This has to be done after exit() is called
  // on the thread (if the thread is the last thread in a daemon ThreadGroup the
  // group should have the destroyed bit set before waiters are notified).
  // 确保线程对象上的等待线程得到通知
  ensure_join(this);
  // 断言确保_ensure_join方法已经清除待定的异常
  assert(!this->has_pending_exception(), "ensure_join should have cleared");

  if (log_is_enabled(Debug, os, thread, timer)) {
    _timer_exit_phase2.stop();
    _timer_exit_phase3.start();
  }

  // 确保线程释放所有持有的Java监视器
  if (exit_type == jni_detach) {
    // 确保在退出时没有Java帧，否则JNI DetachCurrentThread将返回错误
    assert(!this->has_last_Java_frame(),
          "should not have a Java frame when detaching or exiting");
    ObjectSynchronizer::release_monitors_owned_by_thread(this);
    // 断言确保释放监视器后没有待定的异常
    assert(!this->has_pending_exception(), "release_monitors should have cleared");
  }

  // 确保监视器计数正确，JNI监视器计数与普通监视器计数应该相等
  assert(this->held_monitor_count() == this->jni_monitor_count(),
        "held monitor count should be equal to jni: " INT64_FORMAT " != " INT64_FORMAT,
        (int64_t)this->held_monitor_count(), (int64_t)this->jni_monitor_count());
  // 如果JNI监视器计数大于0，则记录调试信息，表明线程在退出时仍然持有JNI监视器
  if (CheckJNICalls && this->jni_monitor_count() > 0) {
    log_debug(jni)("JavaThread %s (tid: " UINTX_FORMAT ") with Objects still locked by JNI MonitorEnter.",
      exit_type == JavaThread::normal_exit ? "exiting" : "detaching", os::current_thread_id());
  }

  // 在GC发生时确保线程处于一致状态
  JFR_ONLY(Jfr::on_thread_exit(this);)

  // 如果有活动句柄，则释放它们
  if (active_handles() != nullptr) {
    JNIHandleBlock* block = active_handles();
    set_active_handles(nullptr);
    JNIHandleBlock::release_block(block);
  }

  // 如果有空闲句柄块，则释放它们
  if (free_handle_block() != nullptr) {
    JNIHandleBlock* block = free_handle_block();
    set_free_handle_block(nullptr);
    JNIHandleBlock::release_block(block);
  }

  // 删除栈保护页
  _stack_overflow_state.remove_stack_guard_pages();

  // 如果使用了TLAB，则退役TLAB
  if (UseTLAB) {
    tlab().retire();
  }

  // 如果JVMTI环境可能存在，则清理线程
  if (JvmtiEnv::environments_might_exist()) {
    JvmtiExport::cleanup_thread(this);
  }

  // 在调用on_thread_detach后，线程不应再访问任何oops，因此需要缓存线程名称
  char* thread_name = nullptr;
  if (log_is_enabled(Debug, os, thread, timer)) {
    ResourceMark rm(this);
    thread_name = os::strdup(name());
  }

  // 记录线程退出信息
  log_info(os, thread)("JavaThread %s (tid: " UINTX_FORMAT ").",
    exit_type == JavaThread::normal_exit ? "exiting" : "detaching",
    os::current_thread_id());

  if (log_is_enabled(Debug, os, thread, timer)) {
    _timer_exit_phase3.stop();
    _timer_exit_phase4.start();
  }
#if INCLUDE_JVMCI
  // 如果启用了JVMCI，则处理JVMCI计数器
  if (JVMCICounterSize > 0) {
    if (jvmci_counters_include(this)) {
      for (int i = 0; i < JVMCICounterSize; i++) {
        _jvmci_old_thread_counters[i] += _jvmci_counters[i];
      }
    }
  }
#endif // INCLUDE_JVMCI

  // 从活动线程列表中移除当前线程，并通知VM线程如果我们是最后的非守护线程
  Threads::remove(this, daemon);

  if (log_is_enabled(Debug, os, thread, timer)) {
    _timer_exit_phase4.stop();
    log_debug(os, thread, timer)("name='%s'"
                                  ", exit-phase1=" JLONG_FORMAT
                                  ", exit-phase2=" JLONG_FORMAT
                                  ", exit-phase3=" JLONG_FORMAT
                                  ", exit-phase4=" JLONG_FORMAT,
                                  thread_name,
                                  _timer_exit_phase1.milliseconds(),
                                  _timer_exit_phase2.milliseconds(),
                                  _timer_exit_phase3.milliseconds(),
                                  _timer_exit_phase4.milliseconds());
    os::free(thread_name);
  }

}

void JavaThread::cleanup_failed_attach_current_thread(bool is_daemon) {
  // 如果存在活动句柄，则释放它们
  if (active_handles() != nullptr) {
    JNIHandleBlock* block = active_handles();
    set_active_handles(nullptr);
    JNIHandleBlock::release_block(block);
  }

  // 如果存在空闲句柄块，则释放它们
  if (free_handle_block() != nullptr) {
    JNIHandleBlock* block = free_handle_block();
    set_free_handle_block(nullptr);
    JNIHandleBlock::release_block(block);
  }

  // 删除栈保护页
  _stack_overflow_state.remove_stack_guard_pages();

  // 如果使用了TLAB，则退役TLAB
  if (UseTLAB) {
    tlab().retire();
  }

  // 从活动线程列表中移除当前线程，并通知VM线程如果我们是最后的非守护线程
  Threads::remove(this, is_daemon);
  // 删除当前线程
  this->smr_delete();
}

// active方法返回当前活跃的JavaThread对象。如果当前线程是Java线程，则直接返回。如果当前线程是VM线程，则返回调用VM操作的Java线程。
JavaThread* JavaThread::active() {
  Thread* thread = Thread::current();
  if (thread->is_Java_thread()) {
    return JavaThread::cast(thread);
  } else {
    assert(thread->is_VM_thread(), "this must be a vm thread");
    VM_Operation* op = ((VMThread*) thread)->vm_operation();
    JavaThread *ret = op == nullptr ? nullptr : JavaThread::cast(op->calling_thread());
    return ret;
  }
}

// is_lock_owned方法检查当前线程是否拥有指定的地址所表示的锁。它首先检查Thread类中是否拥有锁，然后检查所有监视器块，
// 看看是否有监视器包含指定的地址。如果找到，则返回true，表示线程拥有锁；否则返回false
bool JavaThread::is_lock_owned(address adr) const {
  assert(LockingMode != LM_LIGHTWEIGHT, "should not be called with new lightweight locking");
  if (Thread::is_lock_owned(adr)) return true;

  for (MonitorChunk* chunk = monitor_chunks(); chunk != nullptr; chunk = chunk->next()) {
    if (chunk->contains(adr)) return true;
  }

  return false;
}

oop JavaThread::exception_oop() const {
  return Atomic::load(&_exception_oop);
}

void JavaThread::set_exception_oop(oop o) {
  Atomic::store(&_exception_oop, o);
}

void JavaThread::add_monitor_chunk(MonitorChunk* chunk) {
  chunk->set_next(monitor_chunks());
  set_monitor_chunks(chunk);
}

void JavaThread::remove_monitor_chunk(MonitorChunk* chunk) {
  guarantee(monitor_chunks() != nullptr, "must be non empty");
  if (monitor_chunks() == chunk) {
    set_monitor_chunks(chunk->next());
  } else {
    MonitorChunk* prev = monitor_chunks();
    while (prev->next() != chunk) prev = prev->next();
    prev->set_next(chunk->next());
  }
}

void JavaThread::handle_special_runtime_exit_condition() {
  if (is_obj_deopt_suspend()) {
    frame_anchor()->make_walkable();
    wait_for_object_deoptimization();
  }
  JFR_ONLY(SUSPEND_THREAD_CONDITIONAL(this);)
}


// Asynchronous exceptions support
// 用于处理异步异常。异步异常是指在Java线程执行过程中发生的异常，这些异常通常是由于外部事件（如I/O操作）引起的
void JavaThread::handle_async_exception(oop java_throwable) {
  assert(java_throwable != nullptr, "should have an _async_exception to throw");
  assert(!is_at_poll_safepoint(), "should have never called this method");

  if (has_last_Java_frame()) {
    frame f = last_frame();
    if (f.is_runtime_frame()) {
      // 如果最顶层的帧是运行时桩，那么我们是从编译后的代码中调用OptoRuntime。
      // 一些运行时桩（new, monitor_exit等）在继续执行之前必须去优化调用者，
      // 因为编译后的异常处理表可能无效。
      RegisterMap reg_map(this,
                          RegisterMap::UpdateMap::skip,
                          RegisterMap::ProcessFrames::include,
                          RegisterMap::WalkContinuation::skip);
      frame compiled_frame = f.sender(&reg_map);
      if (!StressCompiledExceptionHandlers && compiled_frame.can_be_deoptimized()) {
        Deoptimization::deoptimize(this, compiled_frame);
      }
    }
  }

  // 我们不能在这里调用Exceptions::_throw()，因为我们不能阻塞
  set_pending_exception(java_throwable, __FILE__, __LINE__);

  clear_scopedValueBindings();

  LogTarget(Info, exceptions) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);
    ls.print("Async. exception installed at runtime exit (" INTPTR_FORMAT ")", p2i(this));
    if (has_last_Java_frame()) {
      frame f = last_frame();
      ls.print(" (pc: " INTPTR_FORMAT " sp: " INTPTR_FORMAT " )", p2i(f.pc()), p2i(f.sp()));
    }
    ls.print_cr(" of type: %s", java_throwable->klass()->external_name());
  }
}

void JavaThread::install_async_exception(AsyncExceptionHandshake* aeh) {
  // 不要对编译器线程或正在退出的线程抛出异步异常
  if (!can_call_java() || is_exiting()) {
    delete aeh;
    return;
  }

  oop exception = aeh->exception();
  Handshake::execute(aeh, this);  // 安装异步握手

  ResourceMark rm;
  if (log_is_enabled(Info, exceptions)) {
    log_info(exceptions)("Pending Async. exception installed of type: %s",
                         InstanceKlass::cast(exception->klass())->external_name());
  }
  // 为了AbortVMOnException标志
  Exceptions::debug_check_abort(exception->klass()->external_name());

  oop vt_oop = vthread();
  if (vt_oop == nullptr || !vt_oop->is_a(vmClasses::BaseVirtualThread_klass())) {
    // 中断线程，以便它从可能的等待()/睡眠()/park()中唤醒
    java_lang_Thread::set_interrupted(threadObj(), true);
    this->interrupt();
  }
}

class InstallAsyncExceptionHandshake : public HandshakeClosure {
  AsyncExceptionHandshake* _aeh;
public:
  InstallAsyncExceptionHandshake(AsyncExceptionHandshake* aeh) :
    HandshakeClosure("InstallAsyncException"), _aeh(aeh) {}
  ~InstallAsyncExceptionHandshake() {
    // If InstallAsyncExceptionHandshake was never executed we need to clean up _aeh.
    delete _aeh;
  }
  void do_thread(Thread* thr) {
    JavaThread* target = JavaThread::cast(thr);
    target->install_async_exception(_aeh);
    _aeh = nullptr;
  }
};

void JavaThread::send_async_exception(JavaThread* target, oop java_throwable) {
  OopHandle e(Universe::vm_global(), java_throwable);
  InstallAsyncExceptionHandshake iaeh(new AsyncExceptionHandshake(e));
  Handshake::execute(&iaeh, target);
}

#if INCLUDE_JVMTI
void JavaThread::set_is_in_VTMS_transition(bool val) {
  _is_in_VTMS_transition = val;
}

#ifdef ASSERT
void JavaThread::set_is_VTMS_transition_disabler(bool val) {
  _is_VTMS_transition_disabler = val;
}
#endif
#endif

// External suspension mechanism.
//
// Guarantees on return (for a valid target thread):
//   - Target thread will not execute any new bytecode.
//   - Target thread will not enter any new monitors.
//
bool JavaThread::java_suspend() {
#if INCLUDE_JVMTI
  // Suspending a JavaThread in VTMS transition or disabling VTMS transitions can cause deadlocks.
  assert(!is_in_VTMS_transition(), "no suspend allowed in VTMS transition");
  assert(!is_VTMS_transition_disabler(), "no suspend allowed for VTMS transition disablers");
#endif

  guarantee(Thread::is_JavaThread_protected(/* target */ this),
            "target JavaThread is not protected in calling context.");
  return this->handshake_state()->suspend();
}

bool JavaThread::java_resume() {
  guarantee(Thread::is_JavaThread_protected_by_TLH(/* target */ this),
            "missing ThreadsListHandle in calling context.");
  return this->handshake_state()->resume();
}

// Wait for another thread to perform object reallocation and relocking on behalf of
// this thread. The current thread is required to change to _thread_blocked in order
// to be seen to be safepoint/handshake safe whilst suspended and only after becoming
// handshake safe, the other thread can complete the handshake used to synchronize
// with this thread and then perform the reallocation and relocking.
// See EscapeBarrier::sync_and_suspend_*()
// JVM在执行Java代码时，可能会发现某些对象已经不再需要，从而释放这些对象的内存。
// 这个过程可能需要一些时间，特别是在堆内存很大的情况下。wait_for_object_deoptimization方法允许线程在去优化过程中等待，直到去优化完成
void JavaThread::wait_for_object_deoptimization() {
  assert(!has_last_Java_frame() || frame_anchor()->walkable(), "should have walkable stack");
  assert(this == Thread::current(), "invariant");

  bool spin_wait = os::is_MP();
  do {
    ThreadBlockInVM tbivm(this, true /* allow_suspend */);
    // 如果请求了等待对象去优化，则等待。
    if (spin_wait) {
      // 单个去优化通常非常短暂。微基准测试显示，当自旋时性能提高了5%。
      const uint spin_limit = 10 * SpinYield::default_spin_limit;
      SpinYield spin(spin_limit);
      for (uint i = 0; is_obj_deopt_suspend() && i < spin_limit; i++) {
        spin.wait();
      }
      // 只自旋一次
      spin_wait = false;
    } else {
      MonitorLocker ml(this, EscapeBarrier_lock, Monitor::_no_safepoint_check_flag);
      if (is_obj_deopt_suspend()) {
        ml.wait();
      }
    }
    // 在处理握手后，必须再次检查。
  } while (is_obj_deopt_suspend());
}

#ifdef ASSERT
// Verify the JavaThread has not yet been published in the Threads::list, and
// hence doesn't need protection from concurrent access at this stage.
void JavaThread::verify_not_published() {
  // Cannot create a ThreadsListHandle here and check !tlh.includes(this)
  // since an unpublished JavaThread doesn't participate in the
  // Thread-SMR protocol for keeping a ThreadsList alive.
  assert(!on_thread_list(), "JavaThread shouldn't have been published yet!");
}
#endif

// Slow path when the native==>Java barriers detect a safepoint/handshake is
// pending, when _suspend_flags is non-zero or when we need to process a stack
// watermark. Also check for pending async exceptions (except unsafe access error).
// Note only the native==>Java barriers can call this function when thread state
// is _thread_in_native_trans.
void JavaThread::check_special_condition_for_native_trans(JavaThread *thread) {
  assert(thread->thread_state() == _thread_in_native_trans, "wrong state");
  assert(!thread->has_last_Java_frame() || thread->frame_anchor()->walkable(), "Unwalkable stack in native->Java transition");

  thread->set_thread_state(_thread_in_vm);

  // Enable WXWrite: called directly from interpreter native wrapper.
  MACOS_AARCH64_ONLY(ThreadWXEnable wx(WXWrite, thread));

  SafepointMechanism::process_if_requested_with_exit_check(thread, true /* check asyncs */);

  // After returning from native, it could be that the stack frames are not
  // yet safe to use. We catch such situations in the subsequent stack watermark
  // barrier, which will trap unsafe stack frames.
  StackWatermarkSet::before_unwind(thread);
}

#ifndef PRODUCT
// Deoptimization
// Function for testing deoptimization
// 用于去优化Java线程的执行栈。去优化是一种优化技术，用于撤销之前的优化，通常在某些条件发生改变时进行。
// 例如，如果一个对象被提前释放，与其相关的优化可能不再有效，因此需要去优化。
void JavaThread::deoptimize() {
  StackFrameStream fst(this, false /* update */, true /* process_frames */);
  bool deopt = false;           // 只有在实际发生去优化时才转储堆栈。
  bool only_at = strlen(DeoptimizeOnlyAt) > 0;
  // 遍历线程中的所有帧并去优化
  for (; !fst.is_done(); fst.next()) {
    if (fst.current()->can_be_deoptimized()) {

      if (only_at) {
        // 仅在特定的bci处去优化。DeoptimizeOnlyAt由逗号或换行符分隔的数字组成，
        // 因此搜索当前bci是否在该字符串中。
        address pc = fst.current()->pc();
        nmethod* nm = (nmethod*) fst.current()->cb();
        ScopeDesc* sd = nm->scope_desc_at(pc);
        char buffer[8];
        jio_snprintf(buffer, sizeof(buffer), "%d", sd->bci());
        size_t len = strlen(buffer);
        const char * found = strstr(DeoptimizeOnlyAt, buffer);
        while (found != nullptr) {
          if ((found[len] == ',' || found[len] == '\n' || found[len] == '\0') &&
              (found == DeoptimizeOnlyAt || found[-1] == ',' || found[-1] == '\n')) {
            // 检查找到的bci是否由终结符括起来。
            break;
          }
          found = strstr(found + 1, buffer);
        }
        if (!found) {
          continue;
        }
      }

      if (DebugDeoptimization && !deopt) {
        deopt = true; // 仅在去优化前打印一次
        tty->print_cr("[BEFORE Deoptimization]");
        trace_frames();
        trace_stack();
      }
      Deoptimization::deoptimize(this, *fst.current());
    }
  }

  if (DebugDeoptimization && deopt) {
    tty->print_cr("[AFTER Deoptimization]");
    trace_frames();
  }
}


// Make zombies
void JavaThread::make_zombies() {
  for (StackFrameStream fst(this, true /* update */, true /* process_frames */); !fst.is_done(); fst.next()) {
    if (fst.current()->can_be_deoptimized()) {
      // it is a Java nmethod
      nmethod* nm = CodeCache::find_nmethod(fst.current()->pc());
      nm->make_not_entrant();
    }
  }
}
#endif // PRODUCT


void JavaThread::deoptimize_marked_methods() {
  if (!has_last_Java_frame()) return;
  StackFrameStream fst(this, false /* update */, true /* process_frames */);
  for (; !fst.is_done(); fst.next()) {
    if (fst.current()->should_be_deoptimized()) {
      Deoptimization::deoptimize(this, *fst.current());
    }
  }
}

#ifdef ASSERT
void JavaThread::verify_frame_info() {
  assert((!has_last_Java_frame() && java_call_counter() == 0) ||
         (has_last_Java_frame() && java_call_counter() > 0),
         "unexpected frame info: has_last_frame=%s, java_call_counter=%d",
         has_last_Java_frame() ? "true" : "false", java_call_counter());
}
#endif

// Push on a new block of JNI handles.
void JavaThread::push_jni_handle_block() {
  // Allocate a new block for JNI handles.
  // Inlined code from jni_PushLocalFrame()
  JNIHandleBlock* old_handles = active_handles();
  JNIHandleBlock* new_handles = JNIHandleBlock::allocate_block(this);
  assert(old_handles != nullptr && new_handles != nullptr, "should not be null");
  new_handles->set_pop_frame_link(old_handles);  // make sure java handles get gc'd.
  set_active_handles(new_handles);
}

// Pop off the current block of JNI handles.
void JavaThread::pop_jni_handle_block() {
  // Release our JNI handle block
  JNIHandleBlock* old_handles = active_handles();
  JNIHandleBlock* new_handles = old_handles->pop_frame_link();
  assert(new_handles != nullptr, "should never set active handles to null");
  set_active_handles(new_handles);
  old_handles->set_pop_frame_link(nullptr);
  JNIHandleBlock::release_block(old_handles, this);
}

void JavaThread::oops_do_no_frames(OopClosure* f, CodeBlobClosure* cf) {
  // Verify that the deferred card marks have been flushed.
  assert(deferred_card_mark().is_empty(), "Should be empty during GC");

  // Traverse the GCHandles
  Thread::oops_do_no_frames(f, cf);

  if (active_handles() != nullptr) {
    active_handles()->oops_do(f);
  }

  DEBUG_ONLY(verify_frame_info();)

  if (has_last_Java_frame()) {
    // Traverse the monitor chunks
    for (MonitorChunk* chunk = monitor_chunks(); chunk != nullptr; chunk = chunk->next()) {
      chunk->oops_do(f);
    }
  }

  assert(vframe_array_head() == nullptr, "deopt in progress at a safepoint!");
  // If we have deferred set_locals there might be oops waiting to be
  // written
  GrowableArray<jvmtiDeferredLocalVariableSet*>* list = JvmtiDeferredUpdates::deferred_locals(this);
  if (list != nullptr) {
    for (int i = 0; i < list->length(); i++) {
      list->at(i)->oops_do(f);
    }
  }

  // Traverse instance variables at the end since the GC may be moving things
  // around using this function
  f->do_oop((oop*) &_vm_result);
  f->do_oop((oop*) &_exception_oop);
#if INCLUDE_JVMCI
  f->do_oop((oop*) &_jvmci_reserved_oop0);
#endif

  if (jvmti_thread_state() != nullptr) {
    jvmti_thread_state()->oops_do(f, cf);
  }

  // The continuation oops are really on the stack. But there is typically at most
  // one of those per thread, so we handle them here in the oops_do_no_frames part
  // so that we don't have to sprinkle as many stack watermark checks where these
  // oops are used. We just need to make sure the thread has started processing.
  ContinuationEntry* entry = _cont_entry;
  while (entry != nullptr) {
    f->do_oop((oop*)entry->cont_addr());
    f->do_oop((oop*)entry->chunk_addr());
    entry = entry->parent();
  }

  if (LockingMode == LM_LIGHTWEIGHT) {
    lock_stack().oops_do(f);
  }
}

void JavaThread::oops_do_frames(OopClosure* f, CodeBlobClosure* cf) {
  if (!has_last_Java_frame()) {
    return;
  }
  // Finish any pending lazy GC activity for the frames
  StackWatermarkSet::finish_processing(this, nullptr /* context */, StackWatermarkKind::gc);
  // Traverse the execution stack
  for (StackFrameStream fst(this, true /* update */, false /* process_frames */); !fst.is_done(); fst.next()) {
    fst.current()->oops_do(f, cf, fst.register_map());
  }
}

#ifdef ASSERT
void JavaThread::verify_states_for_handshake() {
  // This checks that the thread has a correct frame state during a handshake.
  verify_frame_info();
}
#endif

void JavaThread::nmethods_do(CodeBlobClosure* cf) {
  DEBUG_ONLY(verify_frame_info();)
  MACOS_AARCH64_ONLY(ThreadWXEnable wx(WXWrite, Thread::current());)

  if (has_last_Java_frame()) {
    // Traverse the execution stack
    for (StackFrameStream fst(this, true /* update */, true /* process_frames */); !fst.is_done(); fst.next()) {
      fst.current()->nmethods_do(cf);
    }
  }

  if (jvmti_thread_state() != nullptr) {
    jvmti_thread_state()->nmethods_do(cf);
  }
}

void JavaThread::metadata_do(MetadataClosure* f) {
  if (has_last_Java_frame()) {
    // Traverse the execution stack to call f() on the methods in the stack
    for (StackFrameStream fst(this, true /* update */, true /* process_frames */); !fst.is_done(); fst.next()) {
      fst.current()->metadata_do(f);
    }
  } else if (is_Compiler_thread()) {
    // need to walk ciMetadata in current compile tasks to keep alive.
    CompilerThread* ct = (CompilerThread*)this;
    if (ct->env() != nullptr) {
      ct->env()->metadata_do(f);
    }
    CompileTask* task = ct->task();
    if (task != nullptr) {
      task->metadata_do(f);
    }
  }
}

// Printing
const char* _get_thread_state_name(JavaThreadState _thread_state) {
  switch (_thread_state) {
  case _thread_uninitialized:     return "_thread_uninitialized";
  case _thread_new:               return "_thread_new";
  case _thread_new_trans:         return "_thread_new_trans";
  case _thread_in_native:         return "_thread_in_native";
  case _thread_in_native_trans:   return "_thread_in_native_trans";
  case _thread_in_vm:             return "_thread_in_vm";
  case _thread_in_vm_trans:       return "_thread_in_vm_trans";
  case _thread_in_Java:           return "_thread_in_Java";
  case _thread_in_Java_trans:     return "_thread_in_Java_trans";
  case _thread_blocked:           return "_thread_blocked";
  case _thread_blocked_trans:     return "_thread_blocked_trans";
  default:                        return "unknown thread state";
  }
}

void JavaThread::print_thread_state_on(outputStream *st) const {
  st->print_cr("   JavaThread state: %s", _get_thread_state_name(_thread_state));
}

// Called by Threads::print() for VM_PrintThreads operation
void JavaThread::print_on(outputStream *st, bool print_extended_info) const {
  st->print_raw("\"");
  st->print_raw(name());
  st->print_raw("\" ");
  oop thread_oop = threadObj();
  if (thread_oop != nullptr) {
    st->print("#" INT64_FORMAT " [%ld] ", (int64_t)java_lang_Thread::thread_id(thread_oop), (long) osthread()->thread_id());
    if (java_lang_Thread::is_daemon(thread_oop))  st->print("daemon ");
    st->print("prio=%d ", java_lang_Thread::priority(thread_oop));
  }
  Thread::print_on(st, print_extended_info);
  // print guess for valid stack memory region (assume 4K pages); helps lock debugging
  st->print_cr("[" INTPTR_FORMAT "]", (intptr_t)last_Java_sp() & ~right_n_bits(12));
  if (thread_oop != nullptr) {
    if (is_vthread_mounted()) {
      oop vt = vthread();
      assert(vt != nullptr, "");
      st->print_cr("   Carrying virtual thread #" INT64_FORMAT, (int64_t)java_lang_Thread::thread_id(vt));
    } else {
      st->print_cr("   java.lang.Thread.State: %s", java_lang_Thread::thread_status_name(thread_oop));
    }
  }
#ifndef PRODUCT
  _safepoint_state->print_on(st);
#endif // PRODUCT
  if (is_Compiler_thread()) {
    CompileTask *task = ((CompilerThread*)this)->task();
    if (task != nullptr) {
      st->print("   Compiling: ");
      task->print(st, nullptr, true, false);
    } else {
      st->print("   No compile task");
    }
    st->cr();
  }
}

void JavaThread::print() const { print_on(tty); }

void JavaThread::print_name_on_error(outputStream* st, char *buf, int buflen) const {
  st->print("%s", get_thread_name_string(buf, buflen));
}

// Called by fatal error handler. The difference between this and
// JavaThread::print() is that we can't grab lock or allocate memory.
void JavaThread::print_on_error(outputStream* st, char *buf, int buflen) const {
  st->print("%s \"%s\"", type_name(), get_thread_name_string(buf, buflen));
  Thread* current = Thread::current_or_null_safe();
  assert(current != nullptr, "cannot be called by a detached thread");
  st->fill_to(60);
  if (!current->is_Java_thread() || JavaThread::cast(current)->is_oop_safe()) {
    // Only access threadObj() if current thread is not a JavaThread
    // or if it is a JavaThread that can safely access oops.
    oop thread_obj = threadObj();
    if (thread_obj != nullptr) {
      st->print(java_lang_Thread::is_daemon(thread_obj) ? " daemon" : "       ");
    }
  }
  st->print(" [");
  st->print("%s", _get_thread_state_name(_thread_state));
  if (osthread()) {
    st->print(", id=%d", osthread()->thread_id());
  }
  st->print(", stack(" PTR_FORMAT "," PTR_FORMAT ") (" PROPERFMT ")",
            p2i(stack_end()), p2i(stack_base()),
            PROPERFMTARGS(stack_size()));
  st->print("]");

  ThreadsSMRSupport::print_info_on(this, st);
  return;
}


// Verification

void JavaThread::frames_do(void f(frame*, const RegisterMap* map)) {
  // ignore if there is no stack
  if (!has_last_Java_frame()) return;
  // traverse the stack frames. Starts from top frame.
  for (StackFrameStream fst(this, true /* update_map */, true /* process_frames */, false /* walk_cont */); !fst.is_done(); fst.next()) {
    frame* fr = fst.current();
    f(fr, fst.register_map());
  }
}

static void frame_verify(frame* f, const RegisterMap *map) { f->verify(map); }

void JavaThread::verify() {
  // Verify oops in the thread.
  oops_do(&VerifyOopClosure::verify_oop, nullptr);

  // Verify the stack frames.
  frames_do(frame_verify);
}

// CR 6300358 (sub-CR 2137150)
// Most callers of this method assume that it can't return null but a
// thread may not have a name whilst it is in the process of attaching to
// the VM - see CR 6412693, and there are places where a JavaThread can be
// seen prior to having its threadObj set (e.g., JNI attaching threads and
// if vm exit occurs during initialization). These cases can all be accounted
// for such that this method never returns null.
const char* JavaThread::name() const  {
  if (Thread::is_JavaThread_protected(/* target */ this)) {
    // The target JavaThread is protected so get_thread_name_string() is safe:
    return get_thread_name_string();
  }

  // The target JavaThread is not protected so we return the default:
  return Thread::name();
}

// Returns a non-null representation of this thread's name, or a suitable
// descriptive string if there is no set name.
const char* JavaThread::get_thread_name_string(char* buf, int buflen) const {
  const char* name_str;
#ifdef ASSERT
  Thread* current = Thread::current_or_null_safe();
  assert(current != nullptr, "cannot be called by a detached thread");
  if (!current->is_Java_thread() || JavaThread::cast(current)->is_oop_safe()) {
    // Only access threadObj() if current thread is not a JavaThread
    // or if it is a JavaThread that can safely access oops.
#endif
    oop thread_obj = threadObj();
    if (thread_obj != nullptr) {
      oop name = java_lang_Thread::name(thread_obj);
      if (name != nullptr) {
        if (buf == nullptr) {
          name_str = java_lang_String::as_utf8_string(name);
        } else {
          name_str = java_lang_String::as_utf8_string(name, buf, buflen);
        }
      } else if (is_attaching_via_jni()) { // workaround for 6412693 - see 6404306
        name_str = "<no-name - thread is attaching>";
      } else {
        name_str = "<un-named>";
      }
    } else {
      name_str = Thread::name();
    }
#ifdef ASSERT
  } else {
    // Current JavaThread has exited...
    if (current == this) {
      // ... and is asking about itself:
      name_str = "<no-name - current JavaThread has exited>";
    } else {
      // ... and it can't safely determine this JavaThread's name so
      // use the default thread name.
      name_str = Thread::name();
    }
  }
#endif
  assert(name_str != nullptr, "unexpected null thread name");
  return name_str;
}

// Helper to extract the name from the thread oop for logging.
const char* JavaThread::name_for(oop thread_obj) {
  assert(thread_obj != nullptr, "precondition");
  oop name = java_lang_Thread::name(thread_obj);
  const char* name_str;
  if (name != nullptr) {
    name_str = java_lang_String::as_utf8_string(name);
  } else {
    name_str = "<un-named>";
  }
  return name_str;
}

void JavaThread::prepare(jobject jni_thread, ThreadPriority prio) {
  assert(Threads_lock->owner() == Thread::current(), "must have threads lock");
  assert(NoPriority <= prio && prio <= MaxPriority, "sanity check");

  // 获取C++线程对象（一个oop）的句柄，并将其放入新的句柄中。
  // 句柄"thread_oop"可以用于将C++线程对象传递给其他方法。
  Handle thread_oop(Thread::current(),
                    JNIHandles::resolve_non_null(jni_thread));
  assert(InstanceKlass::cast(thread_oop->klass())->is_linked(),
         "must be initialized");
  set_threadOopHandles(thread_oop());

  if (prio == NoPriority) {
    prio = java_lang_Thread::priority(thread_oop());
    assert(prio != NoPriority, "A valid priority should be present");
  }

  // 将Java优先级设置为原生线程的优先级；需要Threads_lock
  Thread::set_priority(this, prio);

  // 将新线程添加到线程列表并使其开始运行。
  // 我们必须持有线程锁才能调用Threads::add。
  // 至关重要的是，在调用Threads::add之前，我们不应该阻塞，
  // 因为如果发生GC，那么java_thread oop将不会被GC访问。
  Threads::add(this);
  // 在线程添加到线程列表后发布java.lang.Thread中的JavaThread*。
  // 我们不想在调用者某处释放Threads_lock时等待，
  // 因为JavaThread*已经通过ThreadsList对JVM/TI可见。
  java_lang_Thread::release_set_thread(thread_oop(), this);
}

oop JavaThread::current_park_blocker() {
  // Support for JSR-166 locks
  oop thread_oop = threadObj();
  if (thread_oop != nullptr) {
    return java_lang_Thread::park_blocker(thread_oop);
  }
  return nullptr;
}

// Print current stack trace for checked JNI warnings and JNI fatal errors.
// This is the external format, selecting the platform or vthread
// as applicable, and allowing for a native-only stack.
void JavaThread::print_jni_stack() {
  assert(this == JavaThread::current(), "Can't print stack of other threads");
  if (!has_last_Java_frame()) {
    ResourceMark rm(this);
    char* buf = NEW_RESOURCE_ARRAY_RETURN_NULL(char, O_BUFLEN);
    if (buf == nullptr) {
      tty->print_cr("Unable to print native stack - out of memory");
      return;
    }
    frame f = os::current_frame();
    VMError::print_native_stack(tty, f, this, true /*print_source_info */,
                                -1 /* max stack */, buf, O_BUFLEN);
  } else {
    print_active_stack_on(tty);
  }
}

void JavaThread::print_stack_on(outputStream* st) {
  if (!has_last_Java_frame()) return;

  Thread* current_thread = Thread::current();
  ResourceMark rm(current_thread);
  HandleMark hm(current_thread);

  RegisterMap reg_map(this,
                      RegisterMap::UpdateMap::include,
                      RegisterMap::ProcessFrames::include,
                      RegisterMap::WalkContinuation::skip);
  vframe* start_vf = platform_thread_last_java_vframe(&reg_map);
  int count = 0;
  for (vframe* f = start_vf; f != nullptr; f = f->sender()) {
    if (f->is_java_frame()) {
      javaVFrame* jvf = javaVFrame::cast(f);
      java_lang_Throwable::print_stack_element(st, jvf->method(), jvf->bci());

      // Print out lock information
      if (JavaMonitorsInStackTrace) {
        jvf->print_lock_info_on(st, count);
      }
    } else {
      // Ignore non-Java frames
    }

    // Bail-out case for too deep stacks if MaxJavaStackTraceDepth > 0
    count++;
    if (MaxJavaStackTraceDepth > 0 && MaxJavaStackTraceDepth == count) return;
  }
}

// 打印虚拟线程的栈跟踪信息。虚拟线程是JVM内部用于执行特定任务的线程，它们与常规Java线程不同，通常不显示在Java堆栈跟踪中。
// 这个方法用于调试目的，可以帮助开发者了解虚拟线程的执行情况
void JavaThread::print_vthread_stack_on(outputStream* st) {
  assert(is_vthread_mounted(), "Caller should have checked this");
  assert(has_last_Java_frame(), "must be");

  Thread* current_thread = Thread::current();
  ResourceMark rm(current_thread);
  HandleMark hm(current_thread);

  RegisterMap reg_map(this,
                      RegisterMap::UpdateMap::include,
                      RegisterMap::ProcessFrames::include,
                      RegisterMap::WalkContinuation::include);
  ContinuationEntry* cont_entry = last_continuation();
  vframe* start_vf = last_java_vframe(&reg_map);
  int count = 0;
  for (vframe* f = start_vf; f != nullptr; f = f->sender()) {
    // 检查是否到达虚拟线程栈的末端
    if (Continuation::is_continuation_enterSpecial(f->fr())) {
      assert(cont_entry == Continuation::get_continuation_entry_for_entry_frame(this, f->fr()), "");
      if (cont_entry->is_virtual_thread()) {
        break;
      }
      cont_entry = cont_entry->parent();
    }
    if (f->is_java_frame()) {
      javaVFrame* jvf = javaVFrame::cast(f);
      java_lang_Throwable::print_stack_element(st, jvf->method(), jvf->bci());

      // 打印锁信息
      if (JavaMonitorsInStackTrace) {
        jvf->print_lock_info_on(st, count);
      }
    } else {
      // 忽略非Java帧
    }

    // 对于太深的栈，如果MaxJavaStackTraceDepth > 0，则退出
    count++;
    if (MaxJavaStackTraceDepth > 0 && MaxJavaStackTraceDepth == count) return;
  }
}

void JavaThread::print_active_stack_on(outputStream* st) {
  if (is_vthread_mounted()) {
    print_vthread_stack_on(st);
  } else {
    print_stack_on(st);
  }
}

#if INCLUDE_JVMTI
// Rebind JVMTI thread state from carrier to virtual or from virtual to carrier.
JvmtiThreadState* JavaThread::rebind_to_jvmti_thread_state_of(oop thread_oop) {
  set_jvmti_vthread(thread_oop);

  // unbind current JvmtiThreadState from JavaThread
  JvmtiThreadState::unbind_from(jvmti_thread_state(), this);

  // bind new JvmtiThreadState to JavaThread
  JvmtiThreadState::bind_to(java_lang_Thread::jvmti_thread_state(thread_oop), this);

  return jvmti_thread_state();
}
#endif

// JVMTI PopFrame support
void JavaThread::popframe_preserve_args(ByteSize size_in_bytes, void* start) {
  assert(_popframe_preserved_args == nullptr, "should not wipe out old PopFrame preserved arguments");
  if (in_bytes(size_in_bytes) != 0) {
    _popframe_preserved_args = NEW_C_HEAP_ARRAY(char, in_bytes(size_in_bytes), mtThread);
    _popframe_preserved_args_size = in_bytes(size_in_bytes);
    Copy::conjoint_jbytes(start, _popframe_preserved_args, _popframe_preserved_args_size);
  }
}

void* JavaThread::popframe_preserved_args() {
  return _popframe_preserved_args;
}

ByteSize JavaThread::popframe_preserved_args_size() {
  return in_ByteSize(_popframe_preserved_args_size);
}

WordSize JavaThread::popframe_preserved_args_size_in_words() {
  int sz = in_bytes(popframe_preserved_args_size());
  assert(sz % wordSize == 0, "argument size must be multiple of wordSize");
  return in_WordSize(sz / wordSize);
}

void JavaThread::popframe_free_preserved_args() {
  assert(_popframe_preserved_args != nullptr, "should not free PopFrame preserved arguments twice");
  FREE_C_HEAP_ARRAY(char, (char*)_popframe_preserved_args);
  _popframe_preserved_args = nullptr;
  _popframe_preserved_args_size = 0;
}

#ifndef PRODUCT

void JavaThread::trace_frames() {
  tty->print_cr("[Describe stack]");
  int frame_no = 1;
  for (StackFrameStream fst(this, true /* update */, true /* process_frames */); !fst.is_done(); fst.next()) {
    tty->print("  %d. ", frame_no++);
    fst.current()->print_value_on(tty, this);
    tty->cr();
  }
}

class PrintAndVerifyOopClosure: public OopClosure {
 protected:
  template <class T> inline void do_oop_work(T* p) {
    oop obj = RawAccess<>::oop_load(p);
    if (obj == nullptr) return;
    tty->print(INTPTR_FORMAT ": ", p2i(p));
    if (oopDesc::is_oop_or_null(obj)) {
      if (obj->is_objArray()) {
        tty->print_cr("valid objArray: " INTPTR_FORMAT, p2i(obj));
      } else {
        obj->print();
      }
    } else {
      tty->print_cr("invalid oop: " INTPTR_FORMAT, p2i(obj));
    }
    tty->cr();
  }
 public:
  virtual void do_oop(oop* p) { do_oop_work(p); }
  virtual void do_oop(narrowOop* p)  { do_oop_work(p); }
};

#ifdef ASSERT
// Print or validate the layout of stack frames
void JavaThread::print_frame_layout(int depth, bool validate_only) {
  ResourceMark rm;
  PreserveExceptionMark pm(this);
  FrameValues values;
  int frame_no = 0;
  for (StackFrameStream fst(this, true, true, true); !fst.is_done(); fst.next()) {
    fst.current()->describe(values, ++frame_no, fst.register_map());
    if (depth == frame_no) break;
  }
  Continuation::describe(values);
  if (validate_only) {
    values.validate();
  } else {
    tty->print_cr("[Describe stack layout]");
    values.print(this);
  }
}
#endif

void JavaThread::trace_stack_from(vframe* start_vf) {
  ResourceMark rm;
  int vframe_no = 1;
  for (vframe* f = start_vf; f; f = f->sender()) {
    if (f->is_java_frame()) {
      javaVFrame::cast(f)->print_activation(vframe_no++);
    } else {
      f->print();
    }
    if (vframe_no > StackPrintLimit) {
      tty->print_cr("...<more frames>...");
      return;
    }
  }
}


void JavaThread::trace_stack() {
  if (!has_last_Java_frame()) return;
  Thread* current_thread = Thread::current();
  ResourceMark rm(current_thread);
  HandleMark hm(current_thread);
  RegisterMap reg_map(this,
                      RegisterMap::UpdateMap::include,
                      RegisterMap::ProcessFrames::include,
                      RegisterMap::WalkContinuation::skip);
  trace_stack_from(last_java_vframe(&reg_map));
}


#endif // PRODUCT

void JavaThread::inc_held_monitor_count(int i, bool jni) {
#ifdef SUPPORT_MONITOR_COUNT
  assert(_held_monitor_count >= 0, "Must always be greater than 0: " INT64_FORMAT, (int64_t)_held_monitor_count);
  _held_monitor_count += i;
  if (jni) {
    assert(_jni_monitor_count >= 0, "Must always be greater than 0: " INT64_FORMAT, (int64_t)_jni_monitor_count);
    _jni_monitor_count += i;
  }
#endif
}

void JavaThread::dec_held_monitor_count(int i, bool jni) {
#ifdef SUPPORT_MONITOR_COUNT
  _held_monitor_count -= i;
  assert(_held_monitor_count >= 0, "Must always be greater than 0: " INT64_FORMAT, (int64_t)_held_monitor_count);
  if (jni) {
    _jni_monitor_count -= i;
    assert(_jni_monitor_count >= 0, "Must always be greater than 0: " INT64_FORMAT, (int64_t)_jni_monitor_count);
  }
#endif
}

frame JavaThread::vthread_last_frame() {
  assert (is_vthread_mounted(), "Virtual thread not mounted");
  return last_frame();
}

frame JavaThread::carrier_last_frame(RegisterMap* reg_map) {
  const ContinuationEntry* entry = vthread_continuation();
  guarantee (entry != nullptr, "Not a carrier thread");
  frame f = entry->to_frame();
  if (reg_map->process_frames()) {
    entry->flush_stack_processing(this);
  }
  entry->update_register_map(reg_map);
  return f.sender(reg_map);
}

frame JavaThread::platform_thread_last_frame(RegisterMap* reg_map) {
  return is_vthread_mounted() ? carrier_last_frame(reg_map) : last_frame();
}

javaVFrame* JavaThread::last_java_vframe(const frame f, RegisterMap *reg_map) {
  assert(reg_map != nullptr, "a map must be given");
  for (vframe* vf = vframe::new_vframe(&f, reg_map, this); vf; vf = vf->sender()) {
    if (vf->is_java_frame()) return javaVFrame::cast(vf);
  }
  return nullptr;
}

Klass* JavaThread::security_get_caller_class(int depth) {
  ResetNoHandleMark rnhm;
  HandleMark hm(Thread::current());

  vframeStream vfst(this);
  vfst.security_get_caller_frame(depth);
  if (!vfst.at_end()) {
    return vfst.method()->method_holder();
  }
  return nullptr;
}

// Internal convenience function for millisecond resolution sleeps.
bool JavaThread::sleep(jlong millis) {
  jlong nanos;
  if (millis > max_jlong / NANOUNITS_PER_MILLIUNIT) {
    // Conversion to nanos would overflow, saturate at max
    nanos = max_jlong;
  } else {
    nanos = millis * NANOUNITS_PER_MILLIUNIT;
  }
  return sleep_nanos(nanos);
}

// java.lang.Thread.sleep support
// Returns true if sleep time elapsed as expected, and false
// if the thread was interrupted.
// 用于使当前线程休眠指定的纳秒数。这是一个非阻塞的休眠方法，如果在休眠期间线程被中断，它将立即返回
bool JavaThread::sleep_nanos(jlong nanos) {
  assert(this == Thread::current(),  "thread consistency check");
  assert(nanos >= 0, "nanos are in range");

  ParkEvent * const slp = this->_SleepEvent;
  // 由于线程中断可能会发送一个unpark()到事件，我们在这里明确地重置它，以避免立即返回。
  // 实际的中断状态将在我们停车之前进行检查。
  slp->reset();
  // 线程中断在Java内存模型中建立了一个先行关系。
  // 因此，我们需要确保我们与中断状态同步。
  OrderAccess::fence();

  jlong prevtime = os::javaTimeNanos();

  jlong nanos_remaining = nanos;

  for (;;) {
    // 中断优先于超时
    if (this->is_interrupted(true)) {
      return false;
    }

    if (nanos_remaining <= 0) {
      return true;
    }

    {
      ThreadBlockInVM tbivm(this);
      OSThreadWaitState osts(this->osthread(), false /* not Object.wait() */);
      slp->park_nanos(nanos_remaining);
    }

    // 更新已用时间跟踪
    jlong newtime = os::javaTimeNanos();
    if (newtime - prevtime < 0) {
      // 时间倒退，只可能发生在没有单调时钟的情况下
      // 不是一个保证()，因为JVM不应该因内核/glibc错误而中止
      assert(false,
             "unexpected time moving backwards detected in JavaThread::sleep()");
    } else {
      nanos_remaining -= (newtime - prevtime);
    }
    prevtime = newtime;
  }
}

// Last thread running calls java.lang.Shutdown.shutdown()
// 调用Java虚拟机的关闭钩子。这些关闭钩子是由java.lang.Runtime类的addShutdownHook方法注册的，用于在虚拟机关闭之前执行特定的代码
void JavaThread::invoke_shutdown_hooks() {
  HandleMark hm(this);

  // 我们可能在这里有一个待定的异常，如果是这样，现在清除它。
  if (this->has_pending_exception()) {
    this->clear_pending_exception();
  }

  EXCEPTION_MARK;
  Klass* shutdown_klass =
    SystemDictionary::resolve_or_null(vmSymbols::java_lang_Shutdown(),
                                      THREAD);
  if (shutdown_klass != nullptr) {
    // 如果无法加载Shutdown类，则根本不调用Shutdown.shutdown()。
    // 这将意味着不会运行关闭钩子。注意，如果注册了一个关闭钩子，
    // Shutdown类将已经被加载（Runtime.addShutdownHook将加载它）。
    JavaValue result(T_VOID);
    JavaCalls::call_static(&result,
                           shutdown_klass,
                           vmSymbols::shutdown_name(),
                           vmSymbols::void_method_signature(),
                           THREAD);
  }
  CLEAR_PENDING_EXCEPTION;
}


#ifndef PRODUCT
void JavaThread::verify_cross_modify_fence_failure(JavaThread *thread) {
   report_vm_error(__FILE__, __LINE__, "Cross modify fence failure", "%p", thread);
}
#endif

// Helper function to create the java.lang.Thread object for a
// VM-internal thread. The thread will have the given name, and be
// a member of the "system" ThreadGroup.
Handle JavaThread::create_system_thread_object(const char* name, TRAPS) {
  Handle string = java_lang_String::create_from_str(name, CHECK_NH);

  // Initialize thread_oop to put it into the system threadGroup.
  // This is done by calling the Thread(ThreadGroup group, String name) constructor.
  Handle thread_group(THREAD, Universe::system_thread_group());
  Handle thread_oop =
    JavaCalls::construct_new_instance(vmClasses::Thread_klass(),
                                      vmSymbols::threadgroup_string_void_signature(),
                                      thread_group,
                                      string,
                                      CHECK_NH);

  return thread_oop;
}

// Starts the target JavaThread as a daemon of the given priority, and
// bound to the given java.lang.Thread instance.
// The Threads_lock is held for the duration.
void JavaThread::start_internal_daemon(JavaThread* current, JavaThread* target,
                                       Handle thread_oop, ThreadPriority prio) {

  assert(target->osthread() != nullptr, "target thread is not properly initialized");

  MutexLocker mu(current, Threads_lock);

  // Initialize the fields of the thread_oop first.
  if (prio != NoPriority) {
    java_lang_Thread::set_priority(thread_oop(), prio);
    // Note: we don't call os::set_priority here. Possibly we should,
    // else all threads should call it themselves when they first run.
  }

  java_lang_Thread::set_daemon(thread_oop());

  // Now bind the thread_oop to the target JavaThread.
  target->set_threadOopHandles(thread_oop());

  Threads::add(target); // target is now visible for safepoint/handshake
  // Publish the JavaThread* in java.lang.Thread after the JavaThread* is
  // on a ThreadsList. We don't want to wait for the release when the
  // Theads_lock is dropped when the 'mu' destructor is run since the
  // JavaThread* is already visible to JVM/TI via the ThreadsList.
  java_lang_Thread::release_set_thread(thread_oop(), target); // isAlive == true now
  Thread::start(target);
}

void JavaThread::vm_exit_on_osthread_failure(JavaThread* thread) {
  // At this point it may be possible that no osthread was created for the
  // JavaThread due to lack of resources. However, since this must work
  // for critical system threads just check and abort if this fails.
  if (thread->osthread() == nullptr) {
    // This isn't really an OOM condition, but historically this is what
    // we report.
    vm_exit_during_initialization("java.lang.OutOfMemoryError",
                                  os::native_thread_creation_failed_msg());
  }
}

void JavaThread::pretouch_stack() {
  // Given an established java thread stack with usable area followed by
  // shadow zone and reserved/yellow/red zone, pretouch the usable area ranging
  // from the current frame down to the start of the shadow zone.
  const address end = _stack_overflow_state.shadow_zone_safe_limit();
  if (is_in_full_stack(end)) {
    char* p1 = (char*) alloca(1);
    address here = (address) &p1;
    if (is_in_full_stack(here) && here > end) {
      size_t to_alloc = here - end;
      char* p2 = (char*) alloca(to_alloc);
      log_trace(os, thread)("Pretouching thread stack from " PTR_FORMAT " to " PTR_FORMAT ".",
                            p2i(p2), p2i(end));
      os::pretouch_memory(p2, p2 + to_alloc,
                          NOT_AIX(os::vm_page_size()) AIX_ONLY(4096));
    }
  }
}

// Deferred OopHandle release support.

class OopHandleList : public CHeapObj<mtInternal> {
  static const int _count = 4;
  OopHandle _handles[_count];
  OopHandleList* _next;
  int _index;
 public:
  OopHandleList(OopHandleList* next) : _next(next), _index(0) {}
  void add(OopHandle h) {
    assert(_index < _count, "too many additions");
    _handles[_index++] = h;
  }
  ~OopHandleList() {
    assert(_index == _count, "usage error");
    for (int i = 0; i < _index; i++) {
      _handles[i].release(JavaThread::thread_oop_storage());
    }
  }
  OopHandleList* next() const { return _next; }
};

OopHandleList* JavaThread::_oop_handle_list = nullptr;

// Called by the ServiceThread to do the work of releasing
// the OopHandles.
void JavaThread::release_oop_handles() {
  OopHandleList* list;
  {
    MutexLocker ml(Service_lock, Mutex::_no_safepoint_check_flag);
    list = _oop_handle_list;
    _oop_handle_list = nullptr;
  }
  assert(!SafepointSynchronize::is_at_safepoint(), "cannot be called at a safepoint");

  while (list != nullptr) {
    OopHandleList* l = list;
    list = l->next();
    delete l;
  }
}

// Add our OopHandles for later release.
void JavaThread::add_oop_handles_for_release() {
  MutexLocker ml(Service_lock, Mutex::_no_safepoint_check_flag);
  OopHandleList* new_head = new OopHandleList(_oop_handle_list);
  new_head->add(_threadObj);
  new_head->add(_vthread);
  new_head->add(_jvmti_vthread);
  new_head->add(_scopedValueCache);
  _oop_handle_list = new_head;
  Service_lock->notify_all();
}
