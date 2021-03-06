// Copyright (c) 2014, the Dartino project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#ifdef DARTINO_ENABLE_DEBUGGING

#include "src/vm/session.h"

#include "src/shared/bytecodes.h"
#include "src/shared/connection.h"
#include "src/shared/flags.h"
#include "src/shared/globals.h"
#include "src/shared/platform.h"
#include "src/shared/utils.h"
#include "src/shared/version.h"

#include "src/vm/frame.h"
#include "src/vm/heap_validator.h"
#include "src/vm/native_interpreter.h"
#include "src/vm/links.h"
#include "src/vm/object_map.h"
#include "src/vm/process.h"
#include "src/vm/scheduler.h"
#include "src/vm/snapshot.h"
#include "src/vm/thread.h"

#define GC_AND_RETRY_ON_ALLOCATION_FAILURE(var, exp)    \
  Object* var = (exp);                                  \
  if (var->IsRetryAfterGCFailure()) {                   \
    program()->CollectGarbage();                        \
    var = (exp);                                        \
    ASSERT(!var->IsFailure());                          \
  }

namespace dartino {

static bool IsBuiltin(Class* klass, Program* program) {
#define CHECK(type, name, CamelName) \
    if (static_cast<Object*>(program->name()) == \
        static_cast<Object*>(klass)) return true;
ROOTS_DO(CHECK);
#undef CHECK
  if (klass == program->true_object()->get_class() ||
      klass == program->false_object()->get_class() ||
      klass == program->null_object()->get_class()) {
    return true;
  }

  return false;
}

class SessionState {
 public:
  virtual ~SessionState() {
    delete debug_previous_;
  }

  // Did we get a handshake from the compiler yet?
  bool IsConnected() const { return session()->got_handshake(); }

  // Initial state (pre spawn-for-main).
  virtual bool IsInitial() const { return false; }

  // Connected states (post spawn-for-main).
  virtual bool IsModifying() const { return false; }
  virtual bool IsSpawned() const { return false; }

  // Scheduled states (derived from spawned).
  virtual bool IsScheduled() const { return false; }
  virtual bool IsRunning() const { return false; }
  virtual bool IsPaused() const { return false; }
  virtual bool IsTerminating() const { return false; }

  // Final state.
  virtual bool IsTerminated() const { return false; }

  bool IsLiveEditingEnabled() const { return session()->live_editing(); }
  bool IsDebuggingEnabled() const { return session()->debugging(); }

  virtual void EnableLiveEditing() {
    ASSERT(!IsLiveEditingEnabled());
    session()->set_live_editing(true);
  }

  void EnableDebugging() {
    ASSERT(!IsDebuggingEnabled());
    session()->set_debugging(true);
  }

  // Main transition function on states.
  virtual SessionState* ProcessMessage(Connection::Opcode opcode);

  virtual const char* ToString() const = 0;

  void PrintTrace() {
    if (debug_previous_ != NULL) debug_previous_->PrintTrace();
    Print::Error("- %s\n", ToString());
  }

  // ActivateState is called immediately after changing sessions state.
  virtual void ActivateState(SessionState* previous) {
    ASSERT(session_ == NULL);
    ASSERT(debug_previous_ == NULL);
    session_ = previous->session_;
    debug_previous_ = previous;
    previous->session_ = NULL;
  }

  // Handling methods for interrupts of the interpreted process.
  // These will only be called while holding a lock on main_thread_monitor_
  // and while the program is paused within the scheduler.
  virtual Scheduler::ProcessInterruptionEvent HandleBreakpoint(
      Process* process) {
    UNREACHABLE();
    return Scheduler::kUnhandled;
  }

  virtual Scheduler::ProcessInterruptionEvent HandleUncaughtException(
      Process* process) {
    UNREACHABLE();
    return Scheduler::kUnhandled;
  }

  virtual Scheduler::ProcessInterruptionEvent HandleCompileTimeError(
      Process* process) {
    UNREACHABLE();
    return Scheduler::kUnhandled;
  }

  virtual Scheduler::ProcessInterruptionEvent HandleKilled(Process* process) {
    UNREACHABLE();
    return Scheduler::kUnhandled;
  }

  virtual Scheduler::ProcessInterruptionEvent HandleUnhandledSignal(
      Process* process) {
    UNREACHABLE();
    return Scheduler::kUnhandled;
  }

  virtual Scheduler::ProcessInterruptionEvent HandleTerminated(
      Process* process) {
    UNREACHABLE();
    return Scheduler::kUnhandled;
  }

 protected:
  SessionState()
      : session_(NULL), debug_previous_(NULL) {
  }

  explicit SessionState(Session* session)
      : session_(session), debug_previous_(NULL) {
  }

  Session* session() const { return session_; }
  Program* program() const { return session_->program(); }
  Connection* connection() const { return session_->connection_; }

  /// This enum should be kept in sync with `enum VmState` at
  /// 'pkg/dartino_compiler/lib/vm_commands.dart'.
  enum StateKind {
    kInitial,
    kModifying,
    kSpawned,
    kPaused,
    kRunning,
    kTerminating,
    kTerminated
  };

  virtual StateKind Kind() = 0;

 private:
  Session* session_;

 public:
  // TODO(zerny): Only retain previous state for debug builds.
  SessionState* debug_previous_;
};

static void MessageProcessingError(const char* message) {
  Print::UnregisterPrintInterceptors();
  Print::Error(message);
  Platform::Exit(-1);
}

// The initial session state is the state awaiting a handshake.
class InitialState : public SessionState {
 public:
  explicit InitialState(Session* session) : SessionState(session) {}

  bool IsInitial() const { return true; }

  const char* ToString() const { return "Initial"; }

 private:
  StateKind Kind() { return kInitial; }
};

// The modifying state denotes a temporary state-change of a connected
// state. Once committed or discarded the prior state is restored.
class ModifyingState : public SessionState {
 public:
  bool IsModifying() const { return true; }

  void ActivateState(SessionState* previous) {
    ASSERT(previous->IsConnected());
    SessionState::ActivateState(previous);
    previous_ = previous;
  }

  SessionState* RestorePrevious() {
    SessionState* restored = previous_;
    previous_ = NULL;
    // Swap position of this state and previous in the chain.
    debug_previous_ = restored->debug_previous_;
    restored->debug_previous_ = NULL;
    return restored;
  }

  const char* ToString() const { return "Modifying"; }

  SessionState* ProcessMessage(Connection::Opcode opcode);

 private:
  StateKind Kind() { return kModifying; }
  SessionState* previous_;
};

// The spawned state denotes a state where the main program process has been
// created.
class SpawnedState : public SessionState {
 public:
  explicit SpawnedState(Process* main_process)
      : initial_main_process_(main_process) {}

  bool IsSpawned() const { return true; }

  Process* main_process() const { return session()->main_process(); }

  // True if the session should be informed of the process's termination.
  bool ObserveTermination(Process* process) {
    return process == main_process();
  }

  void ActivateState(SessionState* previous) {
    ASSERT(previous->IsConnected());
    SessionState::ActivateState(previous);
    if (initial_main_process_ == NULL) return;
    // If this is the initial spawn, store the main process on session.
    ASSERT(!previous->IsSpawned());
    session()->set_main_process(initial_main_process_);
    initial_main_process_ = NULL;
    // Ensure that main is allocated the zeroth id.
    program()->EnsureDebuggerAttached();
    main_process()->EnsureDebuggerAttached();
  }

  const char* ToString() const { return "Spawned"; }

  SessionState* ProcessMessage(Connection::Opcode opcode);

 protected:
  SpawnedState() : initial_main_process_(NULL) {}
  StateKind Kind() { return kSpawned; }

 private:
  Process* initial_main_process_;
};

// Abstract base for scheduled states. A scheduled state is a state where the
// program is known to the scheduler.
class ScheduledState : public SpawnedState {
 public:
  bool IsScheduled() const { return true; }

  void ActivateState(SessionState* previous) {
    ASSERT(previous->IsModifying() || previous->IsSpawned());
    SpawnedState::ActivateState(previous);
  }

  Scheduler* scheduler() const {
    return main_process()->program()->scheduler();
  }
};

// A running state is scheduled and unpaused.
class RunningState : public ScheduledState {
 public:
  bool IsRunning() const { return true; }

  const char* ToString() const { return "Running"; }

  void ActivateState(SessionState* previous) {
    ASSERT((!previous->IsScheduled() && previous->IsSpawned()) ||
           (previous->IsScheduled() && previous->IsPaused()));
    ScheduledState::ActivateState(previous);
    if (previous->IsPaused()) {
      session()->ResumeExecution();
    }
  }

  Scheduler::ProcessInterruptionEvent HandleBreakpoint(Process* process) {
    ASSERT(IsDebuggingEnabled());
    SendBreakpoint(process);
    return Scheduler::kRemainPaused;
  }

