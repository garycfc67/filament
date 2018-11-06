/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Note: The overhead of SYSTRACE_TAG_JOBSYSTEM is not negligible especially with parallel_for().
//#define SYSTRACE_TAG SYSTRACE_TAG_JOBSYSTEM
#define SYSTRACE_TAG SYSTRACE_TAG_NEVER

// when SYSTRACE_TAG_JOBSYSTEM is used, enables even heavier systraces
#define HEAVY_SYSTRACE  0


#include <utils/JobSystem.h>

#include <cmath>
#include <random>

#include <utils/compiler.h>
#include <utils/memalign.h>
#include <utils/Panic.h>
#include <utils/Systrace.h>

#if !defined(WIN32)
#    include <pthread.h>
#endif

#ifdef ANDROID
#    include <sys/time.h>
#    include <sys/resource.h>
#    ifndef ANDROID_PRIORITY_URGENT_DISPLAY
#        define ANDROID_PRIORITY_URGENT_DISPLAY -8  // see include/system/thread_defs.h
#    endif
#    ifndef ANDROID_PRIORITY_DISPLAY
#        define ANDROID_PRIORITY_DISPLAY -4  // see include/system/thread_defs.h
#    endif
#    ifndef ANDROID_PRIORITY_NORMAL
#        define ANDROID_PRIORITY_NORMAL 0 // see include/system/thread_defs.h
#    endif
#elif defined(__linux__)
// There is no glibc wrapper for gettid on linux so we need to syscall it.
#    include <unistd.h>
#    include <sys/syscall.h>
#    define gettid() syscall(SYS_gettid)
#endif

