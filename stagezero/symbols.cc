// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/symbols.h"

namespace stagezero {

void SymbolTable::AddSymbol(std::string name, std::string value,
                            bool exported) {
  auto[it, inserted] = symbols_.emplace(std::make_pair(
      name, std::make_unique<Symbol>(name, std::move(value), exported)));
  if (exported) {
    exported_symbols_.push_back(it->second.get());
  }
}

Symbol *SymbolTable::FindSymbol(const std::string &name) const {
  auto it = symbols_.find(name);
  if (it == symbols_.end()) {
    if (parent_ == nullptr) {
      return nullptr;
    }
    return parent_->FindSymbol(name);
    ;
  }
  return it->second.get();
}

absl::flat_hash_map<std::string, Symbol *> SymbolTable::GetEnvironmentSymbols()
    const {
  // Get all global exported symbols.
  absl::flat_hash_map<std::string, Symbol *> symbols;
  if (parent_ != nullptr) {
    symbols = parent_->GetEnvironmentSymbols();
  }

  // Add the locals, replaceing the globals as necessary.
  for (auto *sym : exported_symbols_) {
    auto &s = symbols[sym->Name()];
    s = sym;
  }
  return symbols;
}

std::string SymbolTable::ReplaceSymbols(const std::string &str) {
  std::string result;
  const char *p = str.data();
  while (*p != '\0') {
    if (*p == '$') {
      std::string sym_name;
      p++;
      if (*p == '{') {
        p++;
        while (*p != '\0' && *p != '}') {
          sym_name += *p++;
        }
        if (*p == '}') {
          p++;
        }
      } else if (isalpha(*p) || *p == '_') {
        while (isalnum(*p) || *p == '_') {
          sym_name += *p++;
        }
      }
      Symbol *symbol = FindSymbol(sym_name);
      if (symbol == nullptr) {
        result += sym_name;
      } else {
        result += symbol->Value();
      }
    } else {
      result += *p++;
    }
  }
  return result;
}
}  // namespace stagezero