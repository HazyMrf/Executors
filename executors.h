#pragma once

#include <memory>
#include <chrono>
#include <vector>
#include <functional>
#include <thread>
#include <optional>
#include <queue>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <iostream>
#include <future>

class Task : public std::enable_shared_from_this<Task> {
    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

public:
    virtual ~Task() {
    }

    virtual void Run() = 0;

    void ExecuteTask() {
        std::lock_guard guard(lock_);

        if (dependencies_.empty() && triggers_.empty() &&
            deadline_ == std::chrono::system_clock::time_point{}) {
            TryCatchRun();
        } else {
            if (!triggers_.empty()) {
                for (auto& trig : triggers_) {
                    if (trig->IsFinished()) {
                        TryCatchRun();  // trigger Ok -> go run
                        return;
                    }
                }
            }
            if (!dependencies_.empty()) {
                for (auto& dep : dependencies_) {
                    if (!dep->IsFinished()) {
                        return;  // task is not finished and will be added to the end of queue
                    }
                }
                TryCatchRun();
                return;
            }
            if (deadline_ != std::chrono::system_clock::time_point{} && deadline_ < Clock::now()) {
                TryCatchRun();
            }
        }
    }

    void TryCatchRun() {
        if (!IsCanceled()) {  // cancel execution after submission
            try {
                Run();
                is_completed_ = true;
            } catch (...) {
                ex_ptr_ = std::current_exception();  // is_failed_ = true
                is_failed_ = true;
            }
            waiter_.notify_one();
        }
    }

    void AddDependency(std::shared_ptr<Task> dep) {
        dependencies_.push_back(dep);
    }
    void AddTrigger(std::shared_ptr<Task> trig) {
        triggers_.push_back(trig);
    }
    void SetTimeTrigger(std::chrono::system_clock::time_point at) {
        deadline_ = at;
    }

    // Task::run() completed without throwing exception
    bool IsCompleted() {
        return is_completed_;
    }

    // Task::run() threw exception
    bool IsFailed() {
        return is_failed_;
    }

    // Task was Canceled
    bool IsCanceled() {
        return is_canceled_;
    }

    // Task either completed, failed or was canceled
    bool IsFinished() {
        return is_completed_ || is_failed_ || is_canceled_;
    }

    std::exception_ptr GetError() {
        return ex_ptr_;
    }

    void Cancel() {
        is_canceled_ = true;
    }

    void Wait() {
        std::unique_lock lock(lock_);
        waiter_.wait(lock, [this] { return IsFinished(); });
    }

protected:
    std::atomic<bool> is_canceled_ = false;
    std::atomic<bool> is_completed_ = false;

    std::atomic<bool> is_failed_ = false;  // cannot create atomic over ex_ptr so create a boolean
    std::exception_ptr ex_ptr_ = nullptr;  // failed <=> ex_ptr != nullptr

    std::mutex lock_;
    std::condition_variable waiter_;

    std::vector<std::shared_ptr<Task>> dependencies_;
    std::vector<std::shared_ptr<Task>> triggers_;
    TimePoint deadline_{};  // time trigger
};

// Used instead of void in generic code
struct Unit {};

template <class T>
class Future final : public Task {  // Nobody can derive from Future, so it is marked as final
public:
    T Get() {
        Wait();  // wait for value to calculate
        if (ex_ptr_) {
            std::rethrow_exception(ex_ptr_);
        }
        return value_;
    }

    void Run() override {
        value_ = value_getter_();
    }

private:
    void PushFunc(std::function<T()> fn) {
        value_getter_ = std::move(fn);
    }

    friend class Executor;  // we access only private synchronization method and value_, so it's Ok

private:
    std::function<T()> value_getter_;
    T value_;
};

template <class T>
using FuturePtr = std::shared_ptr<Future<T>>;

/* Code for ThreadPool taken from 10th seminar about condition variables (Executor == ThreadPool) */
class Executor {  // ThreadPool

    template <typename T>
    class UnboundedBlockingQueue {
    public:
        void Put(T value) {
            std::lock_guard guard(lock_);
            queue_.push(value);
            not_empty_.notify_one();
        }

        void Close() {
            std::lock_guard guard(lock_);
            closed_ = true;
            not_empty_.notify_all();
        }

