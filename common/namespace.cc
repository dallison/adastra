// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "common/namespace.h"

#if defined(__linux__)
#include <sched.h>
#endif

namespace adastra {

void Namespace::ToProto(stagezero::config::Namespace *dest) const {
  dest->set_type(type);
}

void Namespace::FromProto(const stagezero::config::Namespace &src) {
  type = src.type();
}

int Namespace::CloneType() const {
#if !defined(__linux__)
  return 0;
#else
  int r = 0;
  if ((type & adastra::stagezero::config::Namespace::NS_NEWCGROUP) != 0) {
    r |= CLONE_NEWCGROUP;
  }
  if ((type & adastra::stagezero::config::Namespace::NS_NEWIPC) != 0) {
    r |= CLONE_NEWIPC;
  }
  if ((type & adastra::stagezero::config::Namespace::NS_NEWNET) != 0) {
    r |= CLONE_NEWNET;
  }
  if ((type & adastra::stagezero::config::Namespace::NS_NEWPID) != 0) {
    r |= CLONE_NEWPID;
  }
  if ((type & adastra::stagezero::config::Namespace::NS_NEWUSER) != 0) {
    r |= CLONE_NEWUSER;
  }
  if ((type & adastra::stagezero::config::Namespace::NS_NEWUTS) != 0) {
    r |= CLONE_NEWUTS;
  }
  return r;
#endif
}
} // namespace adastra