namespace utils {

UTILS_DEFINE_TLS(JobSystem::ThreadState *) JobSystem::sThreadState(nullptr);

void JobSystem::setThreadName(const char* name) noexcept {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
// TODO: implement setting thread name on WIN32 
#endif
}

void JobSystem::setThreadPriority(Priority priority) noexcept {
#ifdef ANDROID
    int androidPriority = 0;
    switch (priority) {
        case Priority::NORMAL:
            androidPriority = ANDROID_PRIORITY_NORMAL;
            break;
        case Priority::DISPLAY:
            androidPriority = ANDROID_PRIORITY_DISPLAY;
            break;
        case Priority::URGENT_DISPLAY:
            androidPriority = ANDROID_PRIORITY_URGENT_DISPLAY;
            break;
    }
    setpriority(PRIO_PROCESS, 0, androidPriority);
#endif
}

void JobSystem::setThreadAffinity(uint32_t mask) noexcept {
#if defined(__linux__)
    int bit = 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    while (mask) {
        if (mask & 1) {
            CPU_SET(bit, &set);
        }
        bit++;
        mask >>= 1;
    }
    sched_setaffinity(gettid(), sizeof(set), &set);
#endif
}

JobSystem::JobSystem(size_t threadCount, size_t adoptableThreadsCount) noexcept
    : mJobPool("JobSystem Job pool", MAX_JOB_COUNT * sizeof(Job)),
      mJobStorageBase(static_cast<Job *>(mJobPool.getAllocator().getCurrent()))
{
    SYSTRACE_ENABLE();

    if (threadCount == 0) {
        // default value, system dependant
        size_t hwThreads = std::thread::hardware_concurrency();
        if (UTILS_HAS_HYPER_THREADING) {
            // For now we avoid using HT, this simplifies profiling.
            // TODO: figure-out what to do with Hyper-threading
            threadCount = hwThreads / 2 - 1;
        } else {
            threadCount = hwThreads - 1;
        }
    }
    threadCount = std::min(size_t(UTILS_HAS_THREADING ? 32 : 0), threadCount);

    mThreadStates = aligned_vector<ThreadState>(threadCount + adoptableThreadsCount);
    mThreadCount = uint16_t(threadCount);
    mParallelSplitCount = (uint8_t)std::ceil((std::log2f(threadCount + adoptableThreadsCount)));

    // this is pitty these are not compile-time checks (C++17 supports it apparently)
    assert(mExitRequested.is_lock_free());
    assert(Job().runningJobCount.is_lock_free());

    std::random_device rd;
    const size_t hardwareThreadCount = mThreadCount;
    auto& states = mThreadStates;

    #pragma nounroll
    for (size_t i = 0, n = states.size(); i < n; i++) {
        auto& state = states[i];
        state.rndGen = default_random_engine(rd());
        state.mask = uint32_t(1UL << i);
        state.js = this;
        if (i < hardwareThreadCount) {
            // don't start a thread of adoptable thread slots
            state.thread = std::thread(&JobSystem::loop, this, &state);
        }
    }
}

JobSystem::~JobSystem() {
    requestExit();

    #pragma nounroll
    for (auto &state : mThreadStates) {
        // adopted threads are not joinable
        if (state.thread.joinable()) {
            state.thread.join();
        }
    }
}

void JobSystem::incRef(Job const* job) noexcept {
    // no action is taken when incrementing the reference counter, therefore we can safely use
    // memory_order_relaxed.
    job->refCount.fetch_add(1, std::memory_order_relaxed);
}

void JobSystem::decRef(Job const* job) noexcept {

    // We must ensure that accesses from other threads happen before deleting the Job.
    // To accomplish this, we need to guarantee that no read/writes are reordered after the
    // dec-ref, because ANOTHER thread could hold the last reference (after us) and that thread
    // needs to see all accesses completed before it deletes the object. This is done
    // with memory_order_release.
    // Similarly, we need to guarantee that no read/write are reordered before the last decref,
    // or some other thread could see a destroyed object before the ref-count is 0. This is done
    // with memory_order_acquire.
    auto c = job->refCount.fetch_sub(1, __has_feature(thread_sanitizer) ?
            std::memory_order_acq_rel :
            std::memory_order_release);
    assert(c > 0);
    if (c == 1) {
        // This was the last reference, it's safe to destroy the job.
#if !__has_feature(thread_sanitizer)
        // TSAN doesn't handle standalone fences, we use memory_order_acq_rel instead
        std::atomic_thread_fence(std::memory_order_acquire);
#endif
        mJobPool.destroy(job);
    }
}

JobSystem* JobSystem::getJobSystem() noexcept {
    ThreadState* const state = sThreadState;
    return state ? state->js : nullptr;
}

void JobSystem::requestExit() noexcept {
    mLock.lock();
    // memory_order_relaxed is okay because we're surrounded by lock/unlock, we just want atomicity here.
    mExitRequested.store(true, std::memory_order_relaxed);
    mLock.unlock();
    mCondition.notify_all();
#if __ARM_ARCH_7A__
    // on ARMv7a SEL is needed
        __dsb(0xA);     // ISHST = 0xA (b1010)
        UTILS_SIGNAL_EVENT();
#endif
}

inline bool JobSystem::exitRequested() const noexcept {
    // memory_order_relaxed is safe because the only action taken is to exit the thread
    return mExitRequested.load(std::memory_order_relaxed);
}

inline bool JobSystem::hasJobCompleted(JobSystem::Job const* job) noexcept {
    return job->runningJobCount.load(std::memory_order_relaxed) <= 0;
}

inline JobSystem::ThreadState& JobSystem::getState() noexcept {
    // check we're not using a thread not owned by the thread pool
    assert(sThreadState);
    return *sThreadState;
}

JobSystem::Job* JobSystem::allocateJob() noexcept {
    return mJobPool.make<Job>();
}

inline JobSystem::ThreadState& JobSystem::getStateToStealFrom(JobSystem::ThreadState& state) noexcept {
    // memory_order_relaxed is okay because we don't take any action that has data dependency
    // on this value (in particular mThreadStates, is always initialized properly).
    uint16_t adopted = mAdoptedThreads.load(std::memory_order_relaxed);
    // this is biased, but frankly, we don't care. it's fast.
    uint16_t index = uint16_t(state.rndGen() % (mThreadCount + adopted));
    assert(index < mThreadStates.size());
    return mThreadStates[index];
}

bool JobSystem::execute(JobSystem::ThreadState& state) noexcept {

    Job* job = pop(state.workQueue);
    if (job == nullptr) {
        // our queue is empty, try to steal a job
        ThreadState& stateToStealFrom = getStateToStealFrom(state);
        if (&stateToStealFrom != &state) {
            // don't steal from our own queue
            job = steal(stateToStealFrom.workQueue);
            // nullptr -> nothing to steal in that queue either
        }
    }

    if (job) {
        SYSTRACE_CALL();

        UTILS_UNUSED_IN_RELEASE uint32_t activeJobs = mActiveJobs.fetch_sub(1, std::memory_order_relaxed);
        assert(activeJobs); // whoops, we were already at 0
        SYSTRACE_VALUE32("JobSystem::activeJobs", activeJobs - 1);

        if (UTILS_LIKELY(job->function)) {
            SYSTRACE_NAME("job->function");
            job->function(job->storage, *this, job);
        }
        finish(job);
    }
    return job != nullptr;
}

void JobSystem::loop(ThreadState* threadState) noexcept {
    setThreadName("JobSystem::loop");
    setThreadPriority(Priority::DISPLAY);

    // record our work queue to thread-local storage
    sThreadState = threadState;

    // run our main loop...
    do {
        if (!execute(*threadState)) {
            std::unique_lock<Mutex> lock(mLock);
            while (!exitRequested() && !(mActiveJobs.load(std::memory_order_relaxed))) {
                mCondition.wait(lock);
            }
        }
    } while (!exitRequested());
}

// -----------------------------------------------------------------------------------------------
// public API...


JobSystem::Job* JobSystem::create(JobSystem::Job* parent, JobFunc func) noexcept {
    parent = (parent == nullptr) ? mMasterJob : parent;
    Job* const job = allocateJob();
    if (UTILS_LIKELY(job)) {
        size_t index = 0x7FFF;
        if (parent) {
            // add a reference to the parent to make sure it can't be terminated.
            // memory_order_relaxed is safe because no action is taken at this point
            // (the job is not started yet).
            auto parentjobCount = parent->runningJobCount.fetch_add(1, std::memory_order_relaxed);

            // can't create a child job of a terminated parent
            assert(parentjobCount > 0);

            index = parent - mJobStorageBase;
            assert(index < MAX_JOB_COUNT);
        }
        job->function = func;
        job->parent = uint16_t(index);
    }
    return job;
}

void JobSystem::finish(Job* job) noexcept {
    SYSTRACE_CALL();

    Pin pin(*this, job);

    // terminate this job and notify its parent
    auto& jobPool = mJobPool;
    Job* const storage = mJobStorageBase;
    do {
        // std::memory_order_release here is needed to synchronize with JobSystem::wait()
        // which needs to "see" all changes that happened before the job terminated.
        auto runningJobCount = job->runningJobCount.fetch_sub(1,
                __has_feature(thread_sanitizer) ?
                std::memory_order_acq_rel :
                std::memory_order_release);
        assert(runningJobCount > 0);
        if (runningJobCount == 1) {
#if !__has_feature(thread_sanitizer)
            std::atomic_thread_fence(std::memory_order_acquire);
#endif
            // no more work, destroy this job and check the parent.
            Job* const parent = job->parent == 0x7FFF ? nullptr : &storage[job->parent];
            decRef(job);
            job = parent;
        } else {
            // there is still work (e.g.: children), we're done.
            break;
        }
    } while (job);

#if __ARM_ARCH_7A__
    // on ARMv7a SEL is needed
    __dsb(0xA);     // ISHST = 0xA (b1010)
    UTILS_SIGNAL_EVENT();
#endif
}

void JobSystem::run(JobSystem::Job*& job, uint32_t flags) noexcept {
#if HEAVY_SYSTRACE
    SYSTRACE_CALL();
#endif

    Pin pin(*this, job);

    ThreadState& state(getState());

    // increase the active job count before we add the job to the queue, because otherwise
    // the job could run and finish before the counter is incremented, which would trigger
    // an assert() in execute(). Either way, it's not "wrong", but the assert() is useful.
    uint32_t activeJobs = mActiveJobs.fetch_add(1, std::memory_order_relaxed);

    put(state.workQueue, job);

    SYSTRACE_CONTEXT();
    SYSTRACE_VALUE32("JobSystem::activeJobs", activeJobs + 1);

    // wake-up a thread if needed...
    if (!(flags & DONT_SIGNAL)) {
        // if it was busy before, try to wake-up another sleeping thread
        if (activeJobs) {
            // wake-up a queue
            { std::lock_guard<Mutex> lock(mLock); }
            mCondition.notify_one();
        }
    }

    // after run() returns, the job is virtually invalid (it'll die on its own)
    job = nullptr;
}

JobSystem::Job* JobSystem::runAndRetain(JobSystem::Job* job, uint32_t flags) noexcept {
    JobSystem::Job* retained = job;
    incRef(retained);
    run(job, flags);
    return retained;
}

void JobSystem::wait(Job*& job) noexcept {
    SYSTRACE_CALL();

    assert(job);

    ThreadState& state(getState());
    do {
        if (!execute(state)) {
            // we're a waiter so we spin!!!
            UTILS_WAIT_FOR_EVENT();
        }
    } while (!hasJobCompleted(job) && !exitRequested());

    decRef(job);

    // after wait() returns, the job has been destroyed
    job = nullptr;
}

void JobSystem::adopt() {
    ThreadState* const state = sThreadState;
    if (state) {
        // we're already part of a JobSystem, do nothing.
        ASSERT_PRECONDITION(this == state->js,
                "Called adopt on a thread owned by another JobSystem (%p), this=%p!",
                state->js, this);
        return;
    }

    // memory_order_relaxed is safe because we don't take action on this value.
    uint16_t adopted = mAdoptedThreads.fetch_add(1, std::memory_order_relaxed);
    size_t index = mThreadCount + adopted;

    ASSERT_POSTCONDITION(index < mThreadStates.size(),
            "Too many calls to adopt(). No more adoptable threads!");

    // all threads adopted by the JobSystem need to run at the same priority
    JobSystem::setThreadPriority(JobSystem::Priority::DISPLAY);

    // This thread's queue will be selectable immediately (i.e.: before we set its TLS)
    // however, it's not a problem since mThreadState is pre-initialized and valid
    // (e.g.: the queue is empty).

    sThreadState = &mThreadStates[index];
}

void JobSystem::emancipate() {
    ThreadState* const state = sThreadState;
    ASSERT_PRECONDITION(state, "this thread is not an adopted thread");
    ASSERT_PRECONDITION(state->js == this, "this thread is not adopted by us");
    sThreadState = nullptr;
}

io::ostream& operator<<(io::ostream& out, JobSystem const& js) {
    for (auto const& item : js.mThreadStates) {
        out << size_t(std::log2f(item.mask)) << ": " << item.workQueue.getCount() << io::endl;
    }
    return out;
}

} // namespace utils
