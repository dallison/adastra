#include "stagezero/client_handler.h"
#include "absl/strings/str_format.h"
#include "stagezero/stagezero.h"
#include "toolbelt/hexdump.h"

#include <iostream>

namespace stagezero {

void EventSenderCoroutine(std::shared_ptr<ClientHandler> client,
                          co::Coroutine *c);

ClientHandler::~ClientHandler() { KillAllProcesses(); }

SymbolTable *ClientHandler::GetGlobalSymbols() const {
  return &stagezero_.global_symbols_;
}

toolbelt::Logger &ClientHandler::GetLogger() const {
  return stagezero_.logger_;
}

co::CoroutineScheduler &ClientHandler::GetScheduler() const {
  return stagezero_.co_scheduler_;
}

std::shared_ptr<Zygote>
ClientHandler::FindZygote(const std::string &name) const {
  return stagezero_.FindZygote(name);
}

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  stagezero_.AddCoroutine(std::move(c));
}

void ClientHandler::Run(co::Coroutine *c) {
  // The data is placed 4 bytes into the buffer.  The first 4
  // bytes of the buffer are used by SendMessage and ReceiveMessage
  // for the length of the data.
  char *sendbuf = command_buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(command_buffer_) - sizeof(int32_t);
  for (;;) {
    absl::StatusOr<ssize_t> n = command_socket_.ReceiveMessage(
        command_buffer_, sizeof(command_buffer_), c);
    if (!n.ok()) {
      printf("ReceiveMessage error %s\n", n.status().ToString().c_str());
      return;
    }
    std::cout << getpid() << " " << this << " server received\n";
    toolbelt::Hexdump(command_buffer_, *n);

    stagezero::control::Request request;
    if (request.ParseFromArray(command_buffer_, *n)) {
      stagezero::control::Response response;
      if (absl::Status s = HandleMessage(request, response, c); !s.ok()) {
        stagezero_.logger_.Log(toolbelt::LogLevel::kError, "%s\n",
                               s.ToString().c_str());
        return;
      }

      std::cout << getpid() << " server sending " << response.DebugString() << std::endl;
      if (!response.SerializeToArray(sendbuf, kSendBufLen)) {
        stagezero_.logger_.Log(toolbelt::LogLevel::kError,
                               "Failed to serialize response\n");
        return;
      }
      size_t msglen = response.ByteSizeLong();
      std::cout << getpid() << " SERVER SEND\n";
      toolbelt::Hexdump(sendbuf, msglen);
      absl::StatusOr<ssize_t> n =
          command_socket_.SendMessage(sendbuf, msglen, c);
      if (!n.ok()) {
        return;
      }
    } else {
      stagezero_.logger_.Log(toolbelt::LogLevel::kError,
                             "Failed to parse message\n");
      return;
    }
  }
}

