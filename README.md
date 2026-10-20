# AdAstra Subsystem Launcher
This software provides a means to coordinate the launch of subsystems within a system of many computers.  **Ad Adstra** is a latin phrase meaning `to the stars` and introduces the general theme of `space travel` for this software.  Components of the software are:

1. **StageZero**: the process laucher.  This is at term used in the space industry for the launch complex needed to launch a rocket (which has stages 1, 2, 3, etc.).  StageZero is responsible for launching the processes and monitoring them, reporting events to `capcom`.
2. **Modules**: the individual components of the system being run.  Analagous to the components of a spacecraft, modules make up the system being run on the robot.  They communicate using `subspace` IPC channels and are loaded as `virtual` processes.
3. **Capcom**: the software for controlling everything launched from StageZero.  In the space industry, Capcom is the `capsule communicator`, the only person in mission control allowed to talk with the astronauts.  All commands are relayed through Capcom.  In this software, Capcom is responsible for taking `subsystems` and sending commands to StageZero to launch the processes on the appropriate computers.  It also monitors the subsytems using the events from StageZero and deals with subsystem startup, shutdown and restart.  Capcom also handles log messages from the processes.
4. **FlightDirector**: responsible for defining the subsystems and telling `capcom` what to do.  In the mission control scenario, the flight director is in charge of the mission, giving directions to Capcom to relay to the spacecraft.  In this system, the flight director is takes the definition of a set of subsytems comprising the system and sends commands to Capcom to load the subsystems, bring them online and take them offline as needed.
5. **FDO (Fido)**: monitor and present a live user interface for all the subsystems, processes, IPC channels, etc.  In mission control, the Flight Dynamics Officer (pronounced fido) is reponsible for monitoring all aspects of the mission and relaying the information to the flight director.  In this software, it a text-based interface that displays live information to the screen.
6. **flight**: a command line tool you can use to control the system

