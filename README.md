# Executors framework

## Intro

When we write multithreaded code, we often end up with something like this:
```c++

class Output;
class Input;

Output Compute(const Input& inputs) {
    // Select concurrency level
    int num_threads = std::thread::hardware_concurrency();

    // Split input into tasks
    std::vector<Tasks> tasks = SplitIntoSubtasks(inputs, num_threads);

    // Launch threads with some Tasks
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, &inputs, &tasks] {
            ComputeSubtask(&tasks[i], input);
        });
    }

    // Join all threads, or use jthread
    for (auto& t : threads) t.join();

    // Combine
    return Combine(tasks, input);
}
```

If you look closely at the code, you may notice that it blends 2 unassociated actions:

1. A large task is partitioned into smaller, autonomous subtasks. Subsequently, the solutions to the subtasks are merged together. This code is algorithm-specific.

2. The code determines the quantity of threads to launch, how to launch them, and when to end them. This code is the same across the board.

The former point is satisfactory, but the latter is problematic.

* The user is incapable of regulating the number of threads that will be launched. _The code acts selfishly and monopolizes all cores on the machine_.

* It is inconvenient to use such code within another parallel algorithm
  _For instance, if we divide the task into 10 parts at the top level and intend to resolve each one using `Compute()`, then we will start `10 * hardware_concurrency` threads._

* Termination of computation cannot be effected, and progress monitoring is impracticable.

All problems arise from the code's self-appointed responsibility of 
creating threads. We aim to elevate this decision to the highest level, 
leaving solely the subdivision of independent subtasks in the code.

### Executors и Tasks

* `Task` - This is some form of computation segment. The effective 
  computation code is situated inside the run() function and is determined by the user.
* `Executor` - Actual ThreadPool. Its role is to execute `Task`'s.
* In order to commence executing a `Task`, the user is required to dispatch it to an `Executor` using the `Submit` function.
* Following this, the user has the option to await completion of the `Task` by invoking the `Task::Wait` function.
```c++
class MyTaks : public Task {
private:
    Params params_;
public:
    MyTask(Params params) : params_(params) {}
    virtual void Run() {
        /* your code goes here */
    }
}

void /* T */ Compute(std::shared_ptr<Executor> executor, Params params) {
    auto task = std::make_shared<MyTask>(params);
    executor->Submit(task);
    task.Wait();
    /* return result of computation */
}
```
* `Task` provides API for setting dependencies and triggers using methods `Task::AddDependency` and `Task::AddTrigger`.

* User can manually cancel `Task` in any time with method `Task::Cancel`. Also, `Task` has 3 possible stats: _Completed, Canceled_ and _Failed_ 
 which can be checked with corresponding functions.

* `Executor` provides API to stop execution.
    * `Executor::StartShutdown` - each task which is still not executed, will not be executed and marked as _Canceled_.
    * `Executor::WaitShutdown` - blocks the executor for any additional `Task`'s;

### Futures

* `Future` - `Task` with some result.

* `Executor` provides API for working with `Future`:
    * `Invoke(callback)` - execute `callback` and return resul as `Future`.
    * `Then(input, cb)` - execute `cb`, after `input`. *Notice*: `Future` is returned immediately, without waiting for input to be executed.
    * `WhenAll`, `WhenFirst`, `WhenAllBeforeDeadline` can be used to work with multiple `Future`s at the same time.

    

