/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include "thread_impl.hh"
#include "future.hh"
#include "do_with.hh"
#include "future-util.hh"
#include "timer.hh"
#include "reactor.hh"
#include "scheduling.hh"
#include <memory>
#include <setjmp.h>
#include <type_traits>
#include <chrono>
#include <experimental/optional>
#include <ucontext.h>
#include <boost/intrusive/list.hpp>

/// \defgroup thread-module Seastar threads
///
/// Seastar threads provide an execution environment where blocking
/// is tolerated; you can issue I/O, and wait for it in the same function,
/// rather then establishing a callback to be called with \ref future<>::then().
///
/// Seastar threads are not the same as operating system threads:
///   - seastar threads are cooperative; they are never preempted except
///     at blocking points (see below)
///     seastar 线程是合作的式,除了租塞以外,它从来都不会占用 cpu 资源
///   - seastar threads always run on the same core they were launched on
///     seastar 线程被运行在相同的核
///
/// Like other seastar code, seastar threads may not issue blocking system calls.
/// 类似于其他的 seastar 代码一样,seastar 线程不会在系统调用上被租塞
/// A seastar thread blocking point is any function that returns a \ref future<>.
/// you block by calling \ref future<>::get(); this waits for the future to become
/// available, and in the meanwhile, other seastar threads and seastar non-threaded
/// code may execute.
/// seastar唯一能租塞的地方就是当对 future 进行 get 操作的时候,这个时候线程就会被阻塞,
// 但是其他的线程和核是不会收到影响的;
///
/// Example:
/// \code
///    seastar::thread th([] {
///       sleep(5s).get();  // blocking point
///    });
/// \endcode
///
/// An easy way to launch a thread and carry out some computation, and return a
/// result from this execution is by using the \ref seastar::async() function.
/// The result is returned as a future, so that non-threaded code can wait for
/// the thread to terminate and yield a result.
/// 一个简单的方式可以让一个线程运行一些复杂的计算任务,通过 seastar::async 返回把结果返回;
/// 这个结果是 future, 所以非线程代码可能等待线程终止或者等待结果

/// Seastar API namespace
namespace seastar {

namespace stdx = std::experimental;

/// \addtogroup thread-module
/// @{

class thread;
class thread_attributes;
class thread_scheduling_group;

/// Class that holds attributes controling the behavior of a thread.
class thread_attributes {
public:
    thread_scheduling_group* scheduling_group = nullptr;  // FIXME: remove
    stdx::optional<seastar::scheduling_group> sched_group;
};


/// \cond internal
class thread_context;

namespace thread_impl {
 /**
  * 1. get 获得线程上下文
  * 2. switch_in 线程切入
  * 3. switch_out 线程切出
  * 4. init 初始化
  */
thread_context* get();
void switch_in(thread_context* to);
void switch_out(thread_context* from);
void init();

}

struct jmp_buf_link {
#ifdef ASAN_ENABLED
    ucontext_t context;
#else
    jmp_buf jmpbuf;
#endif
    jmp_buf_link* link;
    thread_context* thread;
};

extern thread_local jmp_buf_link g_unthreaded_context;

// Internal class holding thread state.  We can't hold this in
// \c thread itself because \c thread is movable, and we want pointers
// to this state to be captured.
class thread_context {
    struct stack_deleter {
        void operator()(char *ptr) const noexcept;
    };
    using stack_holder = std::unique_ptr<char[], stack_deleter>;
    static constexpr size_t base_stack_size = 128*1024;

    thread_attributes _attr;
#ifdef SEASTAR_THREAD_STACK_GUARDS
    const size_t _stack_size;
#else
    static constexpr size_t _stack_size = base_stack_size;
#endif
    stack_holder _stack{make_stack()};
    std::function<void ()> _func;
    jmp_buf_link _context;
    scheduling_group _scheduling_group;
    promise<> _done;
    bool _joined = false;
    timer<> _sched_timer{[this] { reschedule(); }};
    stdx::optional<promise<>> _sched_promise;

    boost::intrusive::list_member_hook<> _preempted_link;
    using preempted_thread_list = boost::intrusive::list<thread_context,
        boost::intrusive::member_hook<thread_context, boost::intrusive::list_member_hook<>,
        &thread_context::_preempted_link>,
        boost::intrusive::constant_time_size<false>>;

