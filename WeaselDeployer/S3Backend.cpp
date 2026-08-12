#include "stdafx.h"
#include "S3Backend.h"

#include <WeaselUtility.h>

#include <ctime>
#include <sstream>

#include "CloudHttp.h"

namespace hare {

namespace {

// R2 ignores the region but SigV4 still signs it, and "auto" is what the
// Cloudflare dashboard hands out.
constexpr const char* kRegion = "auto";
constexpr const char* kService = "s3";

struct Timestamp {
  std::string date;      // yyyymmdd
  std::string datetime;  // yyyymmddThhmmssZ
};

Timestamp UtcNow() {
  const std::time_t now = std::time(nullptr);
  std::tm utc = {};
  gmtime_s(&utc, &now);
  char date[16] = {0};
  char datetime[32] = {0};
  std::strftime(date, sizeof(date), "%Y%m%d", &utc);
  std::strftime(datetime, sizeof(datetime), "%Y%m%dT%H%M%SZ", &utc);
  return {date, datetime};
}

// Pulls the <Key> elements out of a ListObjectsV2 response. The document has a
// fixed, flat shape, so scanning for the tag beats linking an XML parser.
std::vector<std::string> ParseKeys(const std::string& xml) {
  std::vector<std::string> keys;
  size_t pos = 0;
  for (;;) {
    const size_t open = xml.find("<Key>", pos);
    if (open == std::string::npos)
      break;
    const size_t close = xml.find("</Key>", open);
    if (close == std::string::npos)
      break;
    keys.push_back(xml.substr(open + 5, close - open - 5));
    pos = close + 6;
  }
  return keys;
}

}  // namespace

S3Backend::S3Backend(S3Settings settings) : settings_(std::move(settings)) {
  // The prefix keeps snapshots in their own corner of a bucket that may hold
  // unrelated objects.
  if (!settings_.prefix.empty() && settings_.prefix.back() != '/')
    settings_.prefix.push_back('/');
}

std::wstring S3Backend::Describe() const {
  return L"S3 bucket " + u8tow(settings_.bucket);
}

std::map<std::wstring, std::wstring> S3Backend::SignedHeaders(
    const std::string& method,
    const std::string& canonical_uri,
    const std::string& canonical_query,
    const std::string& payload) const {
  const Timestamp now = UtcNow();
  const std::string payload_hash = Sha256Hex(payload);

  std::ostringstream canonical;
  canonical << method << '\n'
            << canonical_uri << '\n'
            << canonical_query << '\n'
            << "host:" << settings_.host << '\n'
            << "x-amz-content-sha256:" << payload_hash << '\n'
            << "x-amz-date:" << now.datetime << '\n'
            << '\n'
            << "host;x-amz-content-sha256;x-amz-date" << '\n'
            << payload_hash;

  const std::string scope =
      now.date + "/" + kRegion + "/" + kService + "/aws4_request";

  std::ostringstream to_sign;
  to_sign << "AWS4-HMAC-SHA256\n"
          << now.datetime << '\n'
          << scope << '\n'
          << Sha256Hex(canonical.str());

  const std::string secret = "AWS4" + settings_.secret_key;
  std::vector<uint8_t> key(secret.begin(), secret.end());
  key = HmacSha256(key, now.date);
  key = HmacSha256(key, kRegion);
  key = HmacSha256(key, kService);
  key = HmacSha256(key, "aws4_request");
  const std::string signature = ToHex(HmacSha256(key, to_sign.str()));

  const std::string authorization =
      "AWS4-HMAC-SHA256 Credential=" + settings_.access_key + "/" + scope +
      ", SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=" +
      signature;

  return {{L"Authorization", u8tow(authorization)},
          {L"x-amz-content-sha256", u8tow(payload_hash)},
          {L"x-amz-date", u8tow(now.datetime)}};
}

std::string S3Backend::ObjectUrl(const std::string& key) const {
  return settings_.endpoint + "/" + settings_.bucket + "/" +
         UriEncode(key, false);
}

std::string S3Backend::CanonicalUri(const std::string& key) const {
  return "/" + settings_.bucket + "/" + UriEncode(key, false);
}

bool S3Backend::List(std::vector<std::string>* names) {
  const std::string query =
      "list-type=2&prefix=" + UriEncode(settings_.prefix, true);
  const auto headers =
      SignedHeaders("GET", "/" + settings_.bucket, query, "");
  const std::wstring url =
      u8tow(settings_.endpoint + "/" + settings_.bucket + "?" + query);

  const HttpResponse response = HttpRequest(L"GET", url, headers, "");
  if (!response.ok())
    return false;

  for (const std::string& key : ParseKeys(response.body)) {
    if (key.size() <= settings_.prefix.size())
      continue;
    names->push_back(key.substr(settings_.prefix.size()));
  }
  return true;
}

bool S3Backend::Get(const std::string& name, std::vector<uint8_t>* out) {
  const std::string key = settings_.prefix + name;
  const auto headers = SignedHeaders("GET", CanonicalUri(key), "", "");
  const HttpResponse response =
      HttpRequest(L"GET", u8tow(ObjectUrl(key)), headers, "");
  if (!response.ok())
    return false;
  out->assign(response.body.begin(), response.body.end());
  return true;
}

bool S3Backend::Put(const std::string& name,
                    const std::vector<uint8_t>& data) {
  const std::string key = settings_.prefix + name;
  const std::string body(data.begin(), data.end());
  const auto headers = SignedHeaders("PUT", CanonicalUri(key), "", body);
  const HttpResponse response =
      HttpRequest(L"PUT", u8tow(ObjectUrl(key)), headers, body);
  return response.ok();
}

}  // namespace hare
