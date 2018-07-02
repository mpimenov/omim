#include "search/categories_cache.hpp"
#include "search/mwm_context.hpp"
#include "search/reverse_geocoder.hpp"
#include "search/search_quality/helpers.hpp"
#include "search/search_quality/sample.hpp"

#include "indexer/classificator_loader.hpp"
#include "indexer/data_source.hpp"
#include "indexer/feature_algo.hpp"
#include "indexer/ftypes_matcher.hpp"
#include "indexer/search_string_utils.hpp"

#include "coding/file_name_utils.hpp"

#include "platform/platform.hpp"

#include "geometry/mercator.hpp"

#include "base/macros.hpp"
#include "base/string_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "3party/gflags/src/gflags/gflags.h"

using namespace search::search_quality;
using namespace search;
using namespace std;

DEFINE_string(data_path, "", "Path to data directory (resources dir)");
DEFINE_string(mwm_path, "", "Path to mwm files (writable dir)");
DEFINE_string(out_buildings_path, "buildings.json", "Path to output file for buildings samples");
DEFINE_string(out_cafes_path, "cafes.json", "Path to output file for cafes samples");

double constexpr kMaxDistanceToObjectM = 7500.0;
double constexpr kMinViewportSizeM = 100.0;
double constexpr kMaxViewportSizeM = 5000.0;
size_t constexpr kMaxSamplesPerMwm = 20;

std::random_device g_rd;
mt19937 g_rng(g_rd());

enum class RequestType
{
  Building,
  Cafe
};

bool GetRandomBool()
{
  std::uniform_int_distribution<> dis(0, 1);
  return dis(g_rng) == 0;
}

// Leaves first letter as is.
void EraseRandom(strings::UniString & uni)
{
  if (uni.size() <= 1)
    return;

  uniform_int_distribution<size_t> dis(1, uni.size() - 1);
  auto const it = uni.begin() + dis(g_rng);
  uni.erase(it, it + 1);
}

// Leaves first letter as is.
void SwapRandom(strings::UniString & uni)
{
  if (uni.size() <= 2)
    return;

  uniform_int_distribution<size_t> dis(1, uni.size() - 2);
  auto const index = dis(g_rng);
  swap(uni[index], uni[index + 1]);
}

void AddRandomMisprint(strings::UniString & str)
{
  // todo(@t.yan): Disable for hieroglyphs, consider implementing InsertRandom.
  if (GetRandomBool())
    return EraseRandom(str);
  return SwapRandom(str);
}

void AddMisprints(string & str)
{
  auto tokens = strings::Tokenize(str, " -&");
  str.clear();
  for (size_t i = 0; i < tokens.size(); ++i)
  {
    auto const & token = tokens[i];
    auto uni = strings::MakeUniString(token);
    if (uni.size() > 4 && g_rng() % 4)
      AddRandomMisprint(uni);
    if (uni.size() > 8 && g_rng() % 4)
      AddRandomMisprint(uni);

    str += strings::ToUtf8(uni);
    if (i != tokens.size() - 1)
      str += " ";
  }
}

map<string, vector<string>> const kStreetSynonyms = {
    {"улица", {"ул", "у"}},
    {"проспект", {"пр-т", "пр", "пркт", "прт", "пр-кт"}},
    {"переулок", {"пер"}},
    {"проезд", {"пр-д", "пр", "прд"}},
    {"аллея", {"ал"}},
    {"бульвар", {"б-р", "бр"}},
    {"набережная", {"наб", "наб-я"}},
    {"шоссе", {"шос"}},
    {"вyлица", {"вул"}},
    {"площадь", {"пл", "площ"}},
    {"тупик", {"туп"}},
    {"street", {"str", "st"}},
    {"avenue", {"ave", "av"}},
    {"boulevard", {"bld", "blv", "bv", "blvd"}},
    {"drive", {"dr"}},
    {"highway", {"hw", "hwy"}},
    {"road", {"rd"}},
    {"square", {"sq"}}};

void ModifyStreet(string & str)
{
  auto tokens = strings::Tokenize(str, " -&");
  str.clear();

  auto const isStreetSynonym = [](string const & s) {
    return kStreetSynonyms.find(s) != kStreetSynonyms.end();
  };

  auto const synonymIt = find_if(tokens.begin(), tokens.end(), isStreetSynonym);
  if (synonymIt != tokens.end() &&
      find_if(synonymIt + 1, tokens.end(), isStreetSynonym) == tokens.end())
  {
    // Only one street synonym.
    if (GetRandomBool())
    {
      if (GetRandomBool())
      {
        auto const & synonyms = kStreetSynonyms.at(*synonymIt);
        uniform_int_distribution<size_t> dis(0, synonyms.size() - 1);
        *synonymIt = synonyms[dis(g_rng)];
      }
      else
      {
        tokens.erase(synonymIt);
      }
    }
    // Else leave as is.
  }

  if (tokens.empty())
    return;

  str = strings::JoinStrings(tokens, " ");
  AddMisprints(str);
}