The modules of the system use the [Subspace](https://github.com/dallison/subspace) IPC system for communication.  This is an ultra-high-performance shared memory based pub/sub system available as open source.

# What is a subsystem?
A subsystem is a set of `processes` that run on any of the computers in the system.  Subsystems can have dependencies on other subsystems and the full set of subsystems make up a subsystem directed acyclic graph.

For example, a robot may have a `camera` subsystem which has processes to read images from cameras.  Another subsystem, maybe the `stereo` subsystem takes in the camera images and calculates disparity or other algorithmic analysis.  Before the `stereo` subsystem can run, it must run the `camera` subsystem in order to receive the image it needs.  In this case, `camera` is a dependency of `stereo` and must be running before `stereo` runs.  

# Processes
A process is something that runs as a separate `process` in the operating system.  There are three types of processes available:

1. **Static processes**: those run by called `execve` on an executable file
2. **Zygotes**: processes whose sole job is to spawn `virtual` processes
3. **Virtual**: processes spawned from `zygotes`

A `static` process is pretty self-explanatory and should be familiar to anyone.  If we can run static processes from files, why do we need the concept of a zygote?

## Zygotes
A zygote (a term from biology) is a process that can be transformed into another process.  This means it is a process that forks but does not exec another process.  In a modern dynamically linked operating system, processes will load dynamic shared objects (sometimes called dynamically linked libraries), when they run.  These objects contain shared code and serve to reduce the amount of memory used by the process, since a lot of the common code can be shared among a lot of processes.  As a system grows, the number of dynamic shared libraries can increase and soon the time taken to load them gets large and the amount of unshareable data grows along with the number of libraries.  Although the code in the library can be shared, the data cannot and there are technical details about the library that make sharing of some memory not feasible (Global Offset Tables on Linux and MacOS, for example).

They are used by the Chrome browser when you open a new tab and by the Android OS for running apps. 

Robots tend to use small, embedded, computers that are fairly short on memory.  If every process that the robot runs is statically linked, there is a copy of all the shared code in every process, which takes up physical memory.  If the processes are dynamically linked, the loading of the shared object files takes memory as relocations need to be done for every file (GOT relocations to be precise).  This all uses memory and is it likely that you will run out eventually.

By spawing processes from zygotes (so-called `virtual` processes in this system), all the code and data in the zygote's process can be shared with all the processes spawned from that zygote.  This can save a huge amount of memory and, as a side effect, the startup time for the processes is greatly reduced.  A win overall.

# AdAstra components

## StageZero
StageZero runs on every computer, most likely started at boot time.  It comprises a server process and a client library.  The server opens a TCP/IP port on the computer (by default 6522) and listens for connections from clients.  There is no configuration for the StageZero server and it does nothing unless told to do so by a client.

Clients communicate with StageZero through a client library.  This is a C++ library that provides the following functionality:

1. LaunchStaticProcess
2. LaunchZygote
3. LaunchVirtualProcess
4. StopProcess
5. ReadEvent
6. SendInput
7. CloseProcessFileDescriptor
8. SetGlobalVariable
9. GetGlobalVariable
10. Abort

The first 4 control the launch and shutdown of processes.  StageZero takes care of getting the processes running and monitoring them, sending events when changes are detected.  Process may be interactive, with their input coming from the client (the SendInput function) and their output being delivered as events.  In addition to the special case of closing a process file descriptor, we also support a set of variables that are available to the processes.  Each process can have a set of local variable, but StageZero also holds some global variables which can be set and read via the API.  Finally, the Abort function allows the system to be aborted, with all processes being killed.

### Variables
Variables are string-valued named symbols that can be seen by the processes and used in the process configuration.  Each process can have some local variables only seen by that process, and StageZero has a set of global variables seen by all processes.  Variables can be used in many contexts in the process configuration, for example, in command line arguments, or in the executable file name.  Virtual processes are also passed a symbol table containing all the variables.  Variables can be exported to the process environment so that they can be seen by the processes directly.

Variables are generally useful for configuring how the process will run.  For example, a command line argument may reference a variable, some like `--port=${this_port}`.  Here, the variable `this_port` will be expanded before being passed to the process as an argument.

Variables are analagous to shell variables in common UNIX shells.

### Stream redirection
Any file descriptor seen by a process may be redirected when it is launched.  The most common file descriptors to redirect are 0, 1 and 2, representing stdin, stdout and stderr, but others may also be redirected as necessary.  File descriptors may be redirected to the following locations:

1. **StageZero**: use the same location as the same file descriptor in StageZero.
2. **Client**: redirect the file descriptor to/from the client.  Output file descriptors are carried as events,and input comes via the SendInput API function.
3. **Filename**: redirect to/from a named file.
4. **File descriptor**: redirect to/from a given file descriptor
5. **Close**: close the file descriptor
6. **Logger**: only for output streams, redirect the file descriptor as log messages to be sent as events

Redirecting stdin, stdout and stderr are obvious uses for stream redireciton, but you can also do things like redirect stream 4 to a given filename.  This will make file descriptor 4 in the process point to a file.  You can also use named pipes to create a pipeline of processes if you want, although I don't see why you would want to.

### Events
A feature of the StageZero client is the ability to receive `events`.  These are objects sent to the client by StageZero containing information that the user of the client might want to see.  The types of events are:

1. StartEvent
2. StopEvent
3. OutputEvent
4. LogMessage
5. Parameter change or deletion
6. Connection successful
7. Connection disconnected
8. Telemetry status

The user can specify whether the OutputEvent, LogMessage, Parameter, and Telemetry events will be sent through the client or not when the client is initialized.

`StartEvent` and `StopEvent` events inform the client's user of the startup or shutdown of a process.  A successful process start will result in a StartEvent.  A StopEvent is sent when a process terminates, for whatever reason (exit or signal), whether intention or not.

Output events are redirected output streams from a process.

LogMessage events contain formatted log message emitted from StageZero or processes and are described below.

The `Connect` and `disconnect` events are used to inform the client that the connection to StageZero is up or down.

The `Telemetry` events are discussed later in the Telemetry section.

### Log Messages
A log message is a formatted user-readable message.  It contains the following attributes:

1. Timestamp (nanosecond walltime from CLOCK_REALTIME)
2. Source: what the log message refers to (process name, etc.)
3. Log level (verbose, debug, info, warning, error)
4. Text: text the log message

They are sent through the API (if masked in).

### Telemetry
The telemetry facility is a way for a StageZero client to send commands and receive status messages from running
processes.  It works by creating two pipes to the process (if it's enabled).  The `command pipe` is written to by StageZero and read by the process, and the 'status pipe` is read by StageZero and written by the process.

A simple example of a system-provided telemetry command is the `ShutdownCommand` which can be sent by StageZero
to stop a process running by causing it to exit with a given code.  If the process is not able to accept
this telemetry command, StageZero will use signals to tell the process to exit.  The telemetry command is a
better way to ask a process to terminate.

A process must explicitly enable the telemetry facility in order for StageZero to set it up for that process.

#### Telemetry protobuf messages
The telemetry system uses protobuf messages as its transport.  In particular, it uses the `google.proto.Any` special
message to transport arbitrary command and status messages.

The basic command and status messages are defined in `proto/telemetry.proto` as follows:

```
package adastra.proto.telemetry;

message Command { google.protobuf.Any command = 1; }

message Status { google.protobuf.Any status = 1; }
```

That is, the contents of a `Command` and `Status` message is any other message.  The `google.protobuf.Any` type
is a special type that contains two fields:

```
string type_url
bytes value
```
The `type_url` is a string that contains (in URL form for some reason) the fully qualified protobuf type
name of the message contained in the `value` field.

The protobuf compiler (and my Phaser protobuf backend) adds special code to handle the `google.protobuf.Any`
type.  Take a look at Google's documentation for how to use it.

The telemetry facility provides two C++ base classes to wrap the protobuf `Command` and `Status` messages.  They
provide a way for a process to convert to and from the protobuf classes.  They are:

```c++
// Base class for telemetry commands.  These are sent from the StageZero client
// to a running process.
struct TelemetryCommand {
  virtual ~TelemetryCommand() = default;

  // The code can be used to distinguish between different commands.  There is
  // no global code namspace, so each module should define its own codes if it
  // needs to.
  virtual int Code() const { return 0; }

  // To and from proto methods are used to serialize and deserialize the
  // command.
  virtual void ToProto(adastra::proto::telemetry::Command &proto) const {};
  virtual bool FromProto(const adastra::proto::telemetry::Command &proto) {
    return false;
  }

  // Convenience method to convert to a proto and return it without needing a
  // temporary object.
  adastra::proto::telemetry::Command AsProto() const {
    adastra::proto::telemetry::Command proto;
    ToProto(proto);
    return proto;
  }
};

// Base class for telemetry status.  These are sent from a process and will be
// delivered as events from StageZero.
struct TelemetryStatus {
  virtual ~TelemetryStatus() = default;
  virtual void ToProto(adastra::proto::telemetry::Status &proto) const {};
  virtual bool FromProto(const adastra::proto::telemetry::Status &proto) {
    return false;
  }

  // Convenience method to convert to a proto and return it without needing a
  // temporary object.
  adastra::proto::telemetry::Status AsProto() const {
    adastra::proto::telemetry::Status proto;
    ToProto(proto);
    return proto;
  }
};
```

As an example, here is how the `ShutdownCommand` is implemented:
```c++
// System telemetry command for process shutdown.
struct ShutdownCommand : public TelemetryCommand {
  ShutdownCommand(int exit_code, int timeout_secs)
      : exit_code(exit_code), timeout_secs(timeout_secs) {}
  int Code() const override { return SystemTelemetry::kShutdownCommand; }

  void ToProto(adastra::proto::telemetry::Command &proto) const override {
    adastra::proto::telemetry::ShutdownCommand shutdown;
    shutdown.set_exit_code(exit_code);
    shutdown.set_timeout_seconds(timeout_secs);
    proto.mutable_command()->PackFrom(shutdown);
  }

  bool FromProto(const adastra::proto::telemetry::Command &proto) override {
    if (!proto.command().Is<adastra::proto::telemetry::ShutdownCommand>()) {
      return false;
    }
    adastra::proto::telemetry::ShutdownCommand shutdown;
    if (!proto.command().UnpackTo(&shutdown)) {
      return false;
    }
    exit_code = shutdown.exit_code();
    timeout_secs = shutdown.timeout_seconds();
    return true;
  }

  int exit_code;    // Exit with this status.
  int timeout_secs; // You have this long to exit before you get a signal.
};
```
The `Code` function returns an integer that is meaningful to the telemetry module, `SystemTelemetry` in 
this case.  It is a convenient way to distinguish between various commands in the module.



#### Telemetry Modules
The library `//stagezero/telemetry` provides a process-side telemetry module facility.  This comprises of a `Telemetry`
class to which a process can add its own `TelemetryModule` instances.

For most use-cases, the process would create an instance of a `stagezero::Telemetry` class with no arguments,
add its telemetry modules and then call the `Run()` function, probably in a thread.  The `Run` function will not
return until the telemetry system is shut down.  The telemetry system will poll the input pipe for commands and allows
the process to send status events back to StageZero.  If the process is coroutine aware and has its own coroutine
scheduler, it can tell the telemetry system to use that instead, in which case the `Run` function will not
block and instead will add a coroutine to the given scheduler.  This is pretty specialized use though.

A telemetry module is a class derived from `::stagezero::TelemetryModule`:

```c++
// This is the base for a telemetry module.  Users should subclass this to
// provide application specific telemetry handling.
class TelemetryModule {
public:
  TelemetryModule(Telemetry &telemetry, std::string name)
      : telemetry_(telemetry), name_(std::move(name)) {}
  virtual ~TelemetryModule() = default;

  const std::string Name() const { return name_; }

  // Parse the protobuf command message and return a command object.  If the
  // type is not recognized return nullptr.
  // To check if the command is of a specific type, use the Is<> method as
  // follows:
  //   if (command.command().Is<PROTOBUF-TYPE>()) {
  //
  // For example:
  // absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  // SystemTelemetry::ParseCommand(
  //  const adastra::proto::telemetry::Command &command) {
  //  if (command.command().Is<adastra::proto::telemetry::ShutdownCommand>()) {
  //    adastra::proto::telemetry::ShutdownCommand shutdown;
  //    if (!command.command().UnpackTo(&shutdown)) {
  //      return absl::InternalError("Failed to unpack shutdown command");
  //    }
  //    return std::make_unique<ShutdownCommand>(shutdown.exit_code(),
  //    shutdown.timeout_seconds());
  //  }

  virtual absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  ParseCommand(const adastra::proto::telemetry::Command &command) = 0;

  // This is called when a command is received.  Subclasses should override this
  // to provide implementation for the command.  For an example of how to
  // use this, see the SystemTelemetry implementation in telemetry.cc.
  virtual absl::Status HandleCommand(std::unique_ptr<TelemetryCommand> command,
                                     co::Coroutine *c) {
    return absl::OkStatus();
  }

protected:
  Telemetry &telemetry_;
  std::string name_;
};
```
The idea is that when an incoming command is received, it is passed around all the registered
telemetry modules in the process asking for one that knows about it.  This is done by calling
the `Parse` command virtual function.  If the module knows about the command type it should
parse the contents of the `google.protobuf.Any` field in the command and return it as a
`std::unique_ptr`.  If it doesn't know about the command, it should return `nullptr`.

If the command is known, the telemetry system will then call `HandleCommand` in the module
that knows about it.  This is where the actual command handling is done.

For example, to take our `ShutdownCommamnd` a stage further, here is how the `SystemTelemetry`
module handles it:

```c++
absl::Status
SystemTelemetry::HandleCommand(std::unique_ptr<TelemetryCommand> command,
                               co::Coroutine *c) {
  switch (command->Code()) {
  case SystemTelemetry::kShutdownCommand: {
    auto shutdown = dynamic_cast<ShutdownCommand *>(command.get());
    exit(shutdown->exit_code);
    break;
  }

  default:
    return absl::InternalError(absl::StrFormat(
        "Unknown system telemetry command code: %d", command->Code()));
  }
  return absl::OkStatus();
}

```
If a process wants to do things other than just call `exit` on a `ShutdownCommand`, it can derive
a new class from `SystemTelemetry` and provide its own version of `HandleCommand`.

For completeness, here is the class declaration for `SystemTelemetry`:
```c++
// This is the system telemetry module.  It provides a command to shutdown a
// process without needing to send a signal.
class SystemTelemetry : public TelemetryModule {
public:
  static constexpr int kShutdownCommand = 1;

  SystemTelemetry(Telemetry &telemetry,
                  const std::string &name = "adastra::system")
      : TelemetryModule(telemetry, name) {}

  absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  ParseCommand(const adastra::proto::telemetry::Command &command) override;

  absl::Status HandleCommand(std::unique_ptr<TelemetryCommand> command,
                             co::Coroutine *c) override;
};

```
The `ParseCommand` is defined as:
```c++
// System telemetry module.
absl::StatusOr<std::unique_ptr<TelemetryCommand>> SystemTelemetry::ParseCommand(
    const adastra::proto::telemetry::Command &command) {
  if (command.command().Is<adastra::proto::telemetry::ShutdownCommand>()) {
    adastra::proto::telemetry::ShutdownCommand shutdown;
    if (!command.command().UnpackTo(&shutdown)) {
      return absl::InternalError("Failed to unpack shutdown command");
    }
    return std::make_unique<ShutdownCommand>(
        ShutdownCommand(shutdown.exit_code(), shutdown.timeout_seconds()));
  }
  return nullptr;
}
```
Pretty simple.

#### Sending telemetry status messages
The `SendStatus` function in the `Telemetry` class allows a process to send its status messages as events to
StageZero client.

It can do this in reponse to an incoming `Command` or it can use one of the three `Call...` functions:
```c++
 // Call the callback every 'period' nanoseconds.
  void CallEvery(std::chrono::nanoseconds period,
                 std::function<void(co::Coroutine *)> callback);

  // Call the callback once after 'timeout' nanoseconds.
  void CallAfter(std::chrono::nanoseconds timeout,
                 std::function<void(co::Coroutine *)> callback);

  // Call the callback now (for some definiton of "now").
  void CallNow(std::function<void(co::Coroutine *)> callback);
```
These functions will call the `callback` function (typically a lambda) under various conditions which
should be self evident.  The callbacks are called from within a coroutine context and are passed a
pointer to the coroutine in which they are running.  You can most likely ignore that if you are
not coroutine based, but there are certain things you might need to know about:

1. You can't block the process inside these functions (don't call a blocking `read` for example)
2. You only have a 64K stack to play with.

As an example of how to use these, take a gander at the file `testdata/telemetry.cc`.

The status messages you send from processes appear as events at the StageZero client as the following
protobuf message:
```
message TelemetryEvent {
  string process_id = 1; // Source process ID
  uint64 timestamp = 2;
  string compute = 3; // Compute instance
  adastra.proto.telemetry.Status telemetry = 4;
}
```
The `process_id` is the ID of the process that sent the event.  The actual telemetry status is in the
`telemetry` field.
You can use the regular `google.protobuf.Any` access methods to get the contents of the individual telemetry status
messages.

For example, if you have received an event from the client (in `ts`), you can decode it like this:
```c++
   if (ts->telemetry().status().Is<::testdata::telemetry::TimeTelemetryStatus>()) {
      ::testdata::telemetry::TimeTelemetryStatus status;
      auto ok = ts->telemetry().status().UnpackTo(&status);
      std::cout << "Time: " << status.time() << std::endl;
```
In this case the telemetry status is a message of the form:

```
package testdata.telemetry;

message TimeTelemetryStatus { uint64 time = 1; }
```


## Capcom
Capcom talks to StageZero processes via their client API.  There is only one Capcom process that talks to all StageZero processes.  It does not launch the StageZero servers on the computers.  Capcom deals with `subsystems` comprising of a set of processes.  Subsystems can have dependencies on other subsystems, building up a DAG.  Capcom is a server process (launched on one computer via systemd or something), and a client library that is used to communicate with it.  The server process opens a TCP/IP port

The main task for Capcom is to allow a subsystem to be defined and loaded via its API.  Once loaded, a subsystem may be started or stopped.  Starting a subsystem also starts all dependencies, unless they are already started.  Stopping a subsystem will also being down its dependencies, unless they are needed by another running subsystem.

If a process crashes, Capcom will stop all the other processes in its subsystem, but not dependent subsystems.  It will also propagate the stop upwards to parent subsystems, before attempting to restart the subsystems, subject to restart limits.

Capcom reports problems (like process crashes) using `alarms` which are sent as events through the client API.  Alarms are problem reports that are raised when the problem occurs and cleared when the issue that causes the problem goes away.  For example, if a process crashed, an alarm will be raised and if the process restarts successfully, the alarm will be cleared.

### Client
The Capcom client provides the following API functions:

1. AddSubsystem
2. RemoveSubsystem
3. AddCompute
4. RemoveCompute
5. StartSubsystem
6. StopSubsystem
7. ReadEvent
8. SendInput
9. SetGlobalVariable
10. GetGlobalVariable
11. Abort
12. CloseFd
13. GetAlarms
14. SetParameter
15. DeleteParameter
16. UploadParameters
17. SendTelemetryCommandToProcess
18. SendTelemetryCommandToSubsystem

Capcom needs to be told what `computes` are available.  A compute is another name for a computer that is in a robot.  Subsystems contain processes that are assigned to a compute.  Each compute must be running a StageZero server process.

Subsystems consist of a set of processes (static, zygote or virtual) and a set of children, which are subsystems upon which a dependency is created.  The process definition is held inside the subsystem being added, but all child subsystems must be already present.  This implies that subsystems must be loaded into Capcom on a bottom up order with all children being loaded before their parents.

Subsystems have two states that control them:

1. Administrative state: the state that the subsystem should be in (`offline` or `online`)
2. Operational state: the actual state of the subsytstem (`offline`, `online`, `starting-processes`, `restarting`, etc.)

The administrative state is controlled by the user connected to the Capcom client using the `StartSubsystem` and `StopSubsystem` functions.  The operational state is controlled by Capcom, in conjunction with StageZero which runs the processes.

A subsystem may have multiple parents (subsystems depending on it).  A subsystem will be operationally `online` once all its children are operationally online and all its processes are running.  A subsystem will be operationally `offline` once all its processes are stopped and any children that are not needed by other subsystems are `offline`.  This means that a subsystem with multiple parents will only be operationally `offline` when it is not needed by any of its parents.

Any part of the subsystem graph may be controlled by the client.  For example, you can bring a camera subsystem online without bringing its parents online.

### Events
A feature of the Capcom client is the ability to receive `events`.  These are objects sent to the client by Capcom containing information that the user of the client might want to see.  The types of events are:

1. SubsystemStatus
2. Alarm
3. Output
4. LogMessage
5. ParameterUpdate
6. ParameterDelete
7. Telemetry

When the client is initialized, the user can pass an `event mask` specifying what type of events they want to see. 

Subsystem status events are sent when the state of a subsystem changes.  They contain information about the subsystem and its processes.

Alarms are descibed below.

Output events are redirected output streams from one of the subsystem's processes.

LogMessage events contain formatted log message emitted from StageZero, processes or Capcom and are described above.

Any parameter updates or deletions (from a process) are sent as `ParameterUpdate` or `ParameterDelete` events.

`Telemetry` events are `::adastra::proto::TelemetryEvent` messages that come from processes that have enabled telemetry.  See the [Telemetry](#telemetry) section for details.  The message is defined as:

```
message TelemetryEvent {
  string subsystem = 1;
  string process_id = 2;
  telemetry.Status telemetry = 3;
  uint64 timestamp = 4;
}

```
This is basically the same event that was sent from StageZero for telemetry status, but also includes the `subsystem` that
the process is part of.


### Alarms
Alarms are reports of problems, like a process crashing.  An alarm is an object with the following attributes:

1. Type (process, subsystem or system)
2. Severity (warning, error or critical)
3. Reason (crashed, broken, emergency abort)
4. Status (raised or cleared)
5. Name (the name of the subsystem or process affected)
6. Details (why the alarm is raised)

They are sent through client's TCP/IP connection as events (if masked in).  Each alarm also has a unique ID that can be used to correlate the alarm by the user.

### Log files
Capcom writes all log messages to a file on disk.  The file is in `/tmp` by default but can be changed with a command line argument to the `capcom` program.  The file is a simple length encoded binary file where each log entry is a 64-bit little endian length followed by the protobuf encoded contents of the log entry.

You can switch this off if you don't want the log messages saved to disk by passing the command line flag `--log_file=/dev/null`.

### Umbilicals
Although stretching the analogy somewhat, Capcom connects to StageZero through `umbilicals`, which are reference counted
TCP connections from Capcom to the StageZero listening ports on the `compute` instances.

There is one main `umbilical` for Capcom itself which is used to convey global variables, parameters and cgroups.  There is also one umbilical per subsystem that is connected to the StageZero instances that the subsystem needs to run its processes.  Generally, the subsystem umbilicals will be connected on demand when it needs to start a process.  When all 
the processes have been stopped for that subsystem on a compute, the umbilical will be disconnected.

The main Capcom umbilical can be either connected dynamically when the first subsystem umbilical is connected or
it can be connected statically when the `Compute` is added.  This is specified when the `Compute` is added to 
Capcom as a parameter with the following type:

```c++
enum class ComputeConnectionPolicy {
  kDynamic,
  kStatic,
};

```
If the value is `kDynamic`, the Capcom will only try to connect to the compute when it needs to.  This allows the
computers in your robot to start up at different times.  If `kStatic` is specified, Capcom will try to connect
to the StageZero on the compute when you add it, which means it must already be running.

The default (and expected to be the most common) is dynamic connections where Capcom will connect and disconnect
to StageZero when it needs to and the computer can come and go.

### Parameters
Parameters are a way to pass runtime information to processes run by `AdAstra`.  ROS has a global parameter server inside its `roscore` process that is used by most ROS programs to pass runtime parameters to nodes.  AdAstra provides a similar facility in the form of a global parameter server running in `Capcom` which is distrubuted to all `StageZero` instances.  The processes can access parameters via a pipe to StageZero.  Alongside global parameters which are seen by all processes, a process can have a set of local parameters that are known only to that process.  ROS has a notional concept of local parameters, but they are just global parameters that are prefixed by a node's name.

Parameters look like the names of files in a filesystem.  Global parameters are `rooted` with a slash and contain a set of names separated by `/` (just like a file).  Each parameter has a value which is one of:

1. Signed 32-bit integer
2. Signed 64-bit integer
3. String
4. Double precision IEEE 754 number
5. Boolean
6. Array of arbitrary bytes
7. A `struct timespec` specifying a nanosecond timestamp
8. A vector of other values (need not all be the same type)
9. A map of name vs value

A process can perform the following operations on parameters, all controlled by the `//stagezero/parameters` library:

* Obtain a list of all parameters without their values
* Obtain a list of all parameters with their values
* Get the value of a specific parameter
* Get the value of a set of parameters, parented by a named parameter (as a map)
* Set the value of a specific parameter or tree of parameters
* Delete a parameter or tree of parameters
* Listen of parameter value updates or deletions

The API for the parameter library from a process is:

```c++
namespace stagezero {
class Parameters {
public:
  Parameters(bool events = false, co::Coroutine *c = nullptr);

  // Set the given parameter to the value.  If it doesn't exist, it will be
  // created.
  absl::Status SetParameter(const std::string &name,
                            adastra::parameters::Value value,
                            co::Coroutine *c = nullptr);

  // Delete the parameter with the given name.  Error if it doesn't exist.
  absl::Status DeleteParameter(const std::string &name,
                               co::Coroutine *c = nullptr);

  // Get a list of all parameters, not in any particular order.
  absl::StatusOr<std::vector<std::string>>
  ListParameters(co::Coroutine *c = nullptr);

  // Get the names and values of all parameters, not in any particular order.
  absl::StatusOr<
      std::vector<std::shared_ptr<adastra::parameters::ParameterNode>>>
  GetAllParameters(co::Coroutine *c = nullptr);

  // Get the value of a parameter.  Error if it doesn't exist
  absl::StatusOr<adastra::parameters::Value>
  GetParameter(const std::string &name, co::Coroutine *c = nullptr);

  // Does the parameter exist?
  absl::StatusOr<bool> HasParameter(const std::string &name,
                                    co::Coroutine *c = nullptr);

  // Use this to poll for events.
  const toolbelt::FileDescriptor &GetEventFD() const { return event_fd_; }

  // Read an event.  This will block until an event is available.
  std::unique_ptr<adastra::parameters::ParameterEvent>
  GetEvent(co::Coroutine *c = nullptr) const;
};
}
```

The value of a parameter is held in the struct `adastra::parameters::Value` which holds a `std::variant` of all the different parameter types.  Take a look at the file `common/parameters.h` for the struct definition.  It is straightforward.  The `Value` struct contains non-explicit constructors for each of the parameter types for convenience (yes, this generally violates company style-guides for explicit constructors but it's worth it).

#### How this works
`StageZero` is the process's parent and holds a cached copy of all the global parameters and all process local parameters.  When a process is spawned, it creates three pipes and passes one end of each of them to the process.  The pipes are:

1. Write pipe: processes write parameter commands to the write end
2. Read pipe: processes read command responses from the read end
3. Event pipe: processes read events from parameter updates and deletes

Say a process wants to read a parameter's value.  It will send a command to StageZero through the write pipe and wait for a result to come back through the read pipe.

If it wants to receive events, it tells StageZero to send events (via an initialization command sent through the write pipe), and then reads the events from the event pipe.

The performance is very good, empirically measured at about 30 microseconds per command/response on a modern computer.

#### Creating a Parameters object
Before a process can access its parameters, it must create a `stagezero::Parameters` instance and use it to talk to StageZero.  The `Parameters` constructor takes a couple of arguments with default values:

1. `events`: a boolean specifying whether you want to use the event pipe to receive events
2. `c`: a coroutine if you are in a coroutine environment.

The `events` argument must be set to `true` if the process wants to see events.  If a process sets it to true and then doesn't actually read any events, the pipe might fill up with data, consuming memory in the kernel.  It won't actually block StageZero but you should avoid it anyway since it uses memory for no reason.

#### Listening for parameter changes
A process can listen for changes to its local and all global parameters.  The API provides a way to get a file descriptor for the  `event pipe` that is connected to StageZero.  This file descriptor can be used in a call to  `poll` or `epoll` (or `select` too I guess) to wait for data to come in.  This can be done in a separate thread or coroutine in the process, or could be in the process's main loop depending on its design.

When you detect a parameter change, you can call `GetEvent` to get the details of what happened.  The `ParameterEvent` is defined as:

```c++
struct ParameterEvent {
  enum class Type {
    kUpdate,
    kDelete,
  };
  ParameterEvent(Type type) : type(type) {}
  Type type;
};

struct ParameterUpdateEvent : public ParameterEvent {
  ParameterUpdateEvent() : ParameterEvent{Type::kUpdate} {}

  std::string name;
  Value value;
};

struct ParameterDeleteEvent : public ParameterEvent {
  ParameterDeleteEvent() : ParameterEvent{Type::kDelete} {}

  std::string name;
};

```
There are two derived classes, one for an update to a parameter and one for the deletion.  The `type` tells you which one is appropriate for the event.

As an example, here's code to print the events seen by a process:

```c++
      stagezero::Parameters params(true);

      const toolbelt::FileDescriptor &fd = params.GetEventFD();
      struct pollfd pfd = {
          .fd = fd.Fd(), .events = POLLIN,
      };
      for (;;) {
        int ret = poll(&pfd, 1, 0);
        if (ret < 0) {
          perror("poll");
          break;
        }
        if (pfd.revents & POLLIN) {
          std::unique_ptr<adastra::parameters::ParameterEvent> event =
              params.GetEvent();
          std::cerr << "Event: " << std::endl;
          switch (event->type) {
          case adastra::parameters::ParameterEvent::Type::kUpdate: {
            adastra::parameters::ParameterUpdateEvent *update =
                static_cast<adastra::parameters::ParameterUpdateEvent *>(
                    event.get());

            std::cerr << "Update: " << update->name << " = " << update->value
                      << std::endl;
            break;
          }
          case adastra::parameters::ParameterEvent::Type::kDelete: {
            adastra::parameters::ParameterDeleteEvent *del =
                static_cast<adastra::parameters::ParameterDeleteEvent *>(
                    event.get());
            std::cerr << "Delete: " << del->name << std::endl;
            break;
          }
          }
          continue;
        }
        break;
      }
    }

```


#### Parameter layout
Parameters form a tree.  If they are global, the tree is rooted at `/`.  If they are local, there is no `/` and the root is the empty name.  Each parameter has a value and that value is either one of the primitive types or is a map of `name` vs `value` nodes.  Put another way, if you access a leaf parameter you will get is value as one of the primitive types, but if you access a non-leaf, you will get a map of all its children (recursively) with their values.


For example, say you have the following parameter tree:

```
/
  a
    b
      c (value 1)
    d
      e (value 2)
  f (value 3)
```
Non-leaf parameters do not have any value themselves. You can access the parameters leaf nodes:

```
/a/b/c = 1
/a/d/e = 2
/f = 3
```
Or, if you access a non-leaf node, you will see a map containing a recursive set of parameters values.  Say you access `/a/b`:

```
/a/b = {c: 1}
```

And if you access `/a`:

```
/a = {b: {c: 1}, {d: {e: 2}}}
```
That is, a map of maps.

When you read non-leaf parameters, you will get a map and when you write to them you pass them a map containing the names and values of the children.

#### Global parameters
Global parameters are held by `Capcom` and distributed to all `StageZero` instances.  Any changes made to global parameters are propagated to all `StageZero` instances immediately.  Processes who are interested in detecting changes to global parameters are can watch for events through the `//stagezero/parameters` library API.

Global parameters all start with a slash `/` and are added to `Capcom` using its regular API.  There are three
functions you can use to create or modify parameters in the Capcom client API:

```c++
  absl::Status SetParameter(const std::string &name, const parameters::Value &v,
                            co::Coroutine *c = nullptr);

  absl::Status DeleteParameters(const std::vector<std::string> &names = {},
                               co::Coroutine *c = nullptr);

  absl::StatusOr<std::vector<parameters::Parameter>>
  GetParameters(const std::vector<std::string> &names = {},
                co::Coroutine *c = nullptr);

  absl::StatusOr<parameters::Value> GetParameter(const std::string &name,
                                                 co::Coroutine *c = nullptr);

  absl::Status
  UploadParameters(const std::vector<parameters::Parameter> &params,
                   co::Coroutine *c = nullptr);
```
The `SetParameter` function will create the parameter if it doesn't exist.  If the value is a map, a tree will be created.  If it does exist, the parameter value will be set.

The `DeleteParameters` will delete a set of parameters or a tree of them.  If the `names` is empty all parameters will
be deleted.

The `GetParameters` and `GetParameter` functions allow the client to read the values of parameters;  If `names` is empty, it will read all the parameters.

The `UploadParameters` allows a whole set of parameters to be sent to Capcom at once.

When the system is running, if you use these functions to change the global parameters, the changes will be propagated to StageZeros and onward to any processes that care to listen for changes.

#### Local parameters
Local parameters are, by definition, local to a process and are specified in the process's configuration when it is added to Capcom.  Processes will see these parameters and can change them, but they are purely local to the process and no other process can access them.  This is unlike the `notionally local` ROS parameter system where a local parameter is just a global parameter with a node name prefix.

To add local parameters to a process, put them in the config.  For example, from one of the unit tests:

```c++
  absl::Status status = client.AddSubsystem(
      "param",
      {
          .static_processes = {{
              .name = "params",
              .executable = "${runfiles_dir}/_main/testdata/params",
              .notify = true,
              .parameters =
                  {
                      {"foo/bar", "local-value1"}, {"foo/baz", "local-value2"},
                  },
          }},
      });
```
This adds two local parameters with really impressive and imaginative names to the `params` process.

## FlightDirector
The FlightDirector is Capcom's manager (think Gene Kranz in Apollo 13).  He/she is in charge of the mission.  In this software, the FlightDirector (or just `flight`) talks to Capcom through a client connection and tells it to load, unload, start and stop subsystems.  It also can receive events from Capcom.

Like all the other AdAstra components, it consists of a server process and a client API.  The server runs on one computer only and opens a TCP/IP port (6524 by default).

Capcom is responsible for getting the subsystems loaded up.  It takes a subsystem configuration from files on disk and loads them into Capcom on startup.  Then, when commanded through its client API or configured to do so on startup, it asks Capcom to start or start subsystems.

One implementation of `flight` is provided in this software but it is by no means the only way to do it.  This implementation uses protobuf text format files to specify the subsystems and their dependencies.  Other configuration formats could easlily be used (YAML is popular for example).

This implementation takes a directory tree of .pb.txt files representing subsystems and loads them all up on startup.  It forms them into a graph, checks the graph and loads it into Capcom.

### Command line
A command line tool that talks to `flight` is provided.  This lets you send commands to flight to start or stop subsystems, show logs, run an interactive process, etc.

The program is simple and has the following commands:

```
  flight start <subsystem> - start a subsystem running
  flight run <subsystem> - run a subsystem interactively
  flight stop <subsystem> - stop a subsystem
  flight status - show status of all subsystems
  flight abort <subsystem> - abort all subsystems
  flight alarms - show all alarms
  flight log - show live text log
  flight log <filename>- show recorded text log from file
```

## Modules
A module is a component that runs as a virtual process and communicates with other modules using `Subspace` IPC.  Each module runs in a separate process spawned from a `zygote`.

Modules use a template libary that takes message types and registers publishers and subscribers for Subspace messages.  Modules can use `serializing` publishers/subscribers or `zero-copy` protocols that require no serialization.  A `protobuf` specialization for the general module template is provided.
This uses Google's Protocol Buffers as a serialization system.

Modules have the following facilities available:

1. RegisterSerializingSubscriber
2. RegisterSerializingPublisher
3. RegisterZeroCopySubscriber
4. RegisterZeroCopyPublisher
5. RemovePublisher
6. RemoveSubscriber
7. RunPeriodically
8. RunAfterDelay
9. RunNow
10. RunOnEvent
11. RunOnEventWithTimeout

The first 4 register publishers and subscribers.  Both reliable and unreliable forms of both are available.  The next 2 allow publishers and subscribers to be removed.

The `Run...` functions allow lambda to be executed under various conditions.  You can run the lambda at a given frequency; once only, immediately or after a delay; or on receipt of an `event` from a file descriptor (when a file descriptor is available for read).

This module system is intended for single-threaded operation, and makes use of `coroutines` to provide parallelization.  It is important that modules do not block the process for any significant length of time.  Coroutines use file descriptors and the poll(2) system call to allow cooperative
sharing of the CPU.  A `coroutine` pointer is available in all functions for use by that function if it needs to perform I/O.

To define a module, derive a class from `adastra::module::Module` and in its `Init` function register the publishers and subscribers you need, specifying their types.  Assuming you are fine with protobuf, you can derive your module's class from `adastra::module::ProtobufModule` and it will automatically use protobuf for serialiation.
 
It is important to register your module by calling `DEFINE_MODULE` in the module's code.  This will define a module that is loaded dynamically after the zygote forks.  You can also, if you want, link the module with a zygote and use `DEFINE_EMBEDDED_MODULE` to define it.

## FDO (fido)
The Flight Dynamics Operator (FDO, pronounced "fido") monitors the mission from a console in mission control.  In this software it is a program that displays a retro-style terminal console that shows a live display of:

1. Subsystem states
2. Running processes
3. Subspace IPC channels
4. Alarms
5. Log messages

Hit the '?' for help on the available keys you can press to control it.  You can add filters to the windows and control the log level for log messages.

## Software layout
The software is laid out as a set of directories containing the various components:

1. **common**: things common to all or most components
2. **proto**: protocol buffers files
3. **stagezero**: the StageZero process
4. **stagezero/client**: the StageZero client
5. **stagezero/zygotes**: zygote processes
6. **module**: the Module and ProtobufModule classes
7. **capcom**: Capcom server
8. **capcom/client**: Capcom client
9. **flight**: FlightDirector server
10. **flight/client**: flight client
11. **flight/command**: flight command line
12. **fido**: FDO user interface
13. **testdata**: some test data for unit tests
14. **robot**: example robot system (see below)

## Building
This uses `bazel`, Google's build system.  You might need to download it on your system if you don't already have it.  It's easy.

To build all of the software, change into the top level directory and:

```
$ bazel build ...
```

If you are on a Mac with Apple Silicon, the following command will avoid running things with Rosetta:

```
$ bazel build --config=apple_silicon ...
```

You can also change into a specific directory to build just that component if you want.

The software imports the following packages:

1. [Abseil](https://github.com/abseil/abseil-cpp)
2. [Protobuf](https://github.com/protocolbuffers/protobuf)
3. [Subspace](https://githug.com/dallison/subspace)
4. [Coroutines](ttps://githug.com/dallison/co)
5. [Toolbelt](ttps://githug.com/dallison/cpp_toolbelt)
6. [Retro](ttps://githug.com/dallison/retro)

This is all handled by Bazel so as long as you have access to the internet, it should all
just work.

Oh, you will need a recent version of `clang` (or maybe gcc).  This is C++17 software.  And, I haven't tried this on Windows so it might not work.

# Example 
In order to try to make things clearer, I have provided an example robot implementation that mimicks some of the modules you would need on a real robot.

It is in the directory `robot` at the top level and comprises of the following subsystems:

1. subspace (the Subspace server)
2. standard_zygote (a simple zygote)
3. camera (two cameras)
4. stereo (disparity algorithm on camera images)
5. gps (a fake gps receiver)
6. mapper (a fake map distributor using RPC semantics)
7. localizer (takes in stereo, mapper and gps)
8. logger (writes all channels to disk)

To run the example, use the shell script `run_flight.sh`, passing `--config_root_dir=robot/flight`.  This will run StageZero, Capcom and FlightDirector.  You can use the `flight` and `fido` commands to control and monitor the progress.

For example, use the commands:

```
$ ./run_flight.sh --config_root_dir=robot/flight
```

And in another window:
```
$ bazel-bin/flight/flight start localizer
```

To start the localizer and all its children.

To see what's going on, run this in a reasonably big terminal window (about 170x60):

```
$ bazel-bin/fido/fido
```

And marvel at the lightweight retro display :-)