absl::Status
ClientHandler::HandleMessage(const stagezero::control::Request &req,
                             stagezero::control::Response &resp,
                             co::Coroutine *c) {
  switch (req.request_case()) {
  case stagezero::control::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
    break;

  case stagezero::control::Request::kLaunchStaticProcess:
    HandleLaunchStaticProcess(std::move(req.launch_static_process()),
                              resp.mutable_launch(), c);
    break;

  case stagezero::control::Request::kLaunchZygote:
    HandleLaunchZygote(std::move(req.launch_zygote()), resp.mutable_launch(),
                       c);
    break;

  case stagezero::control::Request::kLaunchVirtualProcess:
    HandleLaunchVirtualProcess(std::move(req.launch_virtual_process()),
                               resp.mutable_launch(), c);
    break;

  case stagezero::control::Request::kStop:
    HandleStopProcess(req.stop(), resp.mutable_stop(), c);
    break;

  case stagezero::control::Request::kInputData:
    HandleInputData(req.input_data(), resp.mutable_input_data(), c);
    break;

  case stagezero::control::Request::kCloseProcessFileDescriptor:
    HandleCloseProcessFileDescriptor(
        req.close_process_file_descriptor(),
        resp.mutable_close_process_file_descriptor(), c);
    break;

  case stagezero::control::Request::kConnectSocket:
    break;
  case stagezero::control::Request::kOpenPipe:
    break;
  case stagezero::control::Request::kCloseFileDescriptor:
    break;
  case stagezero::control::Request::kSetGlobalVariable:
    HandleSetGlobalVariable(req.set_global_variable(),
                            resp.mutable_set_global_variable(), c);
    break;
  case stagezero::control::Request::kGetGlobalVariable:
    HandleGetGlobalVariable(req.get_global_variable(),
                            resp.mutable_get_global_variable(), c);
    break;

  case stagezero::control::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const stagezero::control::InitRequest &req,
                               stagezero::control::InitResponse *response,
                               co::Coroutine *c) {
  client_name_ = req.client_name();

  // Event channel is an ephemeral port.
  toolbelt::InetAddress event_channel_addr = command_socket_.BoundAddress();
  event_channel_addr.SetPort(0);

  std::cout << "binding event channel to " << event_channel_addr.ToString()
            << std::endl;
  // Open listen socket.
  toolbelt::TCPSocket listen_socket;
  if (absl::Status status = listen_socket.SetCloseOnExec(); !status.ok()) {
    response->set_error(status.ToString());
    return;
  }

  if (absl::Status status = listen_socket.Bind(event_channel_addr, true);
      !status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  int event_port = listen_socket.BoundAddress().Port();

  response->set_event_port(event_port);

  if (absl::Status status = event_trigger_.Open(); !status.ok()) {
    response->set_error(status.ToString());
    return;
  }

  // Spawn a coroutine to accept the event channel connection.
  event_channel_acceptor_ =
      std::make_unique<co::Coroutine>(stagezero_.co_scheduler_, [
        client = shared_from_this(), listen_socket = std::move(listen_socket)
      ](co::Coroutine * c2) mutable {
        std::cout << "accepting event channel " << std::endl;
        absl::StatusOr socket = listen_socket.Accept(c2);
        if (!socket.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError,
                                  "Failed to open event channel: %s",
                                  socket.status().ToString().c_str());
          return;
        }

        if (absl::Status status = socket->SetCloseOnExec(); !status.ok()) {
          client->GetLogger().Log(
              toolbelt::LogLevel::kError,
              "Failed to set close-on-exec on event channel: %s",
              socket.status().ToString().c_str());
          return;
        }
        client->SetEventSocket(std::move(*socket));
        client->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                "Event channel open");

        // Start the event sender coroutine.
        client->AddCoroutine(std::make_unique<co::Coroutine>(
            client->GetScheduler(),
            [client](co::Coroutine *c2) { EventSenderCoroutine(client, c2); }));
      });
}