  Scheduler::ProcessInterruptionEvent HandleUncaughtException(
      Process* process) {
    if (IsDebuggingEnabled() || ObserveTermination(process)) {
      ProcessDebugInfo* debug_info = process->debug_info();
      if (debug_info->is_stepping()) {
        debug_info->ClearSteppingInterrupted();
      }
      WriteBuffer buffer;
      buffer.WriteInt(debug_info->process_id());
      session()->PushTopStackFrame(process->stack());
      buffer.WriteInt64(
          session()->FunctionMessage(Function::cast(session()->Top())));
      // Drop function from session stack.
      session()->Drop(1);
      // Pop bytecode index from session stack and send it.
      buffer.WriteInt64(session()->PopInteger());
      connection()->Send(Connection::kUncaughtException, buffer);
      // If observing termination, remain paused to allow access to the
      // termination state.
      return Scheduler::kRemainPaused;
    }
    return Scheduler::kExitWithUncaughtException;
  }

  Scheduler::ProcessInterruptionEvent HandleCompileTimeError(Process* process) {
    if (IsDebuggingEnabled() || ObserveTermination(process)) {
      ProcessDebugInfo* debug_info = process->debug_info();
      if (debug_info->is_stepping()) {
        debug_info->ClearSteppingInterrupted();
      }
      WriteBuffer buffer;
      connection()->Send(Connection::kProcessCompileTimeError, buffer);
      // If observing termination, remain paused to allow access to the
      // termination state.
      return Scheduler::kRemainPaused;
    }
    return Scheduler::kExitWithCompileTimeError;
  }

  Scheduler::ProcessInterruptionEvent HandleKilled(Process* process) {
    // TODO(kustermann): We might want to let a debugger know if the process
    // didn't normally terminate, but rather was killed.
    if (ObserveTermination(process)) {
      WriteBuffer buffer;
      connection()->Send(Connection::kProcessTerminated, buffer);
    }
    return Scheduler::kExitWithKilledSignal;
  }

  Scheduler::ProcessInterruptionEvent HandleUnhandledSignal(Process* process) {
    // TODO(kustermann): We might want to let a debugger know that the process
    // didn't normally terminate, but rather was killed due to a linked process.
    if (ObserveTermination(process)) {
      WriteBuffer buffer;
      connection()->Send(Connection::kProcessTerminated, buffer);
    }
    return Scheduler::kExitWithUnhandledSignal;
  }

  Scheduler::ProcessInterruptionEvent HandleTerminated(Process* process) {
    if (ObserveTermination(process)) {
      WriteBuffer buffer;
      connection()->Send(Connection::kProcessTerminated, buffer);
    }
    return Scheduler::kExitWithoutError;
  }

  SessionState* ProcessMessage(Connection::Opcode opcode);

 private:
  void SendBreakpoint(Process* process) {
    ProcessDebugInfo* debug_info = process->debug_info();
    ASSERT(debug_info != NULL);
    int breakpoint_id = debug_info->current_breakpoint_id();
    // TODO(zerny): Stepping is per-process, but should be cleared at a program
    // level.
    if (debug_info->is_stepping()) {
      debug_info->ClearSteppingFromBreakPoint();
    }
    WriteBuffer buffer;
    buffer.WriteInt(breakpoint_id);
    buffer.WriteInt(debug_info->process_id());
    session()->PushTopStackFrame(process->stack());
    buffer.WriteInt64(
        session()->FunctionMessage(Function::cast(session()->Top())));
    // Drop function from session stack.
    session()->Drop(1);
    // Pop bytecode index from session stack and send it.
    buffer.WriteInt64(session()->PopInteger());
    connection()->Send(Connection::kProcessBreakpoint, buffer);
  }

  StateKind Kind() { return kRunning; }
};

// A paused state is scheduled and paused.
class PausedState : public ScheduledState {
 public:
  explicit PausedState(Process* process, bool interrupted = false)
      : process_(process),
        interrupted_(interrupted) {}

  bool IsPaused() const { return true; }

  const char* ToString() const { return "Paused"; }

  SessionState* ProcessContinue() {
    if (!interrupted_) {
      scheduler()->ContinueProcess(process_);
    }
    return new RunningState();
  }

  SessionState* ProcessMessage(Connection::Opcode opcode);

  Process* process() const { return process_; }

 private:
  void RestartFrame(int frame_index);

  Process* process_;
  bool interrupted_;
  StateKind Kind() { return kPaused; }
};

// A terminating state is still scheduled, but represents a session that is in
// the process of shutting down. The session has been terminated from the
// client's point of view.
class TerminatingState : public ScheduledState {
 public:
  explicit TerminatingState(Process::State process_state)
      : process_state_(process_state) {
  }

  bool IsTerminating() const { return true; }

  const char* ToString() const { return "Terminating"; }

  void ActivateState(SessionState* previous) {
    ASSERT(previous->IsPaused());
    ScheduledState::ActivateState(previous);
    session()->ResumeExecution();
    session()->SignalMainThread(Session::kSessionEnd);
  }

  Scheduler::ProcessInterruptionEvent HandleBreakpoint(Process* process) {
    return HandleEvent(process);
  }

  Scheduler::ProcessInterruptionEvent HandleUncaughtException(
      Process* process) {
    return HandleEvent(process);
  }

  Scheduler::ProcessInterruptionEvent HandleKilled(Process* process) {
    return HandleEvent(process);
  }

  Scheduler::ProcessInterruptionEvent HandleUnhandledSignal(Process* process) {
    return HandleEvent(process);
  }

  Scheduler::ProcessInterruptionEvent HandleTerminated(Process* process) {
    return HandleEvent(process);
  }

  Scheduler::ProcessInterruptionEvent HandleCompileTimeError(Process* process) {
    return HandleEvent(process);
  }

  Scheduler::ProcessInterruptionEvent HandleEvent(Process* process) {
    switch (process_state_) {
      case Process::kCompileTimeError:
        return Scheduler::kExitWithCompileTimeError;
      case Process::kUncaughtException:
        return Scheduler::kExitWithUncaughtException;
      default:
        return Scheduler::kExitWithoutError;
    }
  }

 private:
  Process::State process_state_;
  StateKind Kind() { return kTerminating; }
};

// A terminated state is the final state. It is not considered connected to the
// session anymore.
class TerminatedState : public SessionState {
 public:
  explicit TerminatedState(int exit_code) : exit_code_(exit_code) {}

  bool IsTerminated() const { return true; }

  const char* ToString() const { return "Terminated"; }

  int exit_code() const { return exit_code_; }

  SessionState* ProcessMessage(Connection::Opcode opcode);

 private:
  int exit_code_;
  StateKind Kind() { return kTerminated; }
};

class ConnectionPrintInterceptor : public PrintInterceptor {
 public:
  explicit ConnectionPrintInterceptor(Connection* connection)
      : connection_(connection) {}
  virtual ~ConnectionPrintInterceptor() {}

  virtual void Out(char* message) {
    WriteBuffer buffer;
    buffer.WriteString(message);
    connection_->Send(Connection::kStdoutData, buffer);
  }

  virtual void Error(char* message) {
    WriteBuffer buffer;
    buffer.WriteString(message);
    connection_->Send(Connection::kStderrData, buffer);
  }

 private:
  Connection* connection_;
};

class PostponedChange {
 public:
  PostponedChange(Object** description, int size)
      : description_(description), size_(size), next_(NULL) {
    ++number_of_changes_;
  }

  ~PostponedChange() {
    delete[] description_;
    --number_of_changes_;
  }

  PostponedChange* next() const { return next_; }
  void set_next(PostponedChange* change) { next_ = change; }

  Object* get(int i) const { return description_[i]; }
  int size() const { return size_; }

  static int number_of_changes() { return number_of_changes_; }

  void IteratePointers(PointerVisitor* visitor) {
    for (int i = 0; i < size_; i++) {
      visitor->Visit(description_ + i);
    }
  }

