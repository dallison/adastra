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

The user can specify whether the OutputEvent and LogMessage events will be sent through the client or not when the client is initialized.

`StartEvent` and `StopEvent` events inform the client's user of the startup or shutdown of a process.  A successful process start will result in a StartEvent.  A StopEvent is sent when a process terminates, for whatever reason (exit or signal), whether intention or not.

Output events are redirected output streams from a process.

LogMessage events contain formatted log message emitted from StageZero or processes and are described below.

### Log Messages
A log message is a formatted user-readable message.  It contains the following attributes:

1. Timestamp (nanosecond walltime from CLOCK_REALTIME)
2. Source: what the log message refers to (process name, etc.)
3. Log level (verbose, debug, info, warning, error)
4. Text: text the log message

They are sent through the API (if masked in).

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

When the client is initialized, the user can pass an `event mask` specifying what type of events they want to see. 

Subsystem status events are sent when the state of a subsystem changes.  They contain information about the subsystem and its processes.

Alarms are descibed below.

Output events are redirected output streams from one of the subsystem's processes.

LogMessage events contain formatted log message emitted from StageZero, processes or Capcom and are described above.

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



