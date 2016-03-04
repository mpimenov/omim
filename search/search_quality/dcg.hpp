#include "measure.hpp"
#include "sample.hpp"

namespace search
{
// Discounted Cumulative Gain.
class DCGCalculator : public Measure
{
  // Measure overrides:
  double Evaluate(vector<Sample> const & samples);
};
}  // namespace search
