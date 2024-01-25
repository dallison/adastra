// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/symbols.h"

namespace adastra::stagezero {

static void WriteInt(uint32_t length, std::stringstream &str) {
  str.put(length & 0xff);
  str.put((length >> 8) & 0xff);
  str.put((length >> 16) & 0xff);
  str.put((length >> 24) & 0xff);
}

static uint32_t ReadInt(std::stringstream &str) {
  uint32_t length = 0;
  for (int i = 0; i < sizeof(length); i++) {
    length |= (str.get() & 0xff) << (i * 8);
    if (str.eof()) {
      return 0;
    }
  }
  return length;
}

static void WriteString(const std::string &s, std::stringstream &str) {
  uint32_t length = uint32_t(s.size());
  WriteInt(length, str);
  str.write(reinterpret_cast<const char *>(s.data()), s.size());
}

static std::string ReadString(std::stringstream &str) {
  uint32_t length = ReadInt(str);
  std::string s;
  for (uint32_t i = 0; i < length; i++) {
    s += str.get() & 0xff;
    if (str.eof()) {
      return "";
    }
  }
  return s;
}

void Symbol::Encode(std::stringstream &str) {
  WriteString(Name(), str);
  WriteString(Value(), str);
  str.put(Exported());
}

std::unique_ptr<Symbol> Symbol::Decode(std::stringstream &str) {
  std::string name = ReadString(str);
  std::string value = ReadString(str);
  bool exported = str.get();
  return std::make_unique<Symbol>(name, value, exported);
}

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
  }
  return it->second.get();
}

absl::flat_hash_map<std::string, Symbol *>
SymbolTable::GetEnvironmentSymbols() const {
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

void SymbolTable::Encode(std::stringstream &str) {
  WriteInt(uint32_t(symbols_.size()), str);
  for (auto & [ _, sym ] : symbols_) {
    sym->Encode(str);
  }
}

void SymbolTable::Decode(std::stringstream &str) {
  uint32_t num_syms = ReadInt(str);
  while (num_syms > 0) {
    auto sym = Symbol::Decode(str);
    symbols_.emplace(sym->Name(), std::move(sym));
    num_syms--;
  }
}
} // namespace adastra::stagezero