// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include "absl/container/flat_hash_map.h"

namespace stagezero {

class Symbol {
 public:
  Symbol(std::string name, std::string value, bool exported)
      : name_(std::move(name)), value_(std::move(value)), exported_(exported) {}
  ~Symbol() = default;

  const std::string &Name() const { return name_; }
  const std::string &Value() const { return value_; }
  void SetValue(std::string value) { value_ = std::move(value); }
  bool Exported() const { return exported_; }

 private:
  std::string name_;
  std::string value_;
  bool exported_;
};

class SymbolTable {
 public:
  SymbolTable() = default;
  SymbolTable(SymbolTable *parent) : parent_(parent) {}
  ~SymbolTable() = default;

  void AddSymbol(std::string name, std::string value, bool exported);
  Symbol *FindSymbol(const std::string &name) const;

  std::string ReplaceSymbols(const std::string &str);

  absl::flat_hash_map<std::string, Symbol *> GetEnvironmentSymbols() const;

  const absl::flat_hash_map<std::string, std::unique_ptr<Symbol>>
      &GetSymbols() {
    return symbols_;
  }

 private:
  absl::flat_hash_map<std::string, std::unique_ptr<Symbol>> symbols_;
  std::vector<Symbol *> exported_symbols_;
  SymbolTable *parent_ = nullptr;
};

}  // namespace stagezero