 private:
  static int number_of_changes_;
  Object** description_;
  int size_;
  PostponedChange* next_;
};

SessionState* SessionState::ProcessMessage(Connection::Opcode opcode) {
  if (opcode != Connection::kHandShake && !IsConnected()) {
    MessageProcessingError("Got command before handshake\n");
  }
  switch (opcode) {
    case Connection::kHandShake: {
      const char* version = GetVersion();
      int version_length = strlen(version);
      int compiler_version_length;
      uint8* compiler_version =
          connection()->ReadBytes(&compiler_version_length);
      bool version_match = Version::Check(version,
          version_length,
          reinterpret_cast<const char *>(compiler_version),
          compiler_version_length);
      WriteBuffer buffer;
      buffer.WriteBoolean(version_match);
      buffer.WriteInt(version_length);
      buffer.WriteString(version);
      free(compiler_version);
      // Send word size.
      buffer.WriteInt(kBitsPerPointer);
      // Send floating point size.
      buffer.WriteInt(kBitsPerDartinoDouble);
      // Send state
      buffer.WriteInt(Kind());
      connection()->Send(Connection::kHandShakeResult, buffer);
      if (!version_match) {
        MessageProcessingError("Error: Different compiler and VM version.\n");
      }
      session()->set_got_handshake(true);
      return this;
    }

    case Connection::kConnectionError: {
      MessageProcessingError("Lost connection to compiler.");
      return NULL;
    }

    case Connection::kCompilerError: {
      session()->SignalMainThread(Session::kError);
      return NULL;
    }

    case Connection::kSessionEnd: {
      // TODO(sigurdm): Allow disconnecting without terminating the process.
      // The main process might have just received a signal in which case we are
      // still in a running state.
      // TODO(zerny): Refactor to a terminated state change and assert that.
      ASSERT(!IsScheduled() || IsRunning());
      session()->SignalMainThread(Session::kSessionEnd);
      return NULL;
    }

    case Connection::kProcessSpawnForMain: {
      int arguments_count = connection()->ReadInt();
      List<List<uint8>> arguments = List<List<uint8>>::New(arguments_count);
      for (int i = 0; i < arguments_count; i++) {
        int length;
        uint8* bytes = connection()->ReadBytes(&length);
        arguments[i] = List<uint8>(bytes, length);
      }
      if (!program()->was_loaded_from_snapshot()) {
        ProgramFolder::FoldProgramByDefault(program());
      }
      return new SpawnedState(program()->ProcessSpawnForMain(arguments));
    }

    case Connection::kDebugging: {
      if (!IsDebuggingEnabled()) EnableDebugging();
      session()->fibers_map_id_ = connection()->ReadInt();
      WriteBuffer buffer;
      buffer.WriteBoolean(program()->was_loaded_from_snapshot());
      buffer.WriteInt(program()->snapshot_hash());
      connection()->Send(Connection::kDebuggingReply, buffer);
      break;
    }

    case Connection::kDisableStandardOutput: {
      Print::DisableStandardOutput();
      break;
    }

    case Connection::kNewMap: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->NewMap(connection()->ReadInt());
      break;
    }

    case Connection::kDeleteMap: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->DeleteMap(connection()->ReadInt());
      break;
    }

    case Connection::kPushFromMap: {
      ASSERT(!IsScheduled() || IsPaused());
      int index = connection()->ReadInt();
      int64 id = connection()->ReadInt64();
      session()->PushFromMap(index, id);
      break;
    }

    case Connection::kPopToMap: {
      ASSERT(!IsScheduled() || IsPaused());
      int index = connection()->ReadInt();
      int64 id = connection()->ReadInt64();
      session()->PopToMap(index, id);
      break;
    }

    case Connection::kRemoveFromMap: {
      ASSERT(!IsScheduled() || IsPaused());
      int index = connection()->ReadInt();
      int64 id = connection()->ReadInt64();
      session()->RemoveFromMap(index, id);
      break;
    }

    case Connection::kDup: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->Dup();
      break;
    }

    case Connection::kDrop: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->Drop(connection()->ReadInt());
      break;
    }

    case Connection::kPushNull: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushNull();
      break;
    }

    case Connection::kPushBoolean: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushBoolean(connection()->ReadBoolean());
      break;
    }

    case Connection::kPushNewInteger: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushNewInteger(connection()->ReadInt64());
      break;
    }

    case Connection::kPushNewBigInteger: {
      ASSERT(!IsScheduled() || IsPaused());
      bool negative = connection()->ReadBoolean();
      int used = connection()->ReadInt();
      int class_map = connection()->ReadInt();
      int64 bigint_class_id = connection()->ReadInt64();
      int64 uint32_digits_class_id = connection()->ReadInt64();

      int rounded_used = (used & 1) == 0 ? used : used + 1;

      // First two arguments for _Bigint allocation.
      session()->PushBoolean(negative);
      session()->PushNewInteger(used);

      // Arguments for _Uint32Digits allocation.
      session()->PushNewInteger(rounded_used);
      GC_AND_RETRY_ON_ALLOCATION_FAILURE(
          object, program()->CreateByteArray(rounded_used * 4));
      ByteArray* backing = ByteArray::cast(object);
      for (int i = 0; i < used; i++) {
        uint32 part = static_cast<uint32>(connection()->ReadInt());
        uint8* part_address = backing->byte_address_for(i * 4);
        *(reinterpret_cast<uint32*>(part_address)) = part;
      }
      session()->Push(backing);
      // _Uint32Digits allocation.
      session()->PushFromMap(class_map, uint32_digits_class_id);
      session()->PushNewInstance();

      // _Bigint allocation.
      session()->PushFromMap(class_map, bigint_class_id);
      session()->PushNewInstance();

      break;
    }

    case Connection::kPushNewDouble: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushNewDouble(connection()->ReadDouble());
      break;
    }

    case Connection::kPushNewOneByteString: {
      ASSERT(!IsScheduled() || IsPaused());
      int length;
      uint8* bytes = connection()->ReadBytes(&length);
      List<uint8> contents(bytes, length);
      session()->PushNewOneByteString(contents);
      contents.Delete();
      break;
    }

    case Connection::kPushNewTwoByteString: {
      ASSERT(!IsScheduled() || IsPaused());
      int length;
      uint8* bytes = connection()->ReadBytes(&length);
      ASSERT((length & 1) == 0);
      List<uint16> contents(reinterpret_cast<uint16*>(bytes), length >> 1);
      session()->PushNewTwoByteString(contents);
      contents.Delete();
      break;
    }

    case Connection::kPushNewInstance: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushNewInstance();
      break;
    }

    case Connection::kPushNewArray: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushNewArray(connection()->ReadInt());
      break;
    }

    case Connection::kPushNewFunction: {
      ASSERT(!IsScheduled() || IsPaused());
      int arity = connection()->ReadInt();
      int literals = connection()->ReadInt();
      int length;
      uint8* bytes = connection()->ReadBytes(&length);
      List<uint8> bytecodes(bytes, length);
      session()->PushNewFunction(arity, literals, bytecodes);
      bytecodes.Delete();
      break;
    }

    case Connection::kPushNewInitializer: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushNewInitializer();
      break;
    }

    case Connection::kPushNewClass: {
      ASSERT(!IsScheduled() || IsPaused());
      session()->PushNewClass(connection()->ReadInt());
      break;
    }

    case Connection::kPushBuiltinClass: {
      ASSERT(!IsScheduled() || IsPaused());
      Names::Id name = static_cast<Names::Id>(connection()->ReadInt());
      int fields = connection()->ReadInt();
      session()->PushBuiltinClass(name, fields);
      break;
    }

    case Connection::kPushConstantList: {
      ASSERT(!IsScheduled() || IsPaused());
      int length = connection()->ReadInt();
      session()->PushConstantList(length);
      break;
    }

    case Connection::kPushConstantByteList: {
      ASSERT(!IsScheduled() || IsPaused());
      int length = connection()->ReadInt();
      session()->PushConstantByteList(length);
      break;
    }

    case Connection::kPushConstantMap: {
      ASSERT(!IsScheduled() || IsPaused());
      int length = connection()->ReadInt();
      session()->PushConstantMap(length);
      break;
    }

    case Connection::kMapLookup: {
      ASSERT(!IsScheduled() || IsPaused());
      int map_index = connection()->ReadInt();
      WriteBuffer buffer;
      buffer.WriteInt64(
          session()->MapLookupByObject(map_index, session()->Top()));
      connection()->Send(Connection::kObjectId, buffer);
      break;
    }

    case Connection::kProcessGetProcessIds: {
      int count = 0;
      auto processes = program()->process_list();
      for (auto process : *processes) {
        USE(process);
        ++count;
      }
      WriteBuffer buffer;
      buffer.WriteInt(count);
      for (auto process : *processes) {
        process->EnsureDebuggerAttached();
        buffer.WriteInt(process->debug_info()->process_id());
      }
      connection()->Send(Connection::kProcessGetProcessIdsResult, buffer);
      break;
    }

#ifdef DARTINO_ENABLE_LIVE_CODING
    case Connection::kSetEntryPoint: {
      program()->set_entry(Function::cast(session()->Pop()));
      break;
    }

    case Connection::kCreateSnapshot: {
      ASSERT(!IsScheduled());
      bool writeToDisk = connection()->ReadBoolean();
      int length;
      uint8* data = connection()->ReadBytes(&length);
      const char* path = reinterpret_cast<const char*>(data);
      ASSERT(static_cast<int>(strlen(path)) == length - 1);
      ASSERT(writeToDisk || (static_cast<int>(strlen(path)) == 0));
      FunctionOffsetsType function_offsets;
      ClassOffsetsType class_offsets;
      bool success = session()->CreateSnapshot(
          writeToDisk, path, &function_offsets, &class_offsets);
      free(data);
      if (success) {
        session()->SendProgramInfo(&class_offsets, &function_offsets);
      } else {
        session()->SendError(Connection::kSnapshotCreationError);
      }
      session()->SignalMainThread(
          success ? Session::kSnapshotDone : Session::kError);
      return NULL;
    }

    case Connection::kPrepareForChanges: {
      ASSERT(!IsModifying() && (!IsScheduled() || IsPaused()));
      session()->PrepareForChanges();
      return new ModifyingState();
    }

    case Connection::kLiveEditing: {
      if (!IsLiveEditingEnabled()) EnableLiveEditing();
      session()->method_map_id_ = connection()->ReadInt();
      session()->class_map_id_ = connection()->ReadInt();
      break;
    }
