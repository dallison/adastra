#pragma once

#include "proto/config.pb.h"

namespace adastra {

// Scheduler type for sched_setscheduler.
// See sched_setscheduler(2) for more information.
enum class KernelSchedulerPolicyType {
  kDefault,
  kBatch,
  kIdle,
  kFifo,
  kRoundRobin,
};

struct KernelSchedulerPolicy {
  KernelSchedulerPolicyType policy = KernelSchedulerPolicyType::kDefault;
  int priority = 0;
  bool reset_on_fork = false;

  void ToProto(stagezero::config::KernelSchedulerPolicy *dest) const {
    switch (policy) {
    case KernelSchedulerPolicyType::kDefault:
    default:
      dest->set_policy(
          stagezero::config::KernelSchedulerPolicy::KERN_SCHED_DEFAULT);
      break;
    case KernelSchedulerPolicyType::kBatch:
      dest->set_policy(
          stagezero::config::KernelSchedulerPolicy::KERN_SCHED_BATCH);
      break;
    case KernelSchedulerPolicyType::kIdle:
      dest->set_policy(
          stagezero::config::KernelSchedulerPolicy::KERN_SCHED_IDLE);
      break;
    case KernelSchedulerPolicyType::kFifo:
      dest->set_policy(
          stagezero::config::KernelSchedulerPolicy::KERN_SCHED_FIFO);
      break;
    case KernelSchedulerPolicyType::kRoundRobin:
      dest->set_policy(stagezero::config::KernelSchedulerPolicy::KERN_SCHED_RR);
      break;
    }
    dest->set_priority(priority);
    dest->set_reset_on_fork(reset_on_fork);
  }

  void FromProto(const stagezero::config::KernelSchedulerPolicy &src) {
    switch (src.policy()) {
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_DEFAULT:
    default:
      policy = KernelSchedulerPolicyType::kDefault;
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_BATCH:
      policy = KernelSchedulerPolicyType::kBatch;
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_IDLE:
      policy = KernelSchedulerPolicyType::kIdle;
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_FIFO:
      policy = KernelSchedulerPolicyType::kFifo;
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_RR:
      policy = KernelSchedulerPolicyType::kRoundRobin;
      break;
    }
    priority = src.priority();
    reset_on_fork = src.reset_on_fork();
  }
};

} // namespace adastra
