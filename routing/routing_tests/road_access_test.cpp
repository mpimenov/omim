#include "testing/testing.hpp"

#include "routing/road_access.hpp"
#include "routing/road_access_serialization.hpp"
#include "routing/routing_tests/index_graph_tools.hpp"

#include "coding/reader.hpp"
#include "coding/writer.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace routing;
using namespace routing_test;
using namespace std;

using Edge = TestIndexGraphTopology::Edge;

namespace
{
UNIT_TEST(RoadAccess_Serialization)
{
  vector<uint32_t> privateRoads = {1, 2, 3, 100};
  RoadAccess roadAccess;
  roadAccess.SetPrivateRoads(move(privateRoads));

  vector<uint8_t> buf;
  {
    MemWriter<decltype(buf)> writer(buf);
    RoadAccessSerializer::Serialize(writer, roadAccess);
  }

  RoadAccess deserializedRoadAccess;
  {
    MemReader memReader(buf.data(), buf.size());
    ReaderSource<MemReader> src(memReader);
    RoadAccessSerializer::Deserialize(src, deserializedRoadAccess);
  }

  TEST_EQUAL(roadAccess, deserializedRoadAccess, ());

  {
    auto const & b = deserializedRoadAccess.GetPrivateRoads();
    TEST(is_sorted(b.begin(), b.end()), ());
  }
}

UNIT_TEST(RoadAccess_WayBlocked)
{
  // Add edges to the graph in the following format: (from, to, weight).
  // Block edges in the following format: (from, to).

  uint32_t const numVertices = 4;
  TestIndexGraphTopology graph(numVertices);

  graph.AddDirectedEdge(0, 1, 1.0);
  graph.AddDirectedEdge(1, 2, 1.0);
  graph.AddDirectedEdge(2, 3, 1.0);

  double const expectedWeight = 0.0;
  vector<Edge> const expectedEdges = {};

  graph.BlockEdge(1, 2);

  TestTopologyGraph(graph, 0, 3, false /* pathFound */, expectedWeight, expectedEdges);
}

UNIT_TEST(RoadAccess_BarrierBypassing)
{
  uint32_t const numVertices = 6;
  TestIndexGraphTopology graph(numVertices);

  graph.AddDirectedEdge(0, 1, 1.0);
  graph.AddDirectedEdge(1, 2, 1.0);
  graph.AddDirectedEdge(2, 5, 1.0);
  graph.AddDirectedEdge(0, 3, 1.0);
  graph.AddDirectedEdge(3, 4, 2.0);
  graph.AddDirectedEdge(4, 5, 1.0);

  double expectedWeight;
  vector<Edge> expectedEdges;

  expectedWeight = 3.0;
  expectedEdges = {{0, 1}, {1, 2}, {2, 5}};
  TestTopologyGraph(graph, 0, 5, true /* pathFound */, expectedWeight, expectedEdges);

  graph.BlockEdge(1, 2);
  expectedWeight = 4.0;
  expectedEdges = {{0, 3}, {3, 4}, {4, 5}};
  TestTopologyGraph(graph, 0, 5, true /* pathFound */, expectedWeight, expectedEdges);

  graph.BlockEdge(3, 4);
  TestTopologyGraph(graph, 0, 5, false /* pathFound */, expectedWeight, expectedEdges);
}
}  // namespace