    boost::intrusive::list_member_hook<> _all_link;
    using all_thread_list = boost::intrusive::list<thread_context,
        boost::intrusive::member_hook<thread_context, boost::intrusive::list_member_hook<>,
        &thread_context::_all_link>,
        boost::intrusive::constant_time_size<false>>;

    static thread_local preempted_thread_list _preempted_threads;
    static thread_local all_thread_list _all_threads;
private:
    static void s_main(int lo, int hi); // all parameters MUST be 'int' for makecontext
    void setup();
    void main();
    stack_holder make_stack();
public:
    thread_context(thread_attributes attr, std::function<void ()> func);
    ~thread_context();
    void switch_in();
    void switch_out();
    bool should_yield() const;
    void reschedule();
    void yield();
    friend class thread;
    friend void thread_impl::switch_in(thread_context*);
    friend void thread_impl::switch_out(thread_context*);
    friend scheduling_group thread_impl::sched_group(const thread_context*);
};

/// \endcond


/// \brief thread - stateful thread of execution
/// 运行中的线程
/// Threads allow using seastar APIs in a blocking manner,
/// by calling future::get() on a non-ready future.  When
/// this happens, the thread is put to sleep until the future
/// becomes ready.
class thread {
    // 线程上下文
    std::unique_ptr<thread_context> _context;
    static thread_local thread* _current;
public:
    /// \brief Constructs a \c thread object that does not represent a thread
    /// of execution.(默认初始化的 thread 表示不是可以运行状态的 thread)
    thread() = default;
    /// \brief Constructs a \c thread object that represents a thread of execution
    ///
    /// \param func Callable object to execute in thread.  The callable is
    ///             called immediately.
    /// 放回一个 func 对象,让 thread 立即运行
    template <typename Func>
    thread(Func func);
    /// \brief Constructs a \c thread object that represents a thread of execution
    ///
    /// \param attr Attributes describing the new thread.
    /// \param func Callable object to execute in thread.  The callable is
    ///             called immediately.
    /// 线程描述信息+ callable 函数
    template <typename Func>
    thread(thread_attributes attr, Func func);
    /// \brief Moves a thread object.
    thread(thread&& x) noexcept = default;
    /// \brief Move-assigns a thread object.
    thread& operator=(thread&& x) noexcept = default;
    /// \brief Destroys a \c thread object.
    ///
    /// The thread must not represent a running thread of execution (see join()).
    /// 判断上下文为空 || 上下文可等待
    ~thread() { assert(!_context || _context->_joined); }
    /// \brief Waits for thread execution to terminate.
    ///
    /// Waits for thread execution to terminate, and marks the thread object as not
    /// representing a running thread of execution.
    /// 等待其他线程中止,并且设置当前线程为不可运行状态
    future<> join();
    /// \brief Voluntarily defer execution of current thread.
    /// 自动退出当前线程的执行,让出时间片
    /// Gives other threads/fibers a chance to run on current CPU.
    /// The current thread will resume execution promptly.
    /// 把当前的 cpu 让出来给别的 thread 使用,当前线程会在很快被恢复
    static void yield();
    /// \brief Checks whether this thread ought to call yield() now.
    /// 检测当前线程是否应该 yield;
    /// Useful where we cannot call yield() immediately because we
    /// Need to take some cleanup action first.
    ///这个函数对下面这种情况下是很有用的; 比如因为我们需要清理一些操作以至于我们不能直接调用 yield 函数
    static bool should_yield();

