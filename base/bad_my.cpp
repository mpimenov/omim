#include "base/bad_my.hpp"

#include "base/assert.hpp"

namespace my
{
std::string DebugPrint(Foo const & foo)
{
  CHECK_GREATER(foo.m_id, 0, ());
  return "foo";
}
}  // namespace my