void ModifyHouse(uint8_t lang, string & str)
{
  if (str.empty())
    return;

  if (lang == StringUtf8Multilang::GetLangIndex("ru") && isdigit(str[0]))
  {
    uniform_int_distribution<size_t> dis(0, 4);
    auto const r = dis(g_rng);
    if (r == 0)
      str = "д " + str;
    if (r == 1)
      str = "д" + str;
    if (r == 2)
      str = "дом " + str;
    if (r == 3)
      str = "д. " + str;
    // Else leave housenumber as is.
  }
}

m2::PointD GenerateNearbyPosition(m2::PointD const & point)
{
  uniform_real_distribution<double> dis(-kMaxDistanceToObjectM, kMaxDistanceToObjectM);
  return MercatorBounds::GetSmPoint(point, dis(g_rng) /* dX */, dis(g_rng) /* dY */);
}

m2::RectD GenerateNearbyViewport(m2::PointD const & point)
{
  uniform_real_distribution<double> dis(kMinViewportSizeM, kMaxViewportSizeM);
  return MercatorBounds::RectByCenterXYAndSizeInMeters(GenerateNearbyPosition(point), dis(g_rng));
}

bool GetBuildingInfo(FeatureType & ft, search::ReverseGeocoder const & coder, string & street)
{
  if (ft.GetHouseNumber().empty())
    return false;

  street = coder.GetFeatureStreetName(ft);
  if (street.empty())
    return false;

  return true;
}

bool GetCafeInfo(FeatureType & ft, search::ReverseGeocoder const & coder, string & street,
                 uint32_t & cafeType, string & name)
{
  if (!ft.HasName())
    return false;

  auto const names = ft.GetNames();
  if (!names.GetString(StringUtf8Multilang::kDefaultCode, name))
    return false;

  for (auto const t : feature::TypesHolder(ft))
  {
    if (ftypes::IsEatChecker::Instance()(t))
    {
      cafeType = t;
      street = coder.GetFeatureStreetName(ft);
      return !street.empty();
    }
  }

  return false;
}

string ModifyAddress(string street, string house, uint8_t lang)
{
  if (street.empty() || house.empty())
    return "";

  ModifyStreet(street);
  ModifyHouse(lang, house);

  if (GetRandomBool())
    return street + " " + house;

  return house + " " + street;
}

string CombineRandomly(string const & mandatory, string const & optional)
{
  if (optional.empty() || GetRandomBool())
    return mandatory;
  if (GetRandomBool())
    return optional + " " + mandatory;
  return mandatory + " " + optional;
}

void ModifyCafe(string const & name, string const & type, string & out)
{
  out = CombineRandomly(name, type);
  AddMisprints(out);
}

string GetLocalizedCafeType(unordered_map<uint32_t, StringUtf8Multilang> const & typesTranslations,
                            uint32_t type, uint8_t lang)
{
  auto const it = typesTranslations.find(type);
  if (it == typesTranslations.end())
    return {};
  string translation;
  if (it->second.GetString(lang, translation))
    return translation;
  it->second.GetString(StringUtf8Multilang::kEnglishCode, translation);
  return translation;
}

boost::optional<Sample> GenerateRequest(
    FeatureType & ft, search::ReverseGeocoder const & coder,
    unordered_map<uint32_t, StringUtf8Multilang> const & typesTranslations,
    vector<int8_t> const & mwmLangCodes, RequestType requestType)
{
  string street;
  string cafeStr;
  auto const lang = !mwmLangCodes.empty() ? mwmLangCodes[0] : StringUtf8Multilang::kEnglishCode;

  switch (requestType)
  {
  case RequestType::Building:
  {
    if (!GetBuildingInfo(ft, coder, street))
      return {};
    break;
  }
  case RequestType::Cafe:
  {
    uint32_t type;
    string name;
    if (!GetCafeInfo(ft, coder, street, type, name))
      return {};

    auto const cafeType = GetLocalizedCafeType(typesTranslations, type, lang);
    ModifyCafe(name, cafeType, cafeStr);
    break;
  }
  }

  auto const house = ft.GetHouseNumber();
  auto const featureCenter = feature::GetCenter(ft);
  auto const address = ModifyAddress(street, house, lang);
  auto const query = cafeStr.empty() ? address : CombineRandomly(cafeStr, address);

  Sample sample;
  sample.m_query = strings::MakeUniString(query);
  sample.m_locale = StringUtf8Multilang::GetLangByCode(lang);
  sample.m_pos = GenerateNearbyPosition(featureCenter);
  sample.m_viewport = GenerateNearbyViewport(featureCenter);
  sample.m_results.push_back(Sample::Result::Build(ft, Sample::Result::Relevance::Vital));

  return sample;
}