#endif  // DARTINO_ENABLE_LIVE_CODING

    default:
      FATALV("Unexpected message opcode %d received in state %s, "
         "live-editing: %s, debugging: %s",
         opcode, ToString(),
         IsLiveEditingEnabled() ? "true" : "false",
         IsDebuggingEnabled() ? "true" : "false");
  }

  return this;
}

int PostponedChange::number_of_changes_ = 0;

Session::Session(Connection* connection)
    : connection_(connection),
      got_handshake_(false),
      program_(NULL),
      state_(NULL),
      live_editing_(false),
      debugging_(false),
      main_process_(NULL),
      next_process_id_(0),
      method_map_id_(-1),
      class_map_id_(-1),
      fibers_map_id_(-1),
      stack_(0),
      first_change_(NULL),
      last_change_(NULL),
      has_program_update_error_(false),
      program_update_error_(NULL),
      main_thread_monitor_(Platform::CreateMonitor()),
      main_thread_resume_kind_(kUnknown) {
#ifdef DARTINO_ENABLE_PRINT_INTERCEPTORS
  ConnectionPrintInterceptor* interceptor =
      new ConnectionPrintInterceptor(connection_);
  Print::RegisterPrintInterceptor(interceptor);
#endif  // DARTINO_ENABLE_PRINT_INTERCEPTORS
}

Session::~Session() {
#ifdef DARTINO_ENABLE_PRINT_INTERCEPTORS
  Print::UnregisterPrintInterceptors();
#endif  // DARTINO_ENABLE_PRINT_INTERCEPTORS

  for (int i = 0; i < maps_.length(); ++i) delete maps_[i];
  maps_.Delete();
  delete main_thread_monitor_;
  delete state_;
  delete connection_;
}

void Session::Initialize(Program *program) {
  state_ = new InitialState(this);
  program_ = program;
  program_->AddSession(this);
}

static void* MessageProcessingThread(void* data) {
  Session* session = reinterpret_cast<Session*>(data);
  session->ProcessMessages();
  return NULL;
}

void Session::StartMessageProcessingThread() {
  message_handling_thread_ = Thread::Run(MessageProcessingThread, this);
}

void Session::JoinMessageProcessingThread() { message_handling_thread_.Join(); }

int64_t Session::PopInteger() {
  Object* top = Pop();
  if (top->IsLargeInteger()) {
    return LargeInteger::cast(top)->value();
  }
  return Smi::cast(top)->value();
}

void Session::SignalMainThread(MainThreadResumeKind kind) {
  while (main_thread_resume_kind_ != kUnknown) main_thread_monitor_->Wait();
  main_thread_resume_kind_ = kind;
  main_thread_monitor_->NotifyAll();
}

void Session::SendError(Connection::ErrorCode errorCode) {
  WriteBuffer buffer;
  buffer.WriteInt(errorCode);
  connection_->Send(Connection::kCommandError, buffer);
}

void Session::SendDartValue(Object* value) {
  WriteBuffer buffer;
  if (value->IsSmi() || value->IsLargeInteger()) {
    int64_t int_value = value->IsSmi() ? Smi::cast(value)->value()
                                       : LargeInteger::cast(value)->value();
    buffer.WriteInt64(int_value);
    connection_->Send(Connection::kInteger, buffer);
  } else if (value->IsTrue() || value->IsFalse()) {
    buffer.WriteBoolean(value->IsTrue());
    connection_->Send(Connection::kBoolean, buffer);
  } else if (value->IsNull()) {
    connection_->Send(Connection::kNull, buffer);
  } else if (value->IsDouble()) {
    buffer.WriteDouble(Double::cast(value)->value());
    connection_->Send(Connection::kDouble, buffer);
  } else if (value->IsOneByteString()) {
    // TODO(ager): We should send the character data as 8-bit values
    // instead of 32-bit values.
    OneByteString* str = OneByteString::cast(value);
    for (int i = 0; i < str->length(); i++) {
      buffer.WriteInt(str->get_char_code(i));
    }
    connection_->Send(Connection::kString, buffer);
  } else if (value->IsClass()) {
    buffer.WriteInt64(ClassMessage(Class::cast(value)));
    connection_->Send(Connection::kClass, buffer);
  } else if (value->IsTwoByteString()) {
    // TODO(ager): We should send the character data as 16-bit values
    // instead of 32-bit values.
    TwoByteString* str = TwoByteString::cast(value);
    for (int i = 0; i < str->length(); i++) {
      buffer.WriteInt(str->get_code_unit(i));
    }
    connection_->Send(Connection::kString, buffer);
  } else if (value->IsArray()) {
    Array* array = Array::cast(value);
    buffer.WriteInt(array->length());
    connection_->Send(Connection::kArray, buffer);
  } else {
    ASSERT(value->IsInstance());
    buffer.WriteInt64(ClassMessage(HeapObject::cast(value)->get_class()));
    connection_->Send(Connection::kInstance, buffer);
  }
}

void Session::SendInstanceStructure(Instance* instance) {
  WriteBuffer buffer;
  Class* klass = instance->get_class();
  buffer.WriteInt64(ClassMessage(klass));
  int fields = klass->NumberOfInstanceFields();
  buffer.WriteInt(fields);
  connection_->Send(Connection::kInstanceStructure, buffer);
  for (int i = 0; i < fields; i++) {
    SendDartValue(instance->GetInstanceField(i));
  }
}

void Session::SendArrayStructure(Array* array) {
  int length = array->length();
  WriteBuffer buffer;
  buffer.WriteInt(length);
  connection_->Send(Connection::kArrayStructure, buffer);
  for (int i = 0; i < length; i++) {
    SendDartValue(array->get(i));
  }
}

void Session::SendStructure(Object* object) {
  if (object->IsArray()) {
    SendArrayStructure(Array::cast(object));
  } else if (object->IsInstance()) {
    SendInstanceStructure(Instance::cast(object));
  } else {
    SendDartValue(object);
  }
}

void Session::SendProgramInfo(ClassOffsetsType* class_offsets,
    FunctionOffsetsType* function_offsets) {
  ASSERT(maps_[class_map_id_] != NULL);
  ASSERT(maps_[method_map_id_] != NULL);

  WriteBuffer buffer;

  // Write the hashtag for the program.
  buffer.WriteInt(program()->snapshot_hash());

  // Class offset table
  buffer.WriteInt(5 * class_offsets->size());
  for (auto& pair : *class_offsets) {
    Class* klass = pair.first;
    const PortableSize& offset = pair.second;

    int id = MapLookupByObject(class_map_id_, klass);
    ASSERT(id != -1 || IsBuiltin(klass, program()));
    buffer.WriteInt(id);
    buffer.WriteInt(offset.ComputeSizeInBytes(kBigPointerBigFloat));
    buffer.WriteInt(offset.ComputeSizeInBytes(kBigPointerSmallFloat));
    buffer.WriteInt(offset.ComputeSizeInBytes(kSmallPointerBigFloat));
    buffer.WriteInt(offset.ComputeSizeInBytes(kSmallPointerSmallFloat));
  }

  // Function offset table
  buffer.WriteInt(5 * function_offsets->size());
  for (auto& pair : *function_offsets) {
    Function* function = pair.first;
    const PortableSize& offset = pair.second;

    int id = MapLookupByObject(method_map_id_, function);
    ASSERT(id != -1);
    buffer.WriteInt(id);
    buffer.WriteInt(offset.ComputeSizeInBytes(kBigPointerBigFloat));
    buffer.WriteInt(offset.ComputeSizeInBytes(kBigPointerSmallFloat));
    buffer.WriteInt(offset.ComputeSizeInBytes(kSmallPointerBigFloat));
    buffer.WriteInt(offset.ComputeSizeInBytes(kSmallPointerSmallFloat));
  }

  connection_->Send(Connection::kProgramInfo, buffer);
}

// Caller thread must have a lock on main_thread_monitor_, ie,
// this should only be called from within ProcessMessage's main loop.
void Session::PauseExecution() {
  ASSERT(state_->IsScheduled() && !state_->IsPaused());
  Scheduler* scheduler = program()->scheduler();
  scheduler->StopProgram(program(), ProgramState::kSession);
}

// Caller thread must have a lock on main_thread_monitor_, ie,
// this should only be called from within ProcessMessage's main loop.
void Session::ResumeExecution() {
  Scheduler* scheduler = program()->scheduler();
  scheduler->ResumeProgram(program(), ProgramState::kSession);
}

void Session::SendStackTrace(Stack* stack) {
  int frames = PushStackFrames(stack);
  WriteBuffer buffer;
  buffer.WriteInt(frames);
  for (int i = 0; i < frames; i++) {
    // Lookup method in method map and send id.
    buffer.WriteInt64(FunctionMessage(Function::cast(Pop())));
    // Pop bytecode index from session stack and send it.
    buffer.WriteInt64(PopInteger());
  }
  connection_->Send(Connection::kProcessBacktrace, buffer);
}

