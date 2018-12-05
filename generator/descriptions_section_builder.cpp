#include "generator/descriptions_section_builder.hpp"

#include "platform/platform.hpp"

#include "coding/multilang_utf8_string.hpp"

#include "base/assert.hpp"
#include "base/string_utils.hpp"

#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>

namespace
{
bool IsValidDir(std::string const & path)
{
  return Platform::IsFileExistsByFullPath(path) && Platform::IsDirectory(path);
}

std::string GetFileName(std::string path)
{
  base::GetNameFromFullPath(path);
  return path;
}
}  // namespace

namespace generator
{
std::string DescriptionsCollectionBuilderStat::LangStatisticsToString() const
{
  std::stringstream stream;
  stream << "Language statistics - ";
  if (m_langsStat.empty())
    stream << "(empty)";

  for (size_t code = 0; code < m_langsStat.size(); ++code)
  {
    if (m_langsStat[code] == 0)
      continue;

    stream << StringUtf8Multilang::GetLangByCode(static_cast<int8_t>(code))
           << ":" << m_langsStat[code] << " ";
  }

  return stream.str();
}

DescriptionsCollectionBuilder::DescriptionsCollectionBuilder(std::string const & wikipediaDir,
                                                             std::string const & mwmFile)
  : m_wikipediaDir(wikipediaDir), m_mwmFile(mwmFile) {}

// static
std::string DescriptionsCollectionBuilder::MakePath(std::string const & wikipediaDir, std::string wikipediaUrl)
{
  strings::Trim(wikipediaUrl);
  strings::ReplaceFirst(wikipediaUrl, "http://", "");
  strings::ReplaceFirst(wikipediaUrl, "https://", "");
  if (strings::EndsWith(wikipediaUrl, "/"))
    wikipediaUrl.pop_back();

  return base::JoinPath(wikipediaDir, wikipediaUrl);
}

// static
size_t DescriptionsCollectionBuilder::FillStringFromFile(std::string const & fullPath, int8_t code,
                                                         StringUtf8Multilang & str)
{
  std::fstream stream;
  stream.exceptions(std::fstream::failbit | std::fstream::badbit);
  stream.open(fullPath);
  std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  auto const contentSize = content.size();
  if (contentSize != 0)
    str.AddString(code, std::move(content));

  return contentSize;
}

boost::optional<size_t> DescriptionsCollectionBuilder::FindPageAndFill(std::string wikipediaUrl,
                                                                       StringUtf8Multilang & str)
{
  auto const path = MakePath(m_wikipediaDir, wikipediaUrl);
  if (!IsValidDir(path))
  {
    LOG(LWARNING, ("Directory", path, "not found."));
    return {};
  }

  size_t size = 0;
  Platform::FilesList filelist;
  Platform::GetFilesByExt(path, ".html", filelist);
  for (auto const & entry : filelist)
  {
    auto const filename = GetFileName(entry);
    auto const lang = base::FilenameWithoutExt(filename);
    auto const code = StringUtf8Multilang::GetLangIndex(lang);
    if (code == StringUtf8Multilang::kUnsupportedLanguageCode)
    {
      LOG(LWARNING, (lang, "is an unsupported language."));
      continue;
    }

    m_stat.IncCode(code);
    auto const fullPath = base::JoinPath(path, filename);
    size += FillStringFromFile(fullPath, code, str);
  }

  return size;
}

size_t DescriptionsCollectionBuilder::GetFeatureDescription(FeatureType & f, uint32_t featureId,
                                                            descriptions::FeatureDescription & description)
{
  auto const wikiUrl = f.GetMetadata().GetWikiURL();
  if (wikiUrl.empty())
    return 0;

  StringUtf8Multilang string;
  auto const ret = FindPageAndFill(wikiUrl, string);
  if (!ret || *ret == 0)
    return 0;

  description = descriptions::FeatureDescription(featureId, std::move(string));
  return *ret;
}

void BuildDescriptionsSection(std::string const & wikipediaDir, std::string const & mwmFile)
{
  DescriptionsSectionBuilder<>::Build(wikipediaDir, mwmFile);
}

namespace
{
using namespace std;

void ExtractDescriptions(string const & mwmPath, vector<descriptions::FeatureDescription> & ds)
{
  FilesContainerR container(mwmPath);
  CHECK(container.IsExist(DESCRIPTIONS_FILE_TAG), ());

  descriptions::Deserializer d;
  auto reader = container.GetReader(DESCRIPTIONS_FILE_TAG);

  vector<int8_t> langCodes;
  for (auto const & lang : StringUtf8Multilang::GetSupportedLanguages())
  {
    string s(lang.m_code);
    if (s != "en" && s != "ru" && s != "es")
      continue;

    langCodes.emplace_back(StringUtf8Multilang::GetLangIndex(lang.m_code));
  }

  auto const fn = [&](FeatureType &, uint32_t fid) {
    StringUtf8Multilang multiStr;
    for (int8_t lang : langCodes)
    {
      vector<int8_t> const oneLang = {lang};
      string str;
      if (d.Deserialize(*reader.GetPtr(), fid, oneLang, str))
        multiStr.AddString(lang, str);
    }
    if (!multiStr.IsEmpty())
      ds.emplace_back(fid, move(multiStr));
  };

  feature::ForEachFromDat(mwmPath, fn);
  LOG(LINFO, ("Extracted", ds.size(), "descriptions"));
}
}  // namespace

void RebuildDescriptionsSection(std::string const & mwmPath)
{
  std::vector<descriptions::FeatureDescription> ds;
  ExtractDescriptions(mwmPath, ds);

  descriptions::Serializer serializer(std::move(ds));

  std::vector<size_t> const blockSizes = {5000, 10000, 20000, 50000, 100000, 200000, 400000, 500000, 1000000};
  for (size_t const sz : blockSizes)
  {
    std::string const tag = std::string(DESCRIPTIONS_FILE_TAG) + "_" + strings::to_string(sz);

    {
      FilesContainerW cont(mwmPath, FileWriter::OP_WRITE_EXISTING);
      FileWriter writer = cont.GetWriter(tag);
      serializer.Serialize(writer, sz);
    }
    FilesContainerR cont(mwmPath);
    LOG(LINFO, ("Section", tag, "is built for", mwmPath, "size =", cont.GetReader(tag).Size()));
  }
}
}  // namespace generator
