#pragma once

#include "spoa/typed_data.h"

#include <string_view>
#include <vector>

namespace spoe {

//! A key-value argument within a SPOE message.
struct Arg {
  std::string_view name;
  TypedData value;
};

//! A single SPOE message sent by HAProxy inside a NOTIFY frame. Views alias
//! the frame payload buffer and remain valid as long as it outlives the
//! message.
struct Message {
  std::string_view name;
  std::vector<Arg> args;

  //! Look up an argument by name; returns nullptr if absent.
  const TypedData *get(std::string_view key) const noexcept {
    for (const auto &arg : args) {
      if (arg.name == key) {
        return &arg.value;
      }
    }
    return nullptr;
  }
};

} // namespace spoe