void ClientHandler::HandleLaunchStaticProcess(
    const stagezero::control::LaunchStaticProcessRequest &&req,
    stagezero::control::LaunchResponse *response, co::Coroutine *c) {

  auto proc = std::make_shared<StaticProcess>(
      GetScheduler(), shared_from_this(), std::move(req));
  absl::Status status = proc->Start(c);
  if (!status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  std::string process_id = proc->GetId();
  response->set_process_id(process_id);
  response->set_pid(proc->GetPid());

  if (!stagezero_.AddProcess(process_id, proc)) {
    response->set_error(
        absl::StrFormat("Unable to add process %s", process_id));
    return;
  }
  AddProcess(process_id, proc);
}

void ClientHandler::HandleLaunchZygote(
    const stagezero::control::LaunchStaticProcessRequest &&req,
    stagezero::control::LaunchResponse *response, co::Coroutine *c) {

  auto zygote = std::make_shared<Zygote>(GetScheduler(), shared_from_this(),
                                         std::move(req));
  absl::Status status = zygote->Start(c);
  if (!status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  std::string process_id = zygote->GetId();
  response->set_process_id(process_id);
  response->set_pid(zygote->GetPid());
  if (!stagezero_.AddZygote(zygote->Name(), process_id, zygote)) {
    response->set_error(absl::StrFormat("Unable to add zygote %s(%s)", zygote->Name(), process_id));
    return;
  }
  AddProcess(process_id, zygote);
}

void ClientHandler::HandleLaunchVirtualProcess(
    const stagezero::control::LaunchVirtualProcessRequest &&req,
    stagezero::control::LaunchResponse *response, co::Coroutine *c) {
  auto proc = std::make_shared<VirtualProcess>(
      GetScheduler(), shared_from_this(), std::move(req));
  absl::Status status = proc->Start(c);
  if (!status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  std::string process_id = proc->GetId();
  response->set_process_id(process_id);
  response->set_pid(proc->GetPid());
  if (!stagezero_.AddProcess(process_id, proc)) {
    response->set_error(
        absl::StrFormat("Unable to add virtual process %s", process_id));
    return;
  }
  AddProcess(process_id, proc);
}

void ClientHandler::HandleStopProcess(
    const stagezero::control::StopProcessRequest &req,
    stagezero::control::StopProcessResponse *response, co::Coroutine *c) {
  std::shared_ptr<Process> proc = stagezero_.FindProcess(req.process_id());
  if (proc == nullptr) {
    response->set_error(
        absl::StrFormat("No such process %s", req.process_id()));
    return;
  }
  absl::Status status = proc->Stop(c);
  if (!status.ok()) {
    response->set_error(absl::StrFormat("Failed to stop process %s: %s",
                                        req.process_id(), status.ToString()));
  }
}

void ClientHandler::HandleInputData(
    const stagezero::control::InputDataRequest &req,
    stagezero::control::InputDataResponse *response, co::Coroutine *c) {
  std::shared_ptr<Process> proc = stagezero_.FindProcess(req.process_id());
  if (proc == nullptr) {
    response->set_error(
        absl::StrFormat("No such process %s", req.process_id()));
    return;
  }
  if (absl::Status status = proc->SendInput(req.fd(), req.data(), c);
      !status.ok()) {
    response->set_error(
        absl::StrFormat("Unable to send input data to process %s: %s",
                        req.process_id(), status.ToString()));
  }
}

void ClientHandler::HandleCloseProcessFileDescriptor(
    const stagezero::control::CloseProcessFileDescriptorRequest &req,
    stagezero::control::CloseProcessFileDescriptorResponse *response,
    co::Coroutine *c) {
  std::shared_ptr<Process> proc = stagezero_.FindProcess(req.process_id());
  if (proc == nullptr) {
    response->set_error(
        absl::StrFormat("No such process %s", req.process_id()));
    return;
  }
  if (absl::Status status = proc->CloseFileDescriptor(req.fd()); !status.ok()) {
    response->set_error(status.ToString());
  }
}

absl::Status
ClientHandler::SendProcessStartEvent(const std::string &process_id) {
  auto event = std::make_unique<stagezero::control::Event>();
  auto start = event->mutable_start();
  start->set_process_id(process_id);
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendProcessStopEvent(const std::string &process_id,
                                                 bool exited, int exit_status,
                                                 int term_signal) {
  auto event = std::make_unique<stagezero::control::Event>();
  auto stop = event->mutable_stop();
  stop->set_process_id(process_id);
  stop->set_exited(exited);
  stop->set_exit_status(exit_status);
  stop->set_signal(term_signal);
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendOutputEvent(const std::string &process_id,
                                            int fd, const char *data,
                                            size_t len) {
  auto event = std::make_unique<stagezero::control::Event>();
  auto output = event->mutable_output();
  output->set_process_id(process_id);
  output->set_data(data, len);
  output->set_fd(fd);
  return QueueEvent(std::move(event));
}

absl::Status
ClientHandler::QueueEvent(std::unique_ptr<stagezero::control::Event> event) {
  if (!event_socket_.Connected()) {
    return absl::InternalError(
        "Unable to send event: event socket is not connected");
  }
  std::cerr << "queueing event" << std::endl;
  events_.push_back(std::move(event));
  event_trigger_.Trigger();
  return absl::OkStatus();
}

void EventSenderCoroutine(std::shared_ptr<ClientHandler> client,
                          co::Coroutine *c) {
  while (client->event_socket_.Connected()) {
    std::cerr << "Waiting for event to be queued" << std::endl;
    // Wait for an event to be queued.
    c->Wait(client->event_trigger_.GetPollFd().Fd(), POLLIN);
    client->event_trigger_.Clear();

    // Empty event queue by sending all events before going back
    // to waiting for the trigger.  This ensures that we never have
    // something in the event queue after the trigger has been
    // cleared.
    while (!client->events_.empty()) {
      std::cerr << "Got queued event" << std::endl;

      // Remove event from the queue and send it.
      std::unique_ptr<control::Event> event =
          std::move(client->events_.front());
      client->events_.pop_front();
      std::cerr << event->DebugString() << std::endl;

      char *sendbuf = client->event_buffer_ + sizeof(int32_t);
      constexpr size_t kSendBufLen =
          sizeof(client->event_buffer_) - sizeof(int32_t);
      if (!event->SerializeToArray(sendbuf, kSendBufLen)) {
        client->GetLogger().Log(toolbelt::LogLevel::kError,
                                "Failed to serialize event");
      } else {
        size_t msglen = event->ByteSizeLong();
        absl::StatusOr<ssize_t> n =
            client->event_socket_.SendMessage(sendbuf, msglen, c);
        if (!n.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError,
                                  "Failed to serialize event",
                                  n.status().ToString().c_str());
        }
      }
    }
  }
}

void ClientHandler::KillAllProcesses() {
  // Copy all processes out of the processes_ map as we will
  // be removing them as they are killed.
  std::vector<std::shared_ptr<Process>> procs;

  for (auto & [ id, proc ] : processes_) {
    procs.push_back(proc);
  }
  for (auto &proc : procs) {
    proc->KillNow();
  }
}

absl::Status ClientHandler::RemoveProcess(Process *proc) {
  std::string id = proc->GetId();
  std::cerr << "removing process " << id << std::endl;
  auto it = processes_.find(id);
  if (it == processes_.end()) {
    return absl::InternalError(absl::StrFormat("No such process %s", id));
  }
  processes_.erase(it);

  return stagezero_.RemoveProcess(proc);
}

void ClientHandler::TryRemoveProcess(std::shared_ptr<Process> proc) {
   std::string id = proc->GetId();
  std::cerr << "removing process " << id << std::endl;
  auto it = processes_.find(id);
  if (it != processes_.end()) {
    processes_.erase(it);
  }

  stagezero_.TryRemoveProcess(proc);
}

void ClientHandler::HandleSetGlobalVariable(
    const stagezero::control::SetGlobalVariableRequest &req,
    stagezero::control::SetGlobalVariableResponse *response, co::Coroutine *c) {
  stagezero_.global_symbols_.AddSymbol(req.name(), req.value(), req.exported());
}

void ClientHandler::HandleGetGlobalVariable(
    const stagezero::control::GetGlobalVariableRequest &req,
    stagezero::control::GetGlobalVariableResponse *response, co::Coroutine *c) {
  Symbol *sym = stagezero_.global_symbols_.FindSymbol(req.name());
  if (sym == nullptr) {
    response->set_error(absl::StrFormat("No such variable %s", req.name()));
    return;
  }
  response->set_name(sym->Name());
  response->set_value(sym->Value());
  response->set_exported(sym->Exported());
  std::cout << response->DebugString();
}
} // namespace stagezero
