# Robot construction demonstration
This is a demo of how to build a robot operating system using StageZero, Capcom
FlightDirector.  It is obviously not a full functional robot, but contains some
of the componentst that a robot operating system typically needs.

The robot comprises:

1. Two cameras, sending images at 10Hz.
2. A stereo module that takes the camera images and calculates disparity
3. A map server that uses RPC semantics to distribute map tiles
4. A GPS receiver that sends GPS coordinates at 2Hz
5. A localizer that receives GPS and stereo messages, loads map tiles and sends localizer messages
6. A logger that records all IPC messages to log files on disk

The "nodes" (ROS term) are built using the "module" facility which are
spawned from a zygote.  IPC is done using Subspace.  All modules use
protobuf for their serialization.  The logger uses zero-copy subscribers
to record the messages.

