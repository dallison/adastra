#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "proto/parameters.pb.h"
#include <functional>
#include <memory>
#include <string>
#include <time.h>
#include <variant>

namespace adastra::parameters {

enum class Type {
  kString,
  kInt32,
  kInt64,
  kDouble,
  kBool,
  kBytes,
  kTime,
  kList,
  kMap,
};

struct Value {
  Value() = default;
  Value(int32_t value) : type(Type::kInt32), value(value) {}
  Value(int64_t value) : type(Type::kInt64), value(value) {}
  Value(double value) : type(Type::kDouble), value(value) {}
  Value(bool value) : type(Type::kBool), value(value) {}
  Value(const char *value) : type(Type::kString), value(value) {}
  Value(std::string value) : type(Type::kString), value(std::move(value)) {}
  Value(absl::Span<char> value) : type(Type::kBytes), value(std::move(value)) {}
  Value(const std::vector<Value> &value) : type(Type::kList), value(value) {}
  Value(const std::map<std::string, Value> &value)
      : type(Type::kMap), value(value) {}
  Value(struct timespec value) : type(Type::kTime), value(value) {}
  Value &operator=(int32_t v) {
    type = Type::kInt32;
    value = v;
    return *this;
  }
  Value &operator=(int64_t v) {
    type = Type::kInt64;
    value = v;
    return *this;
  }
  Value &operator=(double v) {
    type = Type::kDouble;
    value = v;
    return *this;
  }
  Value &operator=(bool v) {
    type = Type::kBool;
    value = v;
    return *this;
  }
  Value &operator=(std::string v) {
    type = Type::kString;
    value = std::move(v);
    return *this;
  }
  Value &operator=(absl::Span<char> v) {
    type = Type::kBytes;
    value = std::move(v);
    return *this;
  }
  Value &operator=(struct timespec v) {
    type = Type::kTime;
    value = v;
    return *this;
  }
  Value &operator=(const std::vector<Value> &v) {
    type = Type::kList;
    value = v;
    return *this;
  }
  Value &operator=(const std::map<std::string, Value> &v) {
    type = Type::kMap;
    value = v;
    return *this;
  }

  bool operator==(const Value &other) const {
    if (type != other.type) {
      return false;
    }
    switch (type) {
    case Type::kString:
      return GetString() == other.GetString();
    case Type::kInt32:
      return GetInt32() == other.GetInt32();
    case Type::kInt64:
      return GetInt64() == other.GetInt64();
    case Type::kDouble:
      return GetDouble() == other.GetDouble();
    case Type::kBool:
      return GetBool() == other.GetBool();
    case Type::kBytes:
      return GetBytes() == other.GetBytes();
    case Type::kTime:
      return GetTime().tv_sec == other.GetTime().tv_sec &&
             GetTime().tv_nsec == other.GetTime().tv_nsec;
    case Type::kList:
      return GetList() == other.GetList();
    case Type::kMap:
      return GetMap() == other.GetMap();
    }
    return false;
  }

  bool operator!=(const Value &other) const { return !(*this == other); }

  Type GetType() const { return type; }
  int32_t GetInt32() const { return std::get<int32_t>(value); }
  int64_t GetInt64() const { return std::get<int64_t>(value); }
  double GetDouble() const { return std::get<double>(value); }
  bool GetBool() const { return std::get<bool>(value); }
  std::string GetString() const { return std::get<std::string>(value); }
  absl::Span<char> GetBytes() const {
    return std::get<absl::Span<char>>(value);
  }
  struct timespec GetTime() const {
    return std::get<struct timespec>(value);
  }
  const std::vector<Value> &GetList() const {
    return std::get<std::vector<Value>>(value);
  }
  const std::map<std::string, Value> &GetMap() const {
    return std::get<std::map<std::string, Value>>(value);
  }

  void ToProto(adastra::proto::parameters::Value *dest) const;
  void FromProto(const adastra::proto::parameters::Value &proto);

