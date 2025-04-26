/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This module wraps the native pthread behind an object-oriented class with
 * delegation protocol where variance of thread functionality is achieved
 * by passing a closure (functor) that the thread runs than using inherittance
 * to varying the different functionalities and which always results in
 * proliferation of subclass and hard to be maintained.
 */

#ifndef DMN_PROC_HPP_

#define DMN_PROC_HPP_

#define DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(mutex)                            \
  pthread_cleanup_push(&dmn::cleanupFuncToUnlockPthreadMutex, (mutex))

#define DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP(...) pthread_cleanup_pop(0)

#include <pthread.h>

#include <functional>
#include <string>
#include <string_view>

namespace dmn {

void cleanupFuncToUnlockPthreadMutex(void *mutex);

/**
 * Proc thread cancellation via (StopExec) is synchronous, so if the functor
 * runs infinitely without any pthread cancellation point, we should voluntarily
 * call Proc::yield() at different point in time in the loop.
 *
 * It is RAII model where in destruction of Proc object, it will try to
 * cancel the thread and join it to free resource, so the thread should respond
 * to pthread cancellation if it is in a loop.
 */
class Proc {
  using Task = std::function<void()>;

  enum State { kInvalid, kNew, kReady, kRunning };

public:
  Proc(std::string_view name, Proc::Task fn = {});
  virtual ~Proc() noexcept;

  Proc(const Proc &obj) = delete;
  const Proc &operator=(const Proc &obj) = delete;
  Proc(Proc &&obj) = delete;
  Proc &operator=(Proc &&obj) = delete;

  bool exec(Proc::Task fn = {});
  bool wait();

  static void yield();

protected:
  Proc::State getState() const;
  Proc::State setState(Proc::State state);
  void setTask(Proc::Task fn);

  bool runExec();
  bool stopExec();

private:
  static void *runFnInThreadHelper(void *context);

  /**
   * data members for constructor to instantiate the object.
   */
  const std::string m_name{};

  /**
   * data members for internal logic.
   */
  Proc::Task m_fn{};
  Proc::State m_state{};
  pthread_t m_th{};
}; // class Proc

} // namespace dmn

#endif // DMN_PROC_HPP_