void Session::ProcessMessages() {
  ASSERT(state_->IsInitial());
  while (true) {
    Connection::Opcode opcode = connection_->Receive();
    ScopedMonitorLock scoped_lock(main_thread_monitor_);
    SessionState* next_state = state_->ProcessMessage(opcode);
    if (next_state == NULL) return;
    ChangeState(next_state);
    if (next_state->IsTerminating()) return;
  }
}

#ifdef DARTINO_ENABLE_LIVE_CODING

SessionState* ModifyingState::ProcessMessage(Connection::Opcode opcode) {
  switch (opcode) {
    case Connection::kCollectGarbage: {
      program()->CollectGarbage();
      break;
    }

    case Connection::kChangeSuperClass: {
      session()->ChangeSuperClass();
      break;
    }

    case Connection::kChangeMethodTable: {
      session()->ChangeMethodTable(connection()->ReadInt());
      break;
    }

    case Connection::kChangeMethodLiteral: {
      session()->ChangeMethodLiteral(connection()->ReadInt());
      break;
    }

    case Connection::kChangeStatics: {
      session()->ChangeStatics(connection()->ReadInt());
      break;
    }

    case Connection::kChangeSchemas: {
      int count = connection()->ReadInt();
      int delta = connection()->ReadInt();
      session()->ChangeSchemas(count, delta);
      break;
    }

    case Connection::kCommitChanges: {
      bool success = session()->CommitChanges(connection()->ReadInt());
      WriteBuffer buffer;
      buffer.WriteBoolean(success);
      if (success) {
        buffer.WriteString("Successfully applied program update.");
      } else {
        if (session()->program_update_error_ != NULL) {
          buffer.WriteString(session()->program_update_error_);
        } else {
          buffer.WriteString(
              "An unknown error occurred during program update.");
        }
      }
      connection()->Send(Connection::kCommitChangesResult, buffer);
      return RestorePrevious();
    }

    case Connection::kDiscardChanges: {
      session()->DiscardChanges();
      return RestorePrevious();
    }

    default: {
      return SessionState::ProcessMessage(opcode);
    }
  }

  return this;
}
#endif  // DARTINO_ENABLE_LIVE_CODING

SessionState* SpawnedState::ProcessMessage(Connection::Opcode opcode) {
  switch (opcode) {
    case Connection::kProcessRun: {
      session()->SignalMainThread(Session::kProcessRun);
      break;
    }

    // Debugging commands that are valid on unscheduled programs.
    case Connection::kProcessDebugInterrupt: {
      // TODO(zerny): Disallow an interrupt on unscheduled programs?
      // This is a noop in all non-running states.
      break;
    }

    case Connection::kProcessSetBreakpoint: {
      ASSERT(IsDebuggingEnabled());
      WriteBuffer buffer;
      int bytecode_index = connection()->ReadInt();
      Function* function = Function::cast(session()->Pop());
      ProgramDebugInfo* debug_info = program()->debug_info();
      int id = debug_info->CreateBreakpoint(function, bytecode_index);
      buffer.WriteInt(id);
      connection()->Send(Connection::kProcessSetBreakpoint, buffer);
      break;
    }

    case Connection::kProcessDeleteBreakpoint: {
      ASSERT(IsDebuggingEnabled());
      WriteBuffer buffer;
      int id = connection()->ReadInt();
      bool deleted = program()->debug_info()->DeleteBreakpoint(id);
      ASSERT(deleted);
      buffer.WriteInt(id);
      connection()->Send(Connection::kProcessDeleteBreakpoint, buffer);
      break;
    }
    // End of debugging commands that are valid on unscheduled programs.

    default: {
      return SessionState::ProcessMessage(opcode);
    }
  }
  return this;
}

SessionState* RunningState::ProcessMessage(Connection::Opcode opcode) {
  switch (opcode) {
    case Connection::kProcessDebugInterrupt: {
      session()->PauseExecution();
      // TODO(zerny): Heuristically select a "paused" process.
      Process* paused_process = main_process();
      SendBreakpoint(paused_process);
      return new PausedState(paused_process, /* interrupted */ true);
    }

    default: {
      return ScheduledState::ProcessMessage(opcode);
    }
  }
  return this;
}

SessionState* PausedState::ProcessMessage(Connection::Opcode opcode) {
  process()->EnsureDebuggerAttached();
  ASSERT(!process()->debug_info()->is_stepping());
  switch (opcode) {
    case Connection::kProcessDeleteOneShotBreakpoint: {
      WriteBuffer buffer;
      int process_id = connection()->ReadInt();
      int breakpoint_id = connection()->ReadInt();
      Process* process = session()->GetProcess(process_id);
      bool deleted = process->debug_info()->DeleteBreakpoint(breakpoint_id);
      ASSERT(deleted);
      buffer.WriteInt(breakpoint_id);
      connection()->Send(Connection::kProcessDeleteBreakpoint, buffer);
      break;
    }

    case Connection::kSessionEnd: {
      Process::State process_state = process()->state();
      program()->scheduler()->KillProgram(program());
      process()->set_exception(program()->null_object());
      // Continue execution, but change to the terminating state.
      if (!interrupted_) scheduler()->ContinueProcess(process());
      return new TerminatingState(process_state);
    }

    case Connection::kProcessStep: {
      process()->debug_info()->SetStepping();
      return ProcessContinue();
    }

    case Connection::kProcessStepOver: {
      int breakpoint_id = process()->PrepareStepOver();
      WriteBuffer buffer;
      buffer.WriteInt(breakpoint_id);
      connection()->Send(Connection::kProcessSetBreakpoint, buffer);
      return ProcessContinue();
    }

    case Connection::kProcessStepOut: {
      int breakpoint_id = process()->PrepareStepOut();
      WriteBuffer buffer;
      buffer.WriteInt(breakpoint_id);
      connection()->Send(Connection::kProcessSetBreakpoint, buffer);
      return ProcessContinue();
    }

    case Connection::kProcessStepTo: {
      int bcp = connection()->ReadInt();
      Function* function = Function::cast(session()->Pop());
      ProcessDebugInfo* debug_info = process()->debug_info();
      debug_info->CreateBreakpoint(function, bcp);
      return ProcessContinue();
    }

    case Connection::kProcessContinue: {
      return ProcessContinue();
    }

    case Connection::kProcessBacktraceRequest: {
      int process_id = connection()->ReadInt() - 1;
      Process* process = session()->GetProcess(process_id);
      Stack* stack = process->stack();
      session()->SendStackTrace(stack);
      break;
    }

    case Connection::kProcessFiberBacktraceRequest: {
      int64 fiber_id = connection()->ReadInt64();
      Stack* stack = Stack::cast(session()->MapLookupById(
          session()->fibers_map_id_, fiber_id));
      session()->SendStackTrace(stack);
      break;
    }

    case Connection::kProcessUncaughtExceptionRequest: {
      Object* exception = process()->exception();
      session()->SendStructure(exception);
      break;
    }

    case Connection::kProcessInstance:
    case Connection::kProcessInstanceStructure: {
      int frame_index = connection()->ReadInt();
      int slot = connection()->ReadInt();
      int fieldAccessCount = connection()->ReadInt();
      Stack* stack = process()->stack();
      Frame frame(stack);
      for (int i = 0; i <= frame_index; i++) frame.MovePrevious();
      word index = frame.FirstLocalIndex() - slot;
      if (index < frame.LastLocalIndex()) {
        session()->SendError(Connection::kInvalidInstanceAccess);
        break;
      }
      Object* object = stack->get(index);
      for (int i = 0; i < fieldAccessCount; i++) {
        if (object->IsArray()) {
          Array* array = Array::cast(object);
          int index = connection()->ReadInt();
          if (index < 0 || index >= array->length()) {
            session()->SendError(Connection::kInvalidInstanceAccess);
            break;
          }
          object = array->get(index);
        } else if (object->IsInstance()) {
          Instance* instance = Instance::cast(object);
          Class* klass = instance->get_class();
          int fieldAccess = connection()->ReadInt();
          if (fieldAccess < 0 ||
             fieldAccess >= klass->NumberOfInstanceFields()) {
            session()->SendError(Connection::kInvalidInstanceAccess);
            break;
          }
          object = instance->GetInstanceField(fieldAccess);
        } else {
          session()->SendError(Connection::kInvalidInstanceAccess);
          break;
        }
      }
      if (opcode == Connection::kProcessInstanceStructure) {
        session()->SendStructure(object);
      } else {
        session()->SendDartValue(object);
      }
      break;
    }

    case Connection::kProcessRestartFrame: {
      int frame_index = connection()->ReadInt();
      RestartFrame(frame_index);
      process()->set_exception(program()->null_object());
      ProcessDebugInfo* debug_info = process()->debug_info();
      if (debug_info != NULL && debug_info->is_at_breakpoint()) {
        debug_info->ClearCurrentBreakpoint();
      }
      return ProcessContinue();
    }

    case Connection::kProcessAddFibersToMap: {
      // TODO(ager): Potentially optimize this to not require a full
      // process GC to locate the live stacks?
      int number_of_stacks = program()->CollectMutableGarbageAndChainStacks();
      Object* current = program()->stack_chain();
      for (int i = 0; i < number_of_stacks; i++) {
        Stack* stack = Stack::cast(current);
        session()->AddToMap(session()->fibers_map_id_, i, stack);
        current = stack->next();
        // Unchain stacks.
        stack->set_next(Smi::FromWord(0));
      }
      ASSERT(current == NULL);
      program()->ClearStackChain();
      WriteBuffer buffer;
      buffer.WriteInt(number_of_stacks);
      connection()->Send(Connection::kProcessNumberOfStacks, buffer);
      break;
    }

    default: {
      return ScheduledState::ProcessMessage(opcode);
    }
  }
  return this;
}

