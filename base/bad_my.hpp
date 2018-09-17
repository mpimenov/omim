#pragma once

#include <cstdint>
#include <string>

namespace my
{
struct Foo
{
  explicit Foo(uint32_t id) : m_id(id) {}

  uint32_t m_id;
};

std::string DebugPrint(Foo const & foo);
}  // namespace my
