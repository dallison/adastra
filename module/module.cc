// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "module/module.h"
#include "absl/strings/numbers.h"
#include "toolbelt/hexdump.h"
#include <cerrno>
#include <signal.h>

namespace adastra::module {

Module::Module(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
    : symbols_(std::move(symbols)) {}

absl::Status Module::ModuleInit() {
  const std::string &subspace_socket = SubspaceSocket();
  if (subspace_socket.empty()) {
    return absl::InternalError("No subspace_socket specified");
  }
  if (absl::Status status = subspace_client_.Init(subspace_socket);
      !status.ok()) {
    return status;
  }

  return absl::OkStatus();
}

const std::string &Module::Name() const { return LookupSymbol("name"); }

const std::string &Module::SubspaceSocket() const {
  return LookupSymbol("subspace_socket");
}

const std::string &Module::LookupSymbol(const std::string &name) const {
  static std::string empty;
  stagezero::Symbol *sym = symbols_->FindSymbol(name);
  if (sym == nullptr) {
    return empty;
  }
  return sym->Value();
}

absl::Status Module::NotifyStartup() {
  // Notify adastra of startup.
  stagezero::Symbol *notify = symbols_->FindSymbol("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd;
    bool ok = absl::SimpleAtoi(notify->Value(), &notify_fd);
    if (ok) {
      int64_t val = 1;
      (void)write(notify_fd, &val, 8);
    }
  }
  return absl::OkStatus();
}

void Module::RunPeriodically(double frequency,
                             std::function<void(co::Coroutine *)> callback) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [ frequency, callback = std::move(callback) ](co::Coroutine * c) {
        uint64_t period_ns = 1000000000.0 / frequency;
        for (;;) {
          c->Nanosleep(period_ns);
          callback(c);
        }
      },
      "ticker"));
}

void Module::RunAfterDelay(std::chrono::nanoseconds delay,
                           std::function<void(co::Coroutine *)> callback) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [ delay, callback = std::move(callback) ](co::Coroutine * c) {
        c->Nanosleep(delay.count());
        callback(c);
      },
      "timer"));
}

void Module::RunNow(std::function<void(co::Coroutine *)> callback) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [callback = std::move(callback)](co::Coroutine * c) { callback(c); },
      "now"));
}

void Module::RunOnEvent(int fd,
                        std::function<void(int, co::Coroutine *)> callback,
                        short poll_events) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [ fd, poll_events, callback = std::move(callback) ](co::Coroutine * c) {
        for (;;) {
          c->Wait(fd, poll_events);
          callback(fd, c);
        }
      },
      "ticker"));
}

void Module::RunOnEventWithTimeout(
    int fd, std::chrono::nanoseconds timeout,
    std::function<void(int, co::Coroutine *)> callback, short poll_events) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [ fd, timeout, poll_events,
                    callback = std::move(callback) ](co::Coroutine * c) {
        for (;;) {
          int result_fd = c->Wait(fd, poll_events, timeout.count());
          callback(result_fd, c);
        }
      },
      "event"));
}

void Module::RunOnEvent(
    toolbelt::FileDescriptor fd,
    std::function<void(toolbelt::FileDescriptor, co::Coroutine *)> callback,
    short poll_events) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [ fd, poll_events, callback = std::move(callback) ](co::Coroutine * c) {
        for (;;) {
          c->Wait(fd.Fd(), poll_events);
          callback(fd, c);
        }
      },
      "ticker"));
}

void Module::RunOnEventWithTimeout(
    toolbelt::FileDescriptor fd, std::chrono::nanoseconds timeout,
    std::function<void(toolbelt::FileDescriptor, co::Coroutine *)> callback,
    short poll_events) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [ fd, timeout, poll_events,
                    callback = std::move(callback) ](co::Coroutine * c) {
        for (;;) {
          int result_fd = c->Wait(fd.Fd(), poll_events, timeout.count());
          if (result_fd == -1) {
            // Timeout;
            callback({}, c);
          } else {
            callback(fd, c);
          }
        }
      },
      "event"));
}

void Module::RemoveSubscriber(const std::shared_ptr<SubscriberBase> sub) {
  RemoveSubscriber(*sub);
}