unordered_map<uint32_t, StringUtf8Multilang> ParseStrings()
{
  auto const stringsFile =
      base::JoinPath(GetPlatform().ResourcesDir(), "strings", "types_strings.txt");
  ifstream s(stringsFile);
  CHECK(s.is_open(), ("Cannot open", stringsFile));

  // Skip the first [[Types]] line.
  string line;
  getline(s, line);

  uint32_t type = 0;
  auto const typePrefixSize = strlen("[type.");
  auto const typePostfixSize = strlen("]");
  unordered_map<uint32_t, StringUtf8Multilang> typesTranslations;
  while (s.good())
  {
    getline(s, line);
    strings::Trim(line);

    // Allow for comments starting with '#' character.
    if (line.empty() || line[0] == '#')
      continue;

    // New type.
    if (line[0] == '[')
    {
      CHECK_GREATER(line.size(), typePrefixSize + typePostfixSize, (line));
      auto typeString = line.substr(typePrefixSize, line.size() - typePrefixSize - typePostfixSize);
      auto const tokens = strings::Tokenize(typeString, ".");
      type = classif().GetTypeByPath(tokens);
    }
    else
    {
      // We can not get initial type by types_strings key for some types like
      // "amenity parking multi-storey" which is stored like amenity.parking.multi.storey
      // and can not be tokenized correctly.
      if (type == 0)
        continue;
      auto const pos = line.find("=");
      CHECK_NOT_EQUAL(pos, string::npos, ());
      auto lang = line.substr(0, pos);
      strings::Trim(lang);
      auto translation = line.substr(pos + 1);
      strings::Trim(translation);
      typesTranslations[type].AddString(lang, translation);
    }
  }
  return typesTranslations;
}

int main(int argc, char * argv[])
{
  ChangeMaxNumberOfOpenFiles(kMaxOpenFiles);

  google::SetUsageMessage("Samples generation tool.");
  google::ParseCommandLineFlags(&argc, &argv, true);

  SetPlatformDirs(FLAGS_data_path, FLAGS_mwm_path);

  ofstream buildingsOut;
  buildingsOut.open(FLAGS_out_buildings_path);
  CHECK(buildingsOut.is_open(), ("Can't open output file", FLAGS_out_buildings_path));

  ofstream cafesOut;
  cafesOut.open(FLAGS_out_cafes_path);
  CHECK(cafesOut.is_open(), ("Can't open output file", FLAGS_out_cafes_path));

  classificator::Load();
  FrozenDataSource dataSource;
  InitDataSource(dataSource, "" /* mwmListPath */);
  search::ReverseGeocoder const coder(dataSource);
  auto const typesTranslations = ParseStrings();

  vector<shared_ptr<MwmInfo>> mwmInfos;
  dataSource.GetMwmsInfo(mwmInfos);
  for (auto const & mwmInfo : mwmInfos)
  {
    MwmSet::MwmId const mwmId(mwmInfo);
    LOG(LINFO, ("Start generation for", mwmId));

    vector<int8_t> mwmLangCodes;
    mwmInfo->GetRegionData().GetLanguages(mwmLangCodes);

    auto handle = dataSource.GetMwmHandleById(mwmId);
    auto & value = *handle.GetValue<MwmValue>();
    // WorldCoasts.
    if (!value.HasSearchIndex())
      continue;

    MwmContext const mwmContext(move(handle));
    base::Cancellable const cancellable;
    FeaturesLoaderGuard g(dataSource, mwmId);

    auto generate = [&](ftypes::BaseChecker const & checker, RequestType type, ofstream & out) {
      CategoriesCache cache(checker, cancellable);
      auto features = cache.Get(mwmContext);

      vector<uint32_t> fids;
      features.ForEach(
          [&fids](uint64_t fid) { fids.push_back(base::asserted_cast<uint32_t>(fid)); });
      shuffle(fids.begin(), fids.end(), g_rng);

      size_t numSamples = 0;
      for (auto const fid : fids)
      {
        if (numSamples >= kMaxSamplesPerMwm)
          break;

        auto ft = g.GetFeatureByIndex(fid);
        CHECK(ft, ());
        auto const sample = GenerateRequest(*ft, coder, typesTranslations, mwmLangCodes, type);
        if (sample)
        {
          string json;
          Sample::SerializeToJSONLines({*sample}, json);
          out << json;
          ++numSamples;
        }
      }
    };

    generate(ftypes::IsBuildingChecker::Instance(), RequestType::Building, buildingsOut);
    generate(ftypes::IsEatChecker::Instance(), RequestType::Cafe, cafesOut);
  }
  return 0;
}