    //判断当前线程是否处于运行状态
    static bool running_in_thread() {
        return thread_impl::get() != nullptr;
    }
private:
    friend class reactor;
    // To be used by seastar reactor only.
    static bool try_run_one_yielded_thread();
};

//调度组
/// An instance of this class can be used to assign a thread to a particular scheduling group.
/// Threads can share the same scheduling group if they hold a pointer to the same instance
/// of this class.
///
/// All threads that belongs to a scheduling group will have a time granularity defined by \c period,
/// and can specify a fraction \c usage of that period that indicates the maximum amount of time they
/// expect to run. \c usage, is expected to be a number between 0 and 1 for this to have any effect.
/// Numbers greater than 1 are allowed for simplicity, but they just have the same meaning of 1, alas,
/// "the whole period".
///
/// Note that this is not a preemptive runtime, and a thread will not exit the CPU unless it is scheduled out.
/// In that case, \c usage will not be enforced and the thread will simply run until it loses the CPU.
/// This can happen when a thread waits on a future that is not ready, or when it voluntarily call yield.
///
/// Unlike what happens for a thread that is not part of a scheduling group - which puts itself at the back
/// of the runqueue everytime it yields, a thread that is part of a scheduling group will only yield if
/// it has exhausted its \c usage at the call to yield. Therefore, threads in a schedule group can and
/// should yield often.
///
/// After those events, if the thread has already run for more than its fraction, it will be scheduled to
/// run again only after \c period completes, unless there are no other tasks to run (the system is
/// idle)
class thread_scheduling_group {
    std::chrono::nanoseconds _period;//周期
    std::chrono::nanoseconds _quota; //
    std::chrono::time_point<thread_clock> _this_period_ends = {};
    std::chrono::time_point<thread_clock> _this_run_start = {};
    std::chrono::nanoseconds _this_period_remain = {};
public:
    /// \brief Constructs a \c thread_scheduling_group object
    ///
    /// \param period a duration representing the period
    /// \param usage which fraction of the \c period to assign for the scheduling group. Expected between 0 and 1.
    thread_scheduling_group(std::chrono::nanoseconds period, float usage);
    /// \brief changes the current maximum usage per period
    ///
    /// \param new_usage The new fraction of the \c period (Expected between 0 and 1) during which to run
    void update_usage(float new_usage) {
        _quota = std::chrono::duration_cast<std::chrono::nanoseconds>(new_usage * _period);
    }
private:
    void account_start();
    void account_stop();
    stdx::optional<thread_clock::time_point> next_scheduling_point() const;
    friend class thread_context;
};

template <typename Func>
inline
thread::thread(thread_attributes attr, Func func)
        : _context(std::make_unique<thread_context>(std::move(attr), func)) {
}

template <typename Func>
inline
thread::thread(Func func)
        : thread(thread_attributes(), std::move(func)) {
}

inline
future<>
thread::join() {
    //设置当前线程的 join = true
    _context->_joined = true;
    //返回一个 promise 的 future
    return _context->_done.get_future();
}

/// Executes a callable in a seastar thread.
///
/// Runs a block of code in a threaded context,
/// which allows it to block (using \ref future::get()).  The
/// result of the callable is returned as a future.
///
/// \param attr a \ref thread_attributes instance
/// \param func a callable to be executed in a thread
/// \param args a parameter pack to be forwarded to \c func.
/// \return whatever \c func returns, as a future.
///
/// Example:
/// \code
///    future<int> compute_sum(int a, int b) {
///        thread_attributes attr = {};
///        attr.scheduling_group = some_scheduling_group_ptr;
///        return seastar::async(attr, [a, b] {
///            // some blocking code:
///            sleep(1s).get();
///            return a + b;
///        });
///    }
/// \endcode
template <typename Func, typename... Args>
inline
futurize_t<std::result_of_t<std::decay_t<Func>(std::decay_t<Args>...)>>
async(thread_attributes attr, Func&& func, Args&&... args) {
    using return_type = std::result_of_t<std::decay_t<Func>(std::decay_t<Args>...)>;
    struct work {
        thread_attributes attr;
        Func func;
        std::tuple<Args...> args;
        promise<return_type> pr;
        thread th;
    };
    return do_with(work{std::move(attr), std::forward<Func>(func), std::forward_as_tuple(std::forward<Args>(args)...)}, [] (work& w) mutable {
        auto ret = w.pr.get_future();
        w.th = thread(std::move(w.attr), [&w] {
            futurize<return_type>::apply(std::move(w.func), std::move(w.args)).forward_to(std::move(w.pr));
        });
        return w.th.join().then([ret = std::move(ret)] () mutable {
            return std::move(ret);
        });
    });
}

/// Executes a callable in a seastar thread.
///
/// Runs a block of code in a threaded context,
/// which allows it to block (using \ref future::get()).  The
/// result of the callable is returned as a future.
///
/// \param func a callable to be executed in a thread
/// \param args a parameter pack to be forwarded to \c func.
/// \return whatever \c func returns, as a future.
template <typename Func, typename... Args>
inline
futurize_t<std::result_of_t<std::decay_t<Func>(std::decay_t<Args>...)>>
async(Func&& func, Args&&... args) {
    return async(thread_attributes{}, std::forward<Func>(func), std::forward<Args>(args)...);
}
/// @}

}