SessionState* TerminatedState::ProcessMessage(Connection::Opcode opcode) {
  if (opcode == Connection::kSessionEnd) {
    session()->SignalMainThread(Session::kSessionEnd);
    return NULL;
  }
  return SessionState::ProcessMessage(opcode);
}

void Session::IterateChangesPointers(PointerVisitor* visitor) {
  for (PostponedChange* current = first_change_; current != NULL;
       current = current->next()) {
    current->IteratePointers(visitor);
  }
}

void Session::IteratePointers(PointerVisitor* visitor) {
  stack_.IteratePointers(visitor);
  IterateChangesPointers(visitor);
  for (int i = 0; i < maps_.length(); ++i) {
    ObjectMap* map = maps_[i];
    if (map != NULL) {
      // TODO(kasperl): Move the table clearing to a GC pre-phase.
      map->ClearTableByObject();
      map->IteratePointers(visitor);
    }
  }
}

int Session::ProcessRun() {
  bool process_started = false;
  ScopedMonitorLock scoped_lock(main_thread_monitor_);
  while (true) {
    if (state_->IsTerminated()) {
      return static_cast<TerminatedState*>(state_)->exit_code();
    }
    MainThreadResumeKind resume_kind;
    while (main_thread_resume_kind_ == kUnknown) main_thread_monitor_->Wait();
    resume_kind = main_thread_resume_kind_;
    main_thread_resume_kind_ = kUnknown;
    main_thread_monitor_->NotifyAll();
    switch (resume_kind) {
      case kError: {
        ChangeState(new TerminatedState(kUncaughtExceptionExitCode));
        break;
      }
      case kSnapshotDone: {
        ChangeState(new TerminatedState(0));
        break;
      }
      case kProcessRun: {
        ASSERT(main_process_ != NULL);
        process_started = true;
        int result = -1;
        ChangeState(new RunningState());
        {
          SimpleProgramRunner runner;

          Program* programs[1] = { program_ };
          Process* processes[1] = { main_process_ };
          int exitcodes[1] = { -1 };

          ScopedMonitorUnlock scoped_unlock(main_thread_monitor_);
          // TODO(ajohnsen): Arguments?
          runner.Run(1, exitcodes, programs, 0, NULL, processes);

          result = exitcodes[0];
          ASSERT(result != -1);
        }
        ASSERT(state_->IsScheduled());
        ChangeState(new TerminatedState(result));
        break;
      }
      case kSessionEnd: {
        ASSERT(!process_started);
        ASSERT(!state_->IsScheduled());
        // If the process was spawned but not started, the scheduler does not
        // know about it and we are therefore responsible for deleting it.
        if (state_->IsSpawned()) {
          main_process_->ChangeState(
              Process::kSleeping, Process::kWaitingForChildren);
          program_->ScheduleProcessForDeletion(
              main_process_, Signal::kTerminated);
        }
        ChangeState(new TerminatedState(0));
        break;
      }
      default: {
        UNREACHABLE();
        break;
      }
    }
  }
  UNREACHABLE();
  return -1;
}

bool Session::CreateSnapshot(
    bool writeToDisk,
    const char* path,
    FunctionOffsetsType* function_offsets,
    ClassOffsetsType* class_offsets) {
  // Make sure that the program is in the compact form before
  // snapshotting.
  if (!program()->is_optimized()) {
    ProgramFolder program_folder(program());
    program_folder.Fold();
  }

  program()->VerifyObjectPlacements();

  SnapshotWriter writer(function_offsets, class_offsets);
  List<uint8> snapshot = writer.WriteProgram(program());
  bool success = writeToDisk
      ? Platform::StoreFile(path, snapshot)
      : true;
  snapshot.Delete();
  return success;
}

void Session::NewMap(int map_index) {
  int length = maps_.length();
  if (map_index >= length) {
    maps_.Reallocate(map_index + 1);
    for (int i = length; i <= map_index; i++) {
      maps_[i] = NULL;
    }
  }
  ObjectMap* existing = maps_[map_index];
  if (existing != NULL) delete existing;
  maps_[map_index] = new ObjectMap(64);
}

void Session::DeleteMap(int map_index) {
  ObjectMap* map = maps_[map_index];
  if (map == NULL) return;
  delete map;
  maps_[map_index] = NULL;
}

void Session::PushFromMap(int map_index, int64 id) {
  bool entry_exists;
  Object* object;
  if (program()->was_loaded_from_snapshot()) {
    uword offset = id;
    object = program()->ObjectAtOffset(offset);
  } else {
    object = maps_[map_index]->LookupById(id, &entry_exists);
    if (!entry_exists && !has_program_update_error_) {
      has_program_update_error_ = true;
      program_update_error_ =
          "Received PushFromMap command which refers to a "
          "non-existent map entry.";
    }
  }
  Push(object);
}

void Session::PopToMap(int map_index, int64 id) {
  ASSERT(!program()->was_loaded_from_snapshot());
  maps_[map_index]->Add(id, Pop());
}

void Session::AddToMap(int map_index, int64 id, Object* value) {
  ASSERT(maps_[map_index] != NULL);
  maps_[map_index]->Add(id, value);
}

void Session::RemoveFromMap(int map_index, int64 id) {
  ASSERT(!program()->was_loaded_from_snapshot());
  maps_[map_index]->RemoveById(id);
}

int64 Session::MapLookupByObject(int map_index, Object* object) {
  int64 id = -1;
  ObjectMap* map = maps_[map_index];
  if (map != NULL) id = map->LookupByObject(object);
  return id;
}

Object* Session::MapLookupById(int map_index, int64 id) {
  return maps_[map_index]->LookupById(id);
}

void Session::PushNull() {
  Push(program()->null_object());
}

void Session::PushBoolean(bool value) {
  if (value) {
    Push(program()->true_object());
  } else {
    Push(program()->false_object());
  }
}

void Session::PushNewInteger(int64 value) {
  if (Smi::IsValid(value)) {
    Push(Smi::FromWord(value));
  } else {
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(result, program()->CreateInteger(value));
    Push(result);
  }
}

void Session::PushNewDouble(double value) {
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(result, program()->CreateDouble(value));
  Push(result);
}

void Session::PushNewOneByteString(List<uint8> contents) {
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(result,
                                     program()->CreateOneByteString(contents));
  Push(result);
}

void Session::PushNewTwoByteString(List<uint16> contents) {
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(result,
                                     program()->CreateTwoByteString(contents));
  Push(result);
}

void Session::PushNewInstance() {
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(
      result, program()->CreateInstance(Class::cast(Top())));
  Class* klass = Class::cast(Pop());
  Instance* instance = Instance::cast(result);
  int fields = klass->NumberOfInstanceFields();
  for (int i = fields - 1; i >= 0; i--) {
    instance->SetInstanceField(i, Pop());
  }
  Push(instance);
}

void Session::PushNewArray(int length) {
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(result, program()->CreateArray(length));
  Array* array = Array::cast(result);
  for (int i = length - 1; i >= 0; i--) {
    array->set(i, Pop());
  }
  Push(array);
}

static void RewriteLiteralIndicesToOffsets(Function* function) {
  uint8_t* bcp = function->bytecode_address_for(0);

  while (true) {
    Opcode opcode = static_cast<Opcode>(*bcp);

    switch (opcode) {
      case kInvokeStatic:
      case kInvokeFactory:
      case kLoadConst:
      case kAllocate:
      case kAllocateImmutable: {
        int literal_index = Utils::ReadInt32(bcp + 1);
        Object** literal_address = function->literal_address_for(literal_index);
        int offset = reinterpret_cast<uint8_t*>(literal_address) - bcp;
        Utils::WriteInt32(bcp + 1, offset);
        break;
      }
      case kMethodEnd:
        return;
      default:
        ASSERT(opcode < Bytecode::kNumBytecodes);
        break;
    }

    bcp += Bytecode::Size(opcode);
  }

  UNREACHABLE();
}

void Session::PushNewFunction(int arity, int literals, List<uint8> bytecodes) {
  ASSERT(!program()->is_optimized());

  GC_AND_RETRY_ON_ALLOCATION_FAILURE(
      result, program()->CreateFunction(arity, bytecodes, literals));
  Function* function = Function::cast(result);
  for (int i = literals - 1; i >= 0; --i) {
    function->set_literal_at(i, Pop());
  }
  RewriteLiteralIndicesToOffsets(function);
  Push(function);

  if (Flags::log_decoder) {
    uint8* bytes = function->bytecode_address_for(0);
    Print::Out("Method: %p\n", bytes);
    Opcode opcode;
    int i = 0;
    do {
      opcode = static_cast<Opcode>(bytes[i]);
      Print::Out("  %04d: ", i);
      i += Bytecode::Print(bytes + i);
      Print::Out("\n");
    } while (opcode != kMethodEnd);
  }
}