        std::optional<T> Take() {
            std::unique_lock lock(lock_);
            not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });

            if (queue_.empty()) {  // if closed AND no values left in queue
                return std::nullopt;
            }

            T value = queue_.front();
            queue_.pop();

            return value;
        }

        bool IsClosed() {
            std::lock_guard guard(lock_);
            return closed_;
        }

    private:
        std::queue<T> queue_;
        std::mutex lock_;
        std::condition_variable not_empty_;
        bool closed_ = false;
    };

public:
    explicit Executor(size_t num_threads) {  //  creating threads
        threads_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            threads_.emplace_back([this] { RunThread(); });
        }
    }

    void Submit(std::shared_ptr<Task> task) {
        if (queue_.IsClosed() || task->IsCanceled()) {  // double cancel is Ok
            task->Cancel();
        } else {
            queue_.Put(task);
        }
    }

    void StartShutdown() {
        queue_.Close();
    }
    void WaitShutdown() {
        for (auto& thr : threads_) {
            if (thr.joinable()) {
                thr.join();
            }
        }
    }

    template <class T>
    FuturePtr<T> Invoke(std::function<T()> fn) {
        FuturePtr<T> f_ptr = std::make_shared<Future<T>>();
        (*f_ptr).PushFunc(fn);
        queue_.Put(f_ptr);
        return f_ptr;
    }

    template <class Y, class T>
    FuturePtr<Y> Then(FuturePtr<T> input, std::function<Y()> fn) {
        FuturePtr<Y> f_ptr = std::make_shared<Future<Y>>();
        (*f_ptr).AddDependency(input);
        (*f_ptr).PushFunc(fn);
        queue_.Put(f_ptr);
        return f_ptr;
    }

    template <class T>
    FuturePtr<std::vector<T>> WhenAll(std::vector<FuturePtr<T>> all) {
        FuturePtr<std::vector<T>> f_ptr = std::make_shared<Future<std::vector<T>>>();
        // creating a callback which calls Get() on every Future
        (*f_ptr).PushFunc([all]() -> std::vector<T> {
            std::vector<T> results;
            results.reserve(all.size());
            for (auto& future : all) {
                results.push_back(future->Get());  // a.k.a wait for all Futures
            }
            return results;
        });
        queue_.Put(f_ptr);
        return f_ptr;
    }

    template <class T>
    FuturePtr<T> WhenFirst(std::vector<FuturePtr<T>> all) {  // PASSED DISABLED TEST
        FuturePtr<T> f_ptr = std::make_shared<Future<T>>();

        for (auto& future : all) {
            (*f_ptr).AddTrigger(future);
        }

        (*f_ptr).PushFunc([all]() -> T {
            for (auto& future : all) {
                if (future->IsFinished()) {
                    return future->Get();
                }
            }
            throw std::logic_error("Aborting due to system failure");
        });
        queue_.Put(f_ptr);
        return f_ptr;
    }

    template <class T>
    FuturePtr<std::vector<T>> WhenAllBeforeDeadline(
            std::vector<FuturePtr<T>> all, std::chrono::system_clock::time_point deadline) {
        FuturePtr<std::vector<T>> f_ptr = std::make_shared<Future<std::vector<T>>>();

        (*f_ptr).SetTimeTrigger(deadline);

        (*f_ptr).PushFunc([all]() -> std::vector<T> {
            std::vector<T> results;
            for (auto& future : all) {
                if (future->IsFinished()) {
                    results.push_back(future->Get());
                }
            }
            return results;
        });
        queue_.Put(f_ptr);
        return f_ptr;
    }

    ~Executor() {
        StartShutdown();
        for (auto& thr : threads_) {
            if (thr.joinable()) {
                thr.join();
            }
        }
    }

private:
    void RunThread() {
        while (auto task = queue_.Take()) {
            (*task)->ExecuteTask();
            if (!(*task)->IsFinished()) {
                queue_.Put(*task);  // task has dependencies, and wasn't executed
            }
        }
    }

private:
    std::vector<std::thread> threads_;
    UnboundedBlockingQueue<std::shared_ptr<Task>> queue_;
};

inline std::shared_ptr<Executor> MakeThreadPoolExecutor(int num_threads) {
    return std::shared_ptr<Executor>(new Executor(num_threads));
}
