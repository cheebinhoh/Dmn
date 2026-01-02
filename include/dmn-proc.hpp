/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-proc.hpp
 * @brief The header files for dmn-proc which wraps the native pthread behind an
 *        object-oriented class with delegation protocol where variance of
 *        thread functionality is achieved by passing a closure (functor) that
 *        the thread runs than using inherittance to varying the different
 *        functionalities and which always results in proliferation of subclass
 *        and hard to be maintained.
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

void cleanupFuncToUnlockPthreadMutex(void *arg);

/**
 * Dmn_Proc thread cancellation via (StopExec) is synchronous, so if the functor
 * runs infinitely without any pthread cancellation point, we should voluntarily
 * call Dmn_Proc::yield() at different point in time in the loop.
 *
 * It is RAII model where in destruction of Dmn_Proc object, it will try to
 * cancel the thread and join it to free resource, so the thread should respond
 * to pthread cancellation if it is in a loop.
 */
class Dmn_Proc {
  using Task = std::function<void()>;

  enum State { kInvalid, kNew, kReady, kRunning };

public:
  Dmn_Proc(std::string_view name, const Dmn_Proc::Task &fnc = {});
  virtual ~Dmn_Proc() noexcept;

  Dmn_Proc(const Dmn_Proc &obj) = delete;
  const Dmn_Proc &operator=(const Dmn_Proc &obj) = delete;
  Dmn_Proc(Dmn_Proc &&obj) = delete;
  Dmn_Proc &operator=(Dmn_Proc &&obj) = delete;

  /**
   * @brief This method executes the fnc passed in or to constructor to be
   *        executed in an asynchronous thread.
   *
   * @param fnc The functor to be executed in an asynchronous thread
   */
  auto exec(const Dmn_Proc::Task &fnc = {}) -> bool;

  /**
   * @brief This method puts the caller in pause and wait for return of the
   *        asynchronous executed thread (aka join the thread).
   *
   * @return True if the thread is joined successfully
   */
  auto wait() -> bool;

  /**
   * @brief This method volunteerly yeild the execution of the current thread.
   */
  static void yield();

protected:
  auto getState() const -> Dmn_Proc::State;
  auto setState(Dmn_Proc::State state) -> Dmn_Proc::State;
  void setTask(Dmn_Proc::Task fnc);

  auto runExec() -> bool;
  auto stopExec() -> bool;

private:
  static auto runFnInThreadHelper(void *context) -> void *;

  /**
   * data members for constructor to instantiate the object.
   */
  const std::string m_name{};

  /**
   * data members for internal logic.
   */
  Dmn_Proc::Task m_fnc{};
  Dmn_Proc::State m_state{};
  pthread_t m_th{};
}; // class Dmn_Proc

} // namespace dmn

#endif // DMN_PROC_HPP_