  Type type;
  std::variant<std::string, int32_t, int64_t, double, bool, std::vector<Value>,
               std::map<std::string, Value>, absl::Span<char>, struct timespec>
      value;
};

inline std::ostream &operator<<(std::ostream &os, const Value &v) {
  const char *sep = "";
  switch (v.type) {
  case Type::kString:
    os << v.GetString();
    break;
  case Type::kInt32:
    os << v.GetInt32();
    break;
  case Type::kInt64:
    os << v.GetInt64();
    break;
  case Type::kDouble:
    os << v.GetDouble();
    break;
  case Type::kBool:
    os << v.GetBool();
    break;
  case Type::kBytes: {
    os << "[";
    for (char c : v.GetBytes()) {
      os << sep << (static_cast<int>(c) & 0xff);
      sep = ", ";
    }
    os << "]";
    break;
  }
  case Type::kTime:
    os << v.GetTime().tv_sec << "." << v.GetTime().tv_nsec;
    break;
  case Type::kList:
    os << "[";
    for (const Value &value : v.GetList()) {
      os << sep << value;
      sep = ", ";
    }
    os << "]";
    break;
  case Type::kMap:
    os << "{";
    for (const auto & [ key, value ] : v.GetMap()) {
      os << sep << key << ": " << value;
      sep = ", ";
    }
    os << "}";
    break;
  }
  return os;
}

struct Parameter {
  std::string name;
  parameters::Value value;

  void ToProto(adastra::proto::parameters::Parameter *dest) const;
  void FromProto(const adastra::proto::parameters::Parameter &proto);
};

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

class ParameterServer;

class ParameterNode : public std::enable_shared_from_this<ParameterNode> {
public:
  ParameterNode() = default;
  ParameterNode(std::string name) : name_(std::move(name)) {}
  ParameterNode(std::string name, Value value)
      : name_(std::move(name)), value_(std::move(value)) {}

  ParameterNode(const ParameterNode &) = default;
  ParameterNode &operator=(const ParameterNode &) = default;
  ParameterNode(ParameterNode &&) = default;
  ParameterNode &operator=(ParameterNode &&) = default;
  ~ParameterNode() = default;

  void RemoveChild(const std::string &name) { children_.erase(name); }

  void SetValue(Value value) { value_ = value; }
  void SetValue(int32_t v) { value_ = v; }
  void SetValue(int64_t v) { value_ = v; }
  void SetValue(const std::string &v) { value_ = v; }
  void SetValue(double v) { value_ = v; }
  void SetValue(bool v) { value_ = v; }
  void SetValue(const std::vector<Value> &v) { value_ = v; }
  void SetValue(const std::map<std::string, Value> &v) { value_ = v; }

  const std::string &GetName() const { return name_; }
  const std::string GetFullName() const {
    std::string name = name_;
    ParameterNode *parent = parent_;
    while (parent) {
      name = parent->GetName() + "/" + name;
      parent = parent->parent_;
    }
    return "/" + name;
  }
  const Value &GetValue() const { return value_; }

  void ToProto(adastra::proto::parameters::Parameter *dest) const;
  void FromProto(const adastra::proto::parameters::Parameter &proto);

  void FlattenNames(std::string prefix, std::vector<std::string> &names);
  void Flatten(std::string prefix, std::vector<Parameter> &params);

  void Dump(std::ostream &os);

private:
  friend class ParameterServer;
  std::string name_;
  Value value_;
  absl::flat_hash_map<std::string, std::shared_ptr<ParameterNode>> children_;
  ParameterNode *parent_ = nullptr;
};

class ParameterServer {
public:
  ParameterServer(bool is_local = false) : is_local_(is_local){};
  ParameterServer(const ParameterServer &) = delete;
  ParameterServer &operator=(const ParameterServer &) = delete;
  ParameterServer(ParameterServer &&) = default;
  ParameterServer &operator=(ParameterServer &&) = default;
  ~ParameterServer() = default;

  absl::Status SetParameter(const std::string &name, Value value);

  absl::StatusOr<Value> GetParameter(const std::string &name);
  bool HasParameter(const std::string &name);
  absl::Status DeleteParameter(const std::string &name);
  absl::StatusOr<std::vector<std::string>> ListParameters();
  std::vector<Parameter> GetAllParameters();

  void Dump(std::ostream &os);

private:
  std::vector<std::string> ParseParameterName(std::string name);
  std::shared_ptr<ParameterNode> FindParameter(const std::string &name);
  std::map<std::string, Value>
  MakeMap(const std::shared_ptr<ParameterNode> &node);
  void ExpandMap(const std::map<std::string, Value> &map,
                 std::shared_ptr<ParameterNode> parent);

  bool is_local_;
  absl::flat_hash_map<std::string, std::shared_ptr<ParameterNode>> nodes_;
};
} // namespace adastra::parameters