void Module::RemovePublisher(const std::shared_ptr<PublisherBase> pub) {
  RemovePublisher(*pub);
}

void Module::RemoveSubscriber(SubscriberBase &sub) {
  for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
    if (it->get() == &sub) {
      sub.Stop();
      subscribers_.erase(it);
      return;
    }
  }
}

void Module::RemovePublisher(PublisherBase &pub) {
  for (auto it = publishers_.begin(); it != publishers_.end(); ++it) {
    if (it->get() == &pub) {
      pub.Stop();
      publishers_.erase(it);
      return;
    }
  }
}
static co::CoroutineScheduler *g_scheduler;

static void Signal(int sig) {
  if (sig == SIGQUIT && g_scheduler != nullptr) {
    g_scheduler->Show();
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

void Module::Run() {
  g_scheduler = &scheduler_;
  signal(SIGQUIT, Signal);
  signal(SIGPIPE, SIG_IGN);

  // Register a callback to be called when a coroutine completes.  The
  // server keeps track of all coroutines created.
  // This deletes them when they are done.
  scheduler_.SetCompletionCallback(
      [this](co::Coroutine *c) { coroutines_.erase(c); });

  // Run the coroutine main loop.
  scheduler_.Run();
}

void Module::Stop() { scheduler_.Stop(); }

SubscriberBase::SubscriberBase(Module &module, subspace::Subscriber sub,
                               SubscriberOptions options)
    : module_(module), sub_(std::move(sub)), options_(std::move(options)) {
  if (absl::Status status = stop_trigger_.Open(); !status.ok()) {
    // TODO log.
    std::cerr << "Failed to open trigger: " << status.ToString() << std::endl;
  }
  coroutine_name_ = absl::StrFormat("sub/%s/%s", module_.Name(), sub_.Name());
}

absl::StatusOr<void *> PublisherBase::GetMessageBuffer(size_t size,
                                                       co::Coroutine *c) {
  auto pub = this->shared_from_this();
  // If we are a reliable publisher we need to keep trying to get a buffer.
  // We will wait for the reliable publisher's trigger to be triggered if
  // we fail to get a buffer.
  bool backpressure_applied = false;

  for (;;) {
    absl::StatusOr<void *> buffer = pub->pub_.GetMessageBuffer(size);
    if (!buffer.ok()) {
      return buffer.status();
    }
    if (*buffer == nullptr) {
      // This should only happen for a reliable publisher.
      if (pub->options_.reliable) {
        // Reliable has been backpressured.  Need to wait until we
        // can try again.
        // Apply backpressure to the subscribers.
        if (!backpressure_applied) {
          pub->BackpressureSubscribers();
          backpressure_applied = true;
        }
        absl::Status wait_status = pub->pub_.Wait(c);
        if (!wait_status.ok()) {
          std::cerr << "Failed to wait for reliable publisher: "
                    << wait_status.ToString() << std::endl;
          abort();
        }
        continue; // Try again to get a buffer
      } else {
        return absl::InternalError(
            absl::StrFormat("Failed to get buffer for publisher to channel %s",
                            pub->pub_.Name()));
      }
    }
    // Release any backpressured subscribers now that we have a buffer.
    if (backpressure_applied) {
      pub->ReleaseSubscribers();
    }
    return *buffer;
  }
}

PublisherBase::PublisherBase(Module &module, subspace::Publisher pub,
                             PublisherOptions options)
    : module_(module), pub_(std::move(pub)), options_(std::move(options)) {
  if (absl::Status status = trigger_.Open(); !status.ok()) {
    // TODO log.
    std::cerr << "Failed to open trigger: " << status.ToString() << std::endl;
  }
  coroutine_name_ = absl::StrFormat("pub/%s/%s", module_.Name(), pub_.Name());
}

void PublisherBase::Stop() {
  running_ = false;
  pending_count_ = 0;
  trigger_.Trigger();
}

void PublisherBase::BackpressureSubscribers() {
  for (auto sub : options_.backpressured_subscribers) {
    sub->Backpressure();
  }
}

void PublisherBase::ReleaseSubscribers() {
  for (auto sub : options_.backpressured_subscribers) {
    sub->ReleaseBackpressure();
  }
}

} // namespace adastra::module