void Session::PushNewInitializer() {
  ASSERT(!state_->IsScheduled() || state_->IsPaused());
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(
      result, program()->CreateInitializer(Function::cast(Top())));
  Pop();
  Push(result);
}

void Session::PushNewClass(int fields) {
  ASSERT(!state_->IsScheduled() || state_->IsPaused());
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(result, program()->CreateClass(fields));
  Push(result);
}

void Session::PushBuiltinClass(Names::Id name, int fields) {
  Class* klass = NULL;
  if (name == Names::kObject) {
    klass = program()->object_class();
  } else if (name == Names::kTearOffClosure) {
    klass = program()->closure_class();
  } else if (name == Names::kBool) {
    klass = program()->bool_class();
  } else if (name == Names::kNull) {
    klass = program()->null_object()->get_class();
  } else if (name == Names::kDouble) {
    klass = program()->double_class();
  } else if (name == Names::kSmi) {
    klass = program()->smi_class();
  } else if (name == Names::kInt) {
    klass = program()->int_class();
  } else if (name == Names::kMint) {
    klass = program()->large_integer_class();
  } else if (name == Names::kConstantList) {
    klass = program()->constant_list_class();
  } else if (name == Names::kConstantByteList) {
    klass = program()->constant_byte_list_class();
  } else if (name == Names::kConstantMap) {
    klass = program()->constant_map_class();
  } else if (name == Names::kNum) {
    klass = program()->num_class();
  } else if (name == Names::kOneByteString) {
    klass = program()->one_byte_string_class();
  } else if (name == Names::kTwoByteString) {
    klass = program()->two_byte_string_class();
  } else if (name == Names::kCoroutine) {
    klass = program()->coroutine_class();
  } else if (name == Names::kPort) {
    klass = program()->port_class();
  } else if (name == Names::kProcess) {
    klass = program()->process_class();
  } else if (name == Names::kProcessDeath) {
    klass = program()->process_death_class();
  } else if (name == Names::kForeignMemory) {
    klass = program()->foreign_memory_class();
  } else if (name == Names::kStackOverflowError) {
    klass = program()->stack_overflow_error_class();
  } else if (name == Names::kDartinoNoSuchMethodError) {
    klass = program()->no_such_method_error_class();
  } else {
    UNREACHABLE();
  }

  ASSERT(klass->instance_format().type() != InstanceFormat::INSTANCE_TYPE ||
         klass->NumberOfInstanceFields() == fields);

  Push(klass);
}

void Session::PushConstantList(int length) {
  PushNewArray(length);
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(
      result, program()->CreateInstance(program()->constant_list_class()));
  Instance* list = Instance::cast(result);
  ASSERT(list->get_class()->NumberOfInstanceFields() == 1);
  list->SetInstanceField(0, Pop());
  Push(list);
}

void Session::PushConstantByteList(int length) {
  {
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(result,
                                       program()->CreateByteArray(length));
    ByteArray* array = ByteArray::cast(result);
    for (int i = length - 1; i >= 0; i--) {
      array->set(i, Smi::cast(Pop())->value());
    }
    Push(array);
  }

  {
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(
        result,
        program()->CreateInstance(program()->constant_byte_list_class()));
    Instance* list = Instance::cast(result);
    ASSERT(list->get_class()->NumberOfInstanceFields() == 1);
    list->SetInstanceField(0, Pop());
    Push(list);
  }
}

void Session::PushConstantMap(int length) {
  GC_AND_RETRY_ON_ALLOCATION_FAILURE(
      result, program()->CreateInstance(program()->constant_map_class()));
  Instance* map = Instance::cast(result);
  ASSERT(map->get_class()->NumberOfInstanceFields() == 2);
  // Values.
  map->SetInstanceField(1, Pop());
  // Keys.
  map->SetInstanceField(0, Pop());
  Push(map);
}

void Session::PrepareForChanges() {
  ASSERT(!state_->IsScheduled() || state_->IsPaused());
  if (program()->is_optimized()) {
    ProgramFolder program_folder(program());
    program_folder.Unfold();
    for (int i = 0; i < maps_.length(); ++i) {
      ObjectMap* map = maps_[i];
      if (map != NULL) {
        map->ClearTableByObject();
      }
    }
  }
}

void Session::ChangeSuperClass() {
  PostponeChange(kChangeSuperClass, 2);
}

void Session::CommitChangeSuperClass(PostponedChange* change) {
  ASSERT(state_->IsModifying());
  Class* klass = Class::cast(change->get(1));
  Class* super = Class::cast(change->get(2));
  klass->set_super_class(super);
}

void Session::ChangeMethodTable(int length) {
  PushNewArray(length * 2);
  PostponeChange(kChangeMethodTable, 2);
}

void Session::CommitChangeMethodTable(PostponedChange* change) {
  ASSERT(state_->IsModifying());
  Class* clazz = Class::cast(change->get(1));
  Array* methods = Array::cast(change->get(2));
  clazz->set_methods(methods);
}

void Session::ChangeMethodLiteral(int index) {
  Push(Smi::FromWord(index));
  PostponeChange(kChangeMethodLiteral, 3);
}

void Session::CommitChangeMethodLiteral(PostponedChange* change) {
  ASSERT(state_->IsModifying());
  Function* function = Function::cast(change->get(1));
  Object* literal = change->get(2);
  int index = Smi::cast(change->get(3))->value();
  function->set_literal_at(index, literal);
}

void Session::ChangeStatics(int count) {
  PushNewArray(count);
  PostponeChange(kChangeStatics, 1);
}

void Session::CommitChangeStatics(PostponedChange* change) {
  ASSERT(state_->IsModifying());
  program()->set_static_fields(Array::cast(change->get(1)));
}

void Session::ChangeSchemas(int count, int delta) {
  // Stack: <count> classes + transformation array
  Push(Smi::FromWord(delta));
  PostponeChange(kChangeSchemas, count + 2);
}

void Session::CommitChangeSchemas(PostponedChange* change) {
  ASSERT(state_->IsModifying());
  // TODO(kasperl): Rework this so we can allow allocation failures
  // as part of allocating the new classes.
  SemiSpace* space = program()->heap()->space();
  NoAllocationFailureScope scope(space);

  int length = change->size();
  Array* transformation = Array::cast(change->get(length - 2));
  int delta = Smi::cast(change->get(length - 1))->value();
  for (int i = 1; i < length - 2; i++) {
    Class* original = Class::cast(change->get(i));
    int fields = original->NumberOfInstanceFields() + delta;
    Class* target = Class::cast(program()->CreateClass(fields));
    target->set_super_class(original->super_class());
    target->set_methods(original->methods());
    original->Transform(target, transformation);
  }
}

bool Session::CommitChanges(int count) {
  ASSERT(state_->IsModifying());
  ASSERT(!program()->is_optimized());

  // Free up as much space as possible so we don't run out of space when
  // transforming instances.
  program()->CollectNewSpace();
  program()->CollectNewSpace();
  program()->PerformSharedGarbageCollection();

  if (count != PostponedChange::number_of_changes()) {
    if (!has_program_update_error_) {
      has_program_update_error_ = true;
      program_update_error_ =
          "The CommitChanges command had a different count of changes than "
          "the buffered changes.";
    }
    DiscardChanges();
  }

  if (!has_program_update_error_) {
    program()->ClearCache();

    // TODO(kustermann): Sanity check all changes the compiler gave us.
    // If we are unable to apply a change, we should continue the program
    // and "return false".

    bool schemas_changed = false;
    for (PostponedChange* current = first_change_; current != NULL;
         current = current->next()) {
      Change change = static_cast<Change>(Smi::cast(current->get(0))->value());
      switch (change) {
        case kChangeSuperClass:
          ASSERT(!schemas_changed);
          CommitChangeSuperClass(current);
          break;
        case kChangeMethodTable:
          ASSERT(!schemas_changed);
          CommitChangeMethodTable(current);
          break;
        case kChangeMethodLiteral:
          CommitChangeMethodLiteral(current);
          break;
        case kChangeStatics:
          CommitChangeStatics(current);
          break;
        case kChangeSchemas:
          CommitChangeSchemas(current);
          schemas_changed = true;
          break;
        default:
          UNREACHABLE();
          break;
      }
    }

    DiscardChanges();

    if (schemas_changed) TransformInstances();

    // Fold the program after applying changes to continue running in the
    // optimized compact form.
    {
      ProgramFolder program_folder(program());
      program_folder.Fold();
    }
  }

  return !has_program_update_error_;
}

void Session::DiscardChanges() {
  ASSERT(state_->IsModifying());
  PostponedChange* current = first_change_;
  while (current != NULL) {
    PostponedChange* next = current->next();
    delete current;
    current = next;
  }
  first_change_ = last_change_ = NULL;
  ASSERT(PostponedChange::number_of_changes() == 0);
}

