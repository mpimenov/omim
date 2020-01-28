#include "api/ge0_generator.hpp"

#include "base/assert.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace std;

namespace ge0
{
char Base64Char(int x)
{
  CHECK_GREATER_OR_EQUAL(x, 0, ());
  CHECK_LESS(x, 64, ());
  return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[x];
}

// Map latitude: [-90, 90] -> [0, maxValue]
int LatToInt(double lat, int maxValue)
{
  // M = maxValue, L = maxValue-1
  // lat: -90                        90
  //   x:  0     1     2       L     M
  //       |--+--|--+--|--...--|--+--|
  //       000111111222222...LLLLLMMMM

  double const x = (lat + 90.0) / 180.0 * maxValue;
  return (x < 0 ? 0 : (x > maxValue ? maxValue : (int)(x + 0.5)));
}

// Make lon in [-180, 180)
double LonIn180180(double lon)
{
  if (lon >= 0)
    return fmod(lon + 180.0, 360.0) - 180.0;
  else
  {
    // Handle the case of l = -180
    double const l = fmod(lon - 180.0, 360.0) + 180.0;
    return l < 180.0 ? l : l - 360.0;
  }
}

// Map longitude: [-180, 180) -> [0, maxValue]
int LonToInt(double lon, int maxValue)
{
  double const x = (LonIn180180(lon) + 180.0) / 360.0 * (maxValue + 1.0) + 0.5;
  return (x <= 0 || x >= maxValue + 1) ? 0 : (int)x;
}

void LatLonToString(double lat, double lon, char * s, int nBytes)
{
  if (nBytes > kMaxPointBytes)
    nBytes = kMaxPointBytes;

  int const latI = LatToInt(lat, (1 << kMaxCoordBits) - 1);
  int const lonI = LonToInt(lon, (1 << kMaxCoordBits) - 1);

  int i, shift;
  for (i = 0, shift = kMaxCoordBits - 3; i < nBytes; ++i, shift -= 3)
  {
    int const latBits = latI >> shift & 7;
    int const lonBits = lonI >> shift & 7;

    int const nextByte =
      (latBits >> 2 & 1) << 5 |
      (lonBits >> 2 & 1) << 4 |
      (latBits >> 1 & 1) << 3 |
      (lonBits >> 1 & 1) << 2 |
      (latBits      & 1) << 1 |
      (lonBits      & 1);

    s[i] = Base64Char(nextByte);
  }
}

// Replaces ' ' with '_' and vice versa.
void TransformName(char * s)
{
  for (; *s != 0; ++s)
  {
    if (*s == ' ')
      *s = '_';
    else if (*s == '_')
      *s = ' ';
  }
}

// URL Encode string s.
// Allocates memory that should be freed.
// Returns the lenghts of the resulting string in bytes including terminating 0.
// URL restricted / unsafe / unwise characters are %-encoded.
// See rfc3986, rfc1738, rfc2396.
size_t UrlEncodeString(char const * s, size_t size, char ** res)
{
  size_t i;
  char * out;
  *res = (char*)malloc(size * 3 + 1);
  out = *res;
  for (i = 0; i < size; ++i)
  {
    unsigned char c = (unsigned char)(s[i]);
    switch (c)
    {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
    case 0x7F:
    case ' ':
    case '<':
    case '>':
    case '#':
    case '%':
    case '"':
    case '!':
    case '*':
    case '\'':
    case '(':
    case ')':
    case ';':
    case ':':
    case '@':
    case '&':
    case '=':
    case '+':
    case '$':
    case ',':
    case '/':
    case '?':
    case '[':
    case ']':
    case '{':
    case '}':
    case '|':
    case '^':
    case '`':
      *(out++) = '%';
      *(out++) = "0123456789ABCDEF"[c >> 4];
      *(out++) = "0123456789ABCDEF"[c & 15];
      break;
    default:
      *(out++) = s[i];
    }
  }
  *(out++) = 0;
  return out - *res - 1;
}

// Append s to buf (is there is space). Increment *bytesAppended by size.
void AppendString(char * buf, int bufSize, int * bytesAppended, char const * s, size_t size)
{
  int64_t const bytesAvailable =
      static_cast<int64_t>(bufSize) - static_cast<int64_t>(*bytesAppended);
  if (bytesAvailable > 0)
  {
    int64_t const toCopy =
        static_cast<int64_t>(size) < bytesAvailable ? static_cast<int64_t>(size) : bytesAvailable;
    memcpy(buf + *bytesAppended, s, static_cast<size_t>(toCopy));
  }

  *bytesAppended += size;
}

int GetMaxBufferSize(int nameSize)
{
  return ((nameSize == 0) ? 17 : 17 + 3 * nameSize + 1);
}

int GenShortShowMapUrl(double lat, double lon, double zoom, char const * name, char * buf,
                       int bufSize)
{
  // URL format:
  //
  //       +------------------  1 byte: zoom level
  //       |+-------+---------  9 bytes: lat,lon
  //       ||       | +--+----  Variable number of bytes: point name
  //       ||       | |  |
  // ge0://ZCoordba64/Name

  int fullUrlSize = 0;

  char urlPrefix[] = "ge0://ZCoord6789";

  int const zoomI = (zoom <= 4 ? 0 : (zoom >= 19.75 ? 63 : (int) ((zoom - 4) * 4)));
  urlPrefix[6] = Base64Char(zoomI);

  LatLonToString(lat, lon, urlPrefix + 7, 9);

  AppendString(buf, bufSize, &fullUrlSize, urlPrefix, 16);

  if (name != 0 && name[0] != 0)
  {
    AppendString(buf, bufSize, &fullUrlSize, "/", 1);

    char * newName = strdup(name);
    TransformName(newName);

    char * encName;
    size_t const encNameSize = UrlEncodeString(newName, strlen(newName), &encName);

    AppendString(buf, bufSize, &fullUrlSize, encName, encNameSize);

    free(encName);
    free(newName);
  }

  return fullUrlSize;
}
}  // namespace ge0
