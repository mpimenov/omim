#include "measure.hpp"
#include "sample.hpp"

namespace search
{
class PfoundCalculator : public Measure
{
  // Measure overrides:
  double Evaluate(vector<Sample> const & samples);
};
}  // namespace search