void Session::PostponeChange(Change change, int count) {
  ASSERT(!state_->IsScheduled() || state_->IsPaused());
  Object** description = new Object*[count + 1];
  description[0] = Smi::FromWord(change);
  for (int i = count; i >= 1; i--) {
    description[i] = Pop();
  }
  PostponedChange* postponed_change =
      new PostponedChange(description, count + 1);
  if (last_change_ != NULL) {
    last_change_->set_next(postponed_change);
    last_change_ = postponed_change;
  } else {
    last_change_ = first_change_ = postponed_change;
  }
}

bool Session::CanHandleEvents() const {
  return got_handshake();
}

Scheduler::ProcessInterruptionEvent Session::CheckForPauseEventResult(
    Process* process,
    Scheduler::ProcessInterruptionEvent result) {
  if (result == Scheduler::kRemainPaused) {
    ChangeState(new PausedState(process));
  }
  return result;
}

static Scheduler::ProcessInterruptionEvent AssertExitEventResult(
    Scheduler::ProcessInterruptionEvent result) {
  ASSERT(result != Scheduler::kRemainPaused);
  return result;
}

Scheduler::ProcessInterruptionEvent Session::UncaughtException(
    Process* process) {
  ScopedMonitorLock scoped_lock(main_thread_monitor_);
  return CheckForPauseEventResult(
      process, state_->HandleUncaughtException(process));
}

Scheduler::ProcessInterruptionEvent Session::Killed(Process* process) {
  ScopedMonitorLock scoped_lock(main_thread_monitor_);
  return AssertExitEventResult(state_->HandleKilled(process));
}

Scheduler::ProcessInterruptionEvent Session::UnhandledSignal(
    Process* process) {
  ScopedMonitorLock scoped_lock(main_thread_monitor_);
  return AssertExitEventResult(state_->HandleUnhandledSignal(process));
}

Scheduler::ProcessInterruptionEvent Session::Breakpoint(Process* process) {
  ScopedMonitorLock scoped_lock(main_thread_monitor_);
  return CheckForPauseEventResult(
      process, state_->HandleBreakpoint(process));
}

Scheduler::ProcessInterruptionEvent Session::ProcessTerminated(
    Process* process) {
  ScopedMonitorLock scoped_lock(main_thread_monitor_);
  return AssertExitEventResult(state_->HandleTerminated(process));
}

Scheduler::ProcessInterruptionEvent Session::CompileTimeError(
    Process* process) {
  ScopedMonitorLock scoped_lock(main_thread_monitor_);
  return CheckForPauseEventResult(
      process, state_->HandleCompileTimeError(process));
}

Process* Session::GetProcess(int process_id) {
  ASSERT(!state_->IsScheduled() || state_->IsPaused());
  // TODO(zerny): Assert here and eliminate the default process.
  if (process_id < 0) return main_process_;

  for (auto process : *program()->process_list()) {
    ProcessDebugInfo* debug_info = process->debug_info();
    if (debug_info == NULL) continue;
    if (debug_info->process_id() == process_id) {
      return process;
    }
  }
  UNREACHABLE();
  return NULL;
}

void Session::ChangeState(SessionState* new_state) {
  if (state_ == new_state) return;
  ASSERT(!state_->IsTerminated());
  SessionState* previous_state = state_;
  state_ = new_state;
  new_state->ActivateState(previous_state);
}

class TransformInstancesPointerVisitor : public PointerVisitor {
 public:
  explicit TransformInstancesPointerVisitor(Heap* heap)
      : heap_(heap) {}

  virtual void VisitClass(Object** p) {
    // The class pointer in the header of an object should not
    // be updated even if it points to a transformed class. If we
    // updated the pointer, the new class would have the wrong
    // instance format for the non-transformed, old instance.
    current_object_address_ = reinterpret_cast<uword>(p);
  }

  virtual void VisitBlock(Object** start, Object** end) {
    for (Object** p = start; p < end; p++) {
      Object* object = *p;
      if (!object->IsHeapObject()) continue;
      HeapObject* heap_object = HeapObject::cast(object);
      if (heap_object->HasForwardingAddress()) {
        *p = heap_object->forwarding_address();
      } else if (heap_object->IsInstance()) {
        Instance* instance = Instance::cast(heap_object);
        if (instance->get_class()->IsTransformed()) {
          Instance* clone;
          ASSERT(heap_->space()->Includes(instance->address()) ||
                 reinterpret_cast<TwoSpaceHeap*>(heap_)->old_space()->Includes(
                     instance->address()));
          // TODO(erikcorry): We should clone old-space objects into
          // old-space to avoid having up update the remembered set.
          clone = instance->CloneTransformed(heap_);
          instance->set_forwarding_address(clone);
          *p = clone;
          if (GCMetadata::GetPageType(clone->address()) == kNewSpacePage) {
            GCMetadata::InsertIntoRememberedSet(current_object_address_);
          }
        }
      } else if (heap_object->IsClass()) {
        Class* clazz = Class::cast(heap_object);
        if (clazz->IsTransformed()) {
          *p = clazz->TransformationTarget();
        }
      }
    }
  }

 private:
  Heap* const heap_;
  uword current_object_address_;
};

void Session::TransformInstances() {
  // Make sure we don't have any mappings that rely on the addresses
  // of objects *not* changing as we transform instances.
  for (int i = 0; i < maps_.length(); ++i) {
    ObjectMap* map = maps_[i];
    ASSERT(!map->HasTableByObject());
  }

  // Deal with program space before the process spaces. This allows
  // the traversal of the process heap to use the already installed
  // forwarding pointers in program space.

  SemiSpace* space = program()->heap()->space();
  NoAllocationFailureScope scope(space);
  TransformInstancesPointerVisitor program_visitor(program()->heap());
  program()->IterateRoots(&program_visitor);
  ASSERT(!space->is_empty());
  space->CompleteTransformations(&program_visitor);

  TwoSpaceHeap* process_heap = program()->process_heap();
  // When we are iterating over the heap we need to skip the areas of active
  // allocation, which are not traversable and do not contain untransformed
  // objects.
  process_heap->old_space()->StartTrackingAllocations();
  // When we rewrite objects we just expand the old space, so don't allow
  // allocations to fail there.
  NoAllocationFailureScope scope2(process_heap->old_space());
  TransformInstancesPointerVisitor process_heap_visitor(process_heap);

  for (auto process : *program()->process_list()) {
    process->IterateRoots(&process_heap_visitor);
  }

  process_heap->space()->CompleteTransformations(&process_heap_visitor);
  process_heap->old_space()->CompleteTransformations(&process_heap_visitor);

  process_heap->space()->RebuildAfterTransformations();
  process_heap->old_space()->RebuildAfterTransformations();

  // Make the newly allocated (newly transformed) objects traversable again.
  process_heap->old_space()->EndTrackingAllocations();
  process_heap->old_space()->UnlinkPromotedTrack();
}

void Session::PushFrameOnSessionStack(const Frame* frame) {
  Function* function = frame->FunctionFromByteCodePointer();
  uint8* start_bcp = function->bytecode_address_for(0);

  uint8* bcp = frame->ByteCodePointer();
  int bytecode_offset = bcp - start_bcp;
  // The byte-code offset is not a return address but the offset for
  // the invoke bytecode. Make it look like a return address by adding
  // the current bytecode size to the byte-code offset.
  Opcode current = static_cast<Opcode>(*bcp);
  bytecode_offset += Bytecode::Size(current);

  PushNewInteger(bytecode_offset);
  PushFunction(function);
}

int Session::PushStackFrames(Stack* stack) {
  ASSERT(!state_->IsScheduled() || state_->IsPaused());
  int frames = 0;
  Frame frame(stack);
  while (frame.MovePrevious()) {
    if (frame.ByteCodePointer() == NULL) continue;
    PushFrameOnSessionStack(&frame);
    ++frames;
  }
  return frames;
}

void Session::PushTopStackFrame(Stack* stack) {
  Frame frame(stack);
  bool has_top_frame = frame.MovePrevious();
  ASSERT(has_top_frame);
  PushFrameOnSessionStack(&frame);
}

void PausedState::RestartFrame(int frame_index) {
  Stack* stack = process()->stack();

  // Move down to the frame we want to reset to.
  Frame frame(stack);
  for (int i = 0; i <= frame_index; i++) frame.MovePrevious();

  // Reset the return address to the entry function.
  frame.SetReturnAddress(reinterpret_cast<void*>(InterpreterEntry));

  // Finally resize the stack to the next frame pointer.
  stack->SetTopFromPointer(frame.FramePointer());
}

int64 Session::FunctionMessage(Function *function) {
  if (program()->was_loaded_from_snapshot()) {
    return program()->OffsetOf(HeapObject::cast(function));
  } else {
    return MapLookupByObject(method_map_id_, function);
  }
}

int64 Session::ClassMessage(Class *klass) {
  if (program()->was_loaded_from_snapshot()) {
    return program()->OffsetOf(HeapObject::cast(klass));
  } else {
    return MapLookupByObject(class_map_id_, klass);
  }
}

}  // namespace dartino

#endif  // DARTINO_ENABLE_DEBUGGING
