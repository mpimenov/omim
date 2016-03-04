#include "sample.hpp"

namespace search
{
class Measure
{
  virtual double Evaluate(vector<Sample> const & samples) = 0;
};
}  // namespace search
