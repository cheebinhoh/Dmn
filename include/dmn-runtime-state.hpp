# Runtime State Engine - Public Header

#ifndef DMN_RUNTIME_STATE_HPP_
#define DMN_RUNTIME_STATE_HPP_

#include "dmn-runtime.hpp"
#include "dmn-state.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>

namespace dmn {

class Dmn_Runtime_State_Engine;

class Dmn_Runtime_State : public Dmn_State {
public:
  using FncType = std::function<void(Dmn_Runtime_State &)>;
  using OnErrorFnc = Dmn_Runtime_Job::OnErrorFncType; // std::function<void(std::exception_ptr &)>

  explicit Dmn_Runtime_State(std::string_view name);
  virtual ~Dmn_Runtime_State() noexcept;

  // State configuration (inherited semantics from Dmn_State)
  void setStateFnc(FncType fnc, int index = 0);
  void setNext(int index);
  void setNext();
  void setEnd();

  // Lifecycle API
  // Enqueue this state for runtime execution. Returns true if enqueue succeeded.
  // Subsequent successful calls are no-ops and return false.
  // Priority maps to Dmn_Runtime_Job::Priority; delay of zero means immediate addJob().
  bool run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
           const std::chrono::steady_clock::duration &delay = std::chrono::steady_clock::duration::zero(),
           OnErrorFnc onError = {});

  // Cooperative cancel. Safe to call from any thread. Idempotent.
  void cancel();

  // Blocking wait until terminal state. Throws if called from runtime thread.
  void wait();

  // Timed wait; returns true if terminal observed before timeout.
  template <typename Rep, typename Period>
  bool wait_for(const std::chrono::duration<Rep, Period> &timeout);

  // Async alternative: a shared_future that becomes ready when the state is terminal.
  // The shared_future is available immediately after createState() and supports multiple waiters.
  std::shared_future<void> getFuture();

  // Introspection
  bool isRunning() const;   // queued or actively running
  bool isCompleted() const; // terminal success
  bool isFailed() const;    // terminal failure
  bool isCancelled() const; // cancelled flag set

protected:
  // Hooks to allow derived implementations to react to lifecycle events.
  virtual void onStarted();
  virtual void onCompleted();
  virtual void onFailed(std::exception_ptr ep);
  virtual void onCancelled();

private:
  // Implementation details (opaque to public header) should be placed in the
  // corresponding .cpp. These members are intentionally documented in the
  // spec but left to the implementation to manage.

  // Note: keep the header minimal to avoid exposing internal synchronization
  // primitives; real implementation may add mutexes, atomic flags and
  // promise/shared_future wiring.
};

class Dmn_Runtime_State_Engine : public Dmn_Singleton<Dmn_Runtime_State_Engine> {
public:
  using DmnRuntimeStatePtr = std::shared_ptr<Dmn_Runtime_State>;

  static auto createInstance() -> Dmn_Runtime_State_Engine &;

  // Create a runtime-managed state object handle.
  // The returned shared_ptr may be kept by the client; the engine will also
  // retain a shared_ptr while the state is queued or running.
  DmnRuntimeStatePtr createState(std::string_view name);

  // Optional engine-level configuration and shutdown APIs are implemented
  // in the source file as needed.

protected:
  Dmn_Runtime_State_Engine();
  virtual ~Dmn_Runtime_State_Engine() noexcept;
};

} // namespace dmn

#endif // DMN_RUNTIME_STATE_HPP_
