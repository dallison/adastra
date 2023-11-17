#include "module/module.h"
#include <cerrno>

namespace stagezero::module {

Module::Module(const std::string &name, const std::string &subspace_socket)
    : name_(name), subspace_socket_(subspace_socket) {}

absl::Status Module::ModuleInit() {
  if (absl::Status status = subspace_client_.Init(subspace_socket_);
      !status.ok()) {
    return status;
  }

  return absl::OkStatus();
}

absl::Status Module::NotifyStartup() {
  // Notify stagezero of startup.
  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    int e = write(notify_fd, &val, 8);
    if (e <= 0) {
      return absl::InternalError(absl::StrFormat(
          "Failed to notify StageZero of startup for module %s: %s", name_,
          strerror(errno)));
    }
  }
  return absl::OkStatus();
}

void Module::RunForever(double frequency,
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
                        std::function<void(int, co::Coroutine *)> callback) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [ fd, callback = std::move(callback) ](co::Coroutine * c) {
        for (;;) {
          c->Wait(fd);
          callback(fd, c);
        }
      },
      "ticker"));
}

void Module::RunOnEventWithTimeout(
    int fd, std::chrono::nanoseconds timeout,
    std::function<void(int, co::Coroutine *)> callback) {
  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [ fd, timeout, callback = std::move(callback) ](co::Coroutine * c) {
        for (;;) {
          int result_fd = c->Wait(fd, POLLIN, timeout.count());
          callback(result_fd, c);
        }
      },
      "event"));
}

void Module::Run() {
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
  if (absl::Status status = trigger_.Open(); !status.ok()) {
    // TODO log.
    std::cerr << "Failed to open trigger: " << status.ToString() << std::endl;
  }
  coroutine_name_ = absl::StrFormat("sub/%s/%s", module_.name_, sub_.Name());
}

absl::StatusOr<void *>
PublisherBase::GetMessageBuffer(size_t size, co::Coroutine *c) {
  auto pub = this->shared_from_this();
  // If we are a reliable publisher we need to keep trying to get a buffer.
  // We will wait for the reliable publisher's trigger to be triggered if
  // we fail to get a buffer.
  for (;;) {
    bool backpressure_applied = false;
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
    coroutine_name_ = absl::StrFormat("pub/%s/%s", module_.name_, pub_.Name());
  }

void PublisherBase::Stop() {
  running_ = false;
  pending_count_ = 0;
  trigger_.Trigger();
}


void PublisherBase::BackpressureSubscribers() {
  for (auto sub : options_.backpressured_subscribers) {
    sub->Stop();
  }
}

void PublisherBase::ReleaseSubscribers() {
  for (auto sub : options_.backpressured_subscribers) {
    sub->Run();
  }
}

} // namespace stagezero::module
