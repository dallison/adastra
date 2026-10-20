#include "common/parameters.h"

namespace adastra::parameters {

std::vector<std::string> ParameterServer::ParseParameterName(std::string name) {
  std::vector<std::string> parts;
  std::string part;
  while (!name.empty() && name[0] == '/') {
    name = name.substr(1);
  }
  for (size_t i = 0; i < name.size(); i++) {
    if (name[i] == '/') {
      i++;
      while (!name.empty() && name[i] == '/') {
        i++;
      }
      parts.push_back(part);
      part.clear();
      i--;
    } else {
      part += name[i];
    }
  }

  parts.push_back(part);
  return parts;
}

std::shared_ptr<ParameterNode>
ParameterServer::FindParameter(const std::string &name) {
  std::vector<std::string> parts = ParseParameterName(name);
  std::shared_ptr<ParameterNode> node = nodes_[parts[0]];
  if (node == nullptr) {
    return nullptr;
  }
  for (size_t i = 1; i < parts.size(); i++) {
    node = node->children_[parts[i]];
    if (node == nullptr) {
      return nullptr;
    }
  }
  return node;
}

absl::StatusOr<std::vector<std::string>> ParameterServer::ListParameters() {
  std::vector<std::string> names;
  for (auto & [ name, node ] : nodes_) {
    if (node == nullptr) {
      continue;
    }
    if (is_local_) {
      node->FlattenNames(name, names);
    } else {
      node->FlattenNames("/" + name, names);
    }
  }
  return names;
}

std::vector<Parameter> ParameterServer::GetAllParameters() {
  std::vector<Parameter> params;
  for (auto & [ name, node ] : nodes_) {
    if (node == nullptr) {
      continue;
    }
    if (is_local_) {
      node->Flatten(name, params);
    } else {
      node->Flatten("/" + name, params);
    }
  }
  return params;
}

absl::Status ParameterServer::SetParameter(const std::string &name,
                                           Value value) {
  std::vector<std::string> parts = ParseParameterName(name);
  if (parts.empty()) {
    return absl::InvalidArgumentError("Invalid parameter name");
  }
  std::shared_ptr<ParameterNode> &node = nodes_[parts[0]];
  if (node == nullptr) {
    auto child = std::make_shared<ParameterNode>(parts[0]);
    child->parent_ = node.get();
    node = child;
  }
  // Only a single part: "/foo".  Just set the value.
  if (parts.size() == 1) {
    if (value.GetType() == Type::kMap) {
      ExpandMap(value.GetMap(), node);
    } else {
      node->children_.clear();
      node->SetValue(value);
    }
    return absl::OkStatus();
  }

  // Add all intermediate children nodes.  Leave the last one off.
  std::shared_ptr<ParameterNode> parent = node;
  for (size_t i = 1; i < parts.size() - 1; i++) {
    std::shared_ptr<ParameterNode> &child = parent->children_[parts[i]];
    if (child == nullptr) {
      child = std::make_shared<ParameterNode>(parts[i]);
      child->parent_ = parent.get();
    }
    parent = child;
  }
  // Add the leaf node and set its value.
  std::shared_ptr<ParameterNode> &last_node = parent->children_[parts.back()];
  if (last_node == nullptr) {
    last_node = std::make_shared<ParameterNode>(parts.back());
    last_node->parent_ = parent.get();
  }
  if (value.GetType() == Type::kMap) {
    ExpandMap(value.GetMap(), last_node);
  } else {
    last_node->children_.clear();
    last_node->SetValue(value);
  }
  return absl::OkStatus();
}

absl::StatusOr<Value> ParameterServer::GetParameter(const std::string &name) {
  std::vector<std::string> parts = ParseParameterName(name);
  if (parts.empty()) {
    return absl::InvalidArgumentError("Invalid parameter name");
  }
  std::shared_ptr<ParameterNode> node = nodes_[parts[0]];
  if (node == nullptr) {
    return absl::NotFoundError(absl::StrFormat("Parameter %s not found", name));
  }
  for (size_t i = 1; i < parts.size(); i++) {
    node = node->children_[parts[i]];
    if (node == nullptr) {
      return absl::NotFoundError(
          absl::StrFormat("Parameter %s not found", name));
    }
  }
  if (node->children_.empty()) {
    return node->GetValue();
  }
  // Parameter has children.  Return this as a map.
  return MakeMap(node);
}

bool ParameterServer::HasParameter(const std::string &name) {
  std::vector<std::string> parts = ParseParameterName(name);
  if (parts.empty()) {
    return false;
  }
  std::shared_ptr<ParameterNode> node = nodes_[parts[0]];
  if (node == nullptr) {
    return false;
  }
  for (size_t i = 1; i < parts.size(); i++) {
    node = node->children_[parts[i]];
    if (node == nullptr) {
      return false;
    }
  }
  return true;
}

absl::Status ParameterServer::DeleteParameter(const std::string &name) {
  std::vector<std::string> parts = ParseParameterName(name);
  if (parts.empty()) {
    return absl::InvalidArgumentError("Invalid parameter name");
  }
  auto it = nodes_.find(parts[0]);
  if (it == nodes_.end()) {
    return absl::NotFoundError(
        absl::StrFormat("Parameter %s not found for deletion", name));
  }
  std::shared_ptr<ParameterNode> node = it->second;
  if (parts.size() == 1) {
    auto it = nodes_.find(parts[0]);
    if (it != nodes_.end()) {
      nodes_.erase(it);
    }
    return absl::OkStatus();
  }
  for (size_t i = 1; i < parts.size() - 1; i++) {
    auto it = node->children_.find(parts[i]);
    if (it == node->children_.end()) {
      return absl::NotFoundError(
          absl::StrFormat("Parameter %s not found for deletion", name));
    }
    node = it->second;
  }
  it = node->children_.find(parts.back());
  if (it == node->children_.end()) {
    return absl::NotFoundError(
        absl::StrFormat("Parameter %s not found for deletion", name));
  }
  node->children_.erase(it);
  return absl::OkStatus();
}

std::map<std::string, Value>
ParameterServer::MakeMap(const std::shared_ptr<ParameterNode> &node) {
  std::map<std::string, Value> map;
  for (auto & [ name, child ] : node->children_) {
    if (child->children_.empty()) {
      map[name] = child->GetValue();
    } else {
      map[name] = MakeMap(child);
    }
  }
  return map;
}

void ParameterServer::ExpandMap(const std::map<std::string, Value> &map,
                                std::shared_ptr<ParameterNode> parent) {
  for (auto & [ name, value ] : map) {
    std::shared_ptr<ParameterNode> &node = parent->children_[name];
    if (node == nullptr) {
      node = std::make_shared<ParameterNode>(name);
      node->parent_ = parent.get();
    }
    if (value.GetType() == Type::kMap) {
      ExpandMap(value.GetMap(), node);
    } else {
      node->SetValue(value);
    }
  }
}

void ParameterServer::Dump(std::ostream &os) {
  for (auto & [ name, node ] : nodes_) {
    if (node == nullptr) {
      continue;
    }
    node->Dump(os);
  }
}

void ParameterNode::Dump(std::ostream &os) {
  os << name_ << ": " << value_ << std::endl;
  for (auto & [ name, child ] : children_) {
    child->Dump(os);
  }
}

void ParameterNode::FlattenNames(std::string prefix,
                                 std::vector<std::string> &names) {
  if (children_.empty()) {
    names.push_back(prefix);
    return;
  }
  for (auto & [ name, child ] : children_) {
    if (child == nullptr) {
      continue;
    }
    child->FlattenNames(prefix + "/" + name, names);
  }
}

void ParameterNode::Flatten(std::string prefix,
                            std::vector<Parameter> &params) {
  if (children_.empty()) {
    params.push_back({.name = GetFullName(), .value = GetValue()});
    return;
  }
  for (auto & [ name, child ] : children_) {
    if (child == nullptr) {
      continue;
    }
    child->Flatten(prefix + "/" + name, params);
  }
}

// To and from proto.
void Value::ToProto(adastra::proto::parameters::Value *dest) const {
  switch (GetType()) {
  case Type::kInt32:
    dest->set_int32_value(GetInt32());
    break;
  case Type::kInt64:
    dest->set_int64_value(GetInt64());
    break;
  case Type::kDouble:
    dest->set_double_value(GetDouble());
    break;
  case Type::kBool:
    dest->set_bool_value(GetBool());
    break;
  case Type::kString:
    dest->set_string_value(GetString());
    break;
  case Type::kBytes:
    dest->set_bytes_value(GetBytes().data(), GetBytes().size());
    break;
  case Type::kTime:
    dest->mutable_time_value()->set_seconds(GetTime().tv_sec);
    dest->mutable_time_value()->set_nanos(GetTime().tv_nsec);
    break;
  case Type::kList: {
    auto list = dest->mutable_list_value();
    for (const Value &v : GetList()) {
      v.ToProto(list->add_values());
    }
    break;
  }
  case Type::kMap: {
    auto map = dest->mutable_map_value();
    for (const auto & [ key, v ] : GetMap()) {
      auto *entry = map->mutable_values();
      adastra::proto::parameters::Value value;
      v.ToProto(&value);
      (*entry)[key] = value;
    }
    break;
  }
  }
}

void Value::FromProto(const adastra::proto::parameters::Value &proto) {
  switch (proto.value_case()) {
  case adastra::proto::parameters::Value::kInt32Value:
    *this = proto.int32_value();
    break;
  case adastra::proto::parameters::Value::kInt64Value:
    *this = proto.int64_value();
    break;
  case adastra::proto::parameters::Value::kDoubleValue:
    *this = proto.double_value();
    break;
  case adastra::proto::parameters::Value::kBoolValue:
    *this = proto.bool_value();
    break;
  case adastra::proto::parameters::Value::kStringValue:
    *this = proto.string_value();
    break;
  case adastra::proto::parameters::Value::kBytesValue:
    *this = proto.bytes_value();
    break;
  case adastra::proto::parameters::Value::kTimeValue: {
    struct timespec ts;
    ts.tv_sec = proto.time_value().seconds();
    ts.tv_nsec = proto.time_value().nanos();
    *this = ts;
    break;
  }
  case adastra::proto::parameters::Value::kListValue: {
    std::vector<Value> list;
    for (const auto &v : proto.list_value().values()) {
      Value value;
      value.FromProto(v);
      list.push_back(value);
    }
    *this = list;
    break;
  }
  case adastra::proto::parameters::Value::kMapValue: {
    std::map<std::string, Value> map;
    for (const auto &entry : proto.map_value().values()) {
      Value value;
      value.FromProto(entry.second);
      map[entry.first] = value;
    }
    *this = map;
    break;
  }
  default:
    break;
  }
}

void ParameterNode::ToProto(adastra::proto::parameters::Parameter *dest) const {
  dest->set_name(name_);
  auto value = dest->mutable_value();
  value_.ToProto(value);
}

void ParameterNode::FromProto(
    const adastra::proto::parameters::Parameter &proto) {
  name_ = proto.name();
  value_.FromProto(proto.value());
}

void Parameter::ToProto(adastra::proto::parameters::Parameter *dest) const {
  dest->set_name(name);
  auto v = dest->mutable_value();
  value.ToProto(v);
}

void Parameter::FromProto(const adastra::proto::parameters::Parameter &proto) {
  name = proto.name();
  value.FromProto(proto.value());
}

} // namespace adastra::parameters
