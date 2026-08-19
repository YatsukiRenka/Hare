// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "S3Backend.h"

#include <WeaselUtility.h>

#include <ctime>
#include <limits>
#include <sstream>
#include <xmllite.h>

#include "CloudHttp.h"
#include "CloudStorage.h"

namespace hare {

namespace {

// R2 ignores the region but SigV4 still signs it, and "auto" is what the
// Cloudflare dashboard hands out.
constexpr const char* kRegion = "auto";
constexpr const char* kService = "s3";
constexpr const wchar_t* kS3Namespace =
    L"http://s3.amazonaws.com/doc/2006-03-01/";

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

struct XmlInput {
  CComPtr<IStream> stream;
  CComPtr<IXmlReader> reader;
};

bool OpenXml(const std::string& body, XmlInput* input) {
  if (body.empty() || body.size() > (std::numeric_limits<ULONG>::max)())
    return false;

  IStream* raw_stream = nullptr;
  HRESULT result = CreateStreamOnHGlobal(nullptr, TRUE, &raw_stream);
  if (FAILED(result))
    return false;
  input->stream.Attach(raw_stream);
  ULONG written = 0;
  if (FAILED(input->stream->Write(body.data(), static_cast<ULONG>(body.size()),
                                  &written)) ||
      written != body.size()) {
    return false;
  }
  LARGE_INTEGER beginning = {};
  if (FAILED(input->stream->Seek(beginning, STREAM_SEEK_SET, nullptr)))
    return false;

  IXmlReader* raw_reader = nullptr;
  result = ::CreateXmlReader(__uuidof(IXmlReader),
                             reinterpret_cast<void**>(&raw_reader), nullptr);
  if (FAILED(result))
    return false;
  input->reader.Attach(raw_reader);

  if (FAILED(input->reader->SetProperty(XmlReaderProperty_DtdProcessing,
                                        DtdProcessing_Prohibit)) ||
      FAILED(input->reader->SetProperty(XmlReaderProperty_ConformanceLevel,
                                        XmlConformanceLevel_Document)) ||
      FAILED(
          input->reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64)) ||
      FAILED(input->reader->SetInput(input->stream))) {
    return false;
  }
  return true;
}

bool CurrentElement(IXmlReader* reader,
                    std::wstring* local_name,
                    std::wstring* namespace_uri) {
  LPCWSTR value = nullptr;
  UINT length = 0;
  if (FAILED(reader->GetLocalName(&value, &length)))
    return false;
  local_name->assign(value, length);
  if (FAILED(reader->GetNamespaceUri(&value, &length)))
    return false;
  namespace_uri->assign(value, length);
  return true;
}

bool AppendCurrentValue(IXmlReader* reader, std::wstring* value) {
  LPCWSTR chunk = nullptr;
  UINT length = 0;
  if (FAILED(reader->GetValue(&chunk, &length)))
    return false;
  value->append(chunk, length);
  return true;
}

std::wstring TrimXmlWhitespace(const std::wstring& value) {
  const wchar_t* whitespace = L" \t\r\n";
  const size_t begin = value.find_first_not_of(whitespace);
  if (begin == std::wstring::npos)
    return std::wstring();
  const size_t end = value.find_last_not_of(whitespace);
  return value.substr(begin, end - begin + 1);
}

struct S3ListPage {
  std::vector<std::string> keys;
  bool truncated = false;
  std::string continuation_token;
};

enum class S3Element {
  kOther,
  kRoot,
  kContents,
  kKey,
  kIsTruncated,
  kContinuationToken,
};

struct S3Frame {
  S3Element element = S3Element::kOther;
  bool has_key = false;
};

bool IsS3ListElement(const std::wstring& local_name) {
  return local_name == L"Contents" || local_name == L"Key" ||
         local_name == L"IsTruncated" || local_name == L"NextContinuationToken";
}

bool ParseS3ListPage(const std::string& body, S3ListPage* page) {
  XmlInput input;
  if (!OpenXml(body, &input))
    return false;

  S3ListPage parsed;
  std::vector<S3Frame> stack;
  std::wstring text;
  bool root_closed = false;
  bool saw_is_truncated = false;
  bool saw_continuation_token = false;

  const auto close_element = [&]() -> bool {
    if (stack.empty())
      return false;
    const S3Frame frame = stack.back();
    switch (frame.element) {
      case S3Element::kContents:
        if (!frame.has_key)
          return false;
        break;
      case S3Element::kKey: {
        const std::string key = wtou8(text);
        if (key.empty())
          return false;
        parsed.keys.push_back(key);
        break;
      }
      case S3Element::kIsTruncated: {
        const std::wstring value = TrimXmlWhitespace(text);
        if (value == L"true" || value == L"1") {
          parsed.truncated = true;
        } else if (value == L"false" || value == L"0") {
          parsed.truncated = false;
        } else {
          return false;
        }
        break;
      }
      case S3Element::kContinuationToken:
        parsed.continuation_token = wtou8(text);
        break;
      case S3Element::kRoot:
        root_closed = true;
        break;
      default:
        break;
    }
    stack.pop_back();
    return true;
  };

  XmlNodeType node_type = XmlNodeType_None;
  HRESULT result = S_OK;
  while ((result = input.reader->Read(&node_type)) == S_OK) {
    if (node_type == XmlNodeType_Element) {
      if (!stack.empty() &&
          (stack.back().element == S3Element::kKey ||
           stack.back().element == S3Element::kIsTruncated ||
           stack.back().element == S3Element::kContinuationToken)) {
        return false;
      }

      std::wstring local_name;
      std::wstring namespace_uri;
      if (!CurrentElement(input.reader, &local_name, &namespace_uri))
        return false;

      S3Frame frame;
      if (stack.empty()) {
        if (root_closed || local_name != L"ListBucketResult" ||
            namespace_uri != kS3Namespace || input.reader->IsEmptyElement()) {
          return false;
        }
        frame.element = S3Element::kRoot;
      } else if (namespace_uri != kS3Namespace && IsS3ListElement(local_name)) {
        return false;
      } else if (namespace_uri == kS3Namespace &&
                 stack.back().element == S3Element::kRoot &&
                 local_name == L"Contents") {
        frame.element = S3Element::kContents;
      } else if (namespace_uri == kS3Namespace &&
                 stack.back().element == S3Element::kContents &&
                 local_name == L"Key") {
        if (stack.back().has_key)
          return false;
        stack.back().has_key = true;
        frame.element = S3Element::kKey;
        text.clear();
      } else if (namespace_uri == kS3Namespace &&
                 stack.back().element == S3Element::kRoot &&
                 local_name == L"IsTruncated") {
        if (saw_is_truncated)
          return false;
        saw_is_truncated = true;
        frame.element = S3Element::kIsTruncated;
        text.clear();
      } else if (namespace_uri == kS3Namespace &&
                 stack.back().element == S3Element::kRoot &&
                 local_name == L"NextContinuationToken") {
        if (saw_continuation_token)
          return false;
        saw_continuation_token = true;
        frame.element = S3Element::kContinuationToken;
        text.clear();
      } else if (namespace_uri == kS3Namespace && IsS3ListElement(local_name)) {
        return false;
      }

      stack.push_back(frame);
      if (input.reader->IsEmptyElement() && !close_element())
        return false;
    } else if (node_type == XmlNodeType_EndElement) {
      if (!close_element())
        return false;
    } else if ((node_type == XmlNodeType_Text ||
                node_type == XmlNodeType_CDATA ||
                node_type == XmlNodeType_Whitespace) &&
               !stack.empty() &&
               (stack.back().element == S3Element::kKey ||
                stack.back().element == S3Element::kIsTruncated ||
                stack.back().element == S3Element::kContinuationToken)) {
      if (!AppendCurrentValue(input.reader, &text))
        return false;
    }
  }

  if (result != S_FALSE || !stack.empty() || !root_closed ||
      !saw_is_truncated ||
      (parsed.truncated &&
       (!saw_continuation_token || parsed.continuation_token.empty()))) {
    return false;
  }
  *page = std::move(parsed);
  return true;
}

}  // namespace

S3Backend::S3Backend(S3Settings settings) : settings_(std::move(settings)) {
  // A configured endpoint ending in a slash would produce "//bucket/..." in the
  // request while the signature covers "/bucket/...", and services that
  // compare the two reject it.
  while (!settings_.endpoint.empty() && settings_.endpoint.back() == '/')
    settings_.endpoint.pop_back();

  // The prefix keeps snapshots in their own corner of a bucket that may hold
  // unrelated objects.
  settings_.prefix = CanonicalS3Prefix(std::move(settings_.prefix));
}

std::wstring S3Backend::Describe() const {
  return L"S3 bucket " + u8tow(settings_.bucket);
}

std::map<std::wstring, std::wstring> S3Backend::SignedHeaders(
    const std::string& method,
    const std::string& canonical_uri,
    const std::string& canonical_query,
    const std::string& payload,
    bool if_none_match) const {
  const Timestamp now = UtcNow();
  const std::string payload_hash = Sha256Hex(payload);
  const std::string signed_header_names =
      if_none_match ? "host;if-none-match;x-amz-content-sha256;x-amz-date"
                    : "host;x-amz-content-sha256;x-amz-date";

  std::ostringstream canonical;
  canonical << method << '\n'
            << canonical_uri << '\n'
            << canonical_query << '\n'
            << "host:" << settings_.host << '\n';
  if (if_none_match)
    canonical << "if-none-match:*\n";
  canonical << "x-amz-content-sha256:" << payload_hash << '\n'
            << "x-amz-date:" << now.datetime << '\n'
            << '\n'
            << signed_header_names << '\n'
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
      ", SignedHeaders=" + signed_header_names + ", Signature=" + signature;

  std::map<std::wstring, std::wstring> headers = {
      {L"Authorization", u8tow(authorization)},
      {L"x-amz-content-sha256", u8tow(payload_hash)},
      {L"x-amz-date", u8tow(now.datetime)}};
  if (if_none_match)
    headers[L"If-None-Match"] = L"*";
  return headers;
}

std::string S3Backend::ObjectUrl(const std::string& key) const {
  return settings_.endpoint + "/" + settings_.bucket + "/" +
         UriEncode(key, false);
}

std::string S3Backend::CanonicalUri(const std::string& key) const {
  return "/" + settings_.bucket + "/" + UriEncode(key, false);
}

bool S3Backend::List(std::vector<std::string>* names) {
  // A listing is capped at 1000 keys, so the continuation token has to be
  // followed; stopping after the first page would silently drop snapshots
  // once enough devices or dictionaries accumulate.
  std::vector<std::string> listed_names;
  std::string continuation_token;
  for (;;) {
    std::string query =
        "list-type=2&prefix=" + UriEncode(settings_.prefix, true);
    if (!continuation_token.empty()) {
      // Query parameters must be signed in sorted order, and
      // continuation-token sorts before list-type and prefix.
      query = "continuation-token=" + UriEncode(continuation_token, true) +
              "&" + query;
    }

    const auto headers =
        SignedHeaders("GET", "/" + settings_.bucket, query, "", false);
    const std::wstring url =
        u8tow(settings_.endpoint + "/" + settings_.bucket + "?" + query);

    const HttpResponse response = HttpRequest(L"GET", url, headers, "");
    if (!response.ok())
      return false;

    S3ListPage page;
    if (!ParseS3ListPage(response.body, &page))
      return false;
    for (const std::string& key : page.keys) {
      std::string name;
      const ObjectPrefixResult prefix_result =
          RemoveObjectPrefix(key, settings_.prefix, &name);
      if (prefix_result == ObjectPrefixResult::kInvalid)
        return false;
      if (prefix_result == ObjectPrefixResult::kPrefixMarker)
        continue;
      listed_names.push_back(std::move(name));
    }

    if (!page.truncated) {
      *names = std::move(listed_names);
      return true;
    }
    if (page.continuation_token == continuation_token)
      return false;
    continuation_token = std::move(page.continuation_token);
  }
}

FetchResult S3Backend::Get(const std::string& name, std::vector<uint8_t>* out) {
  const std::string key = settings_.prefix + name;
  const auto headers = SignedHeaders("GET", CanonicalUri(key), "", "", false);
  const HttpResponse response =
      HttpRequest(L"GET", u8tow(ObjectUrl(key)), headers, "");
  if (response.failure == HttpFailure::kPayloadTooLarge)
    return FetchResult::kPayloadTooLarge;
  if (response.status == 404)
    return FetchResult::kNotFound;
  if (!response.ok())
    return FetchResult::kError;
  out->assign(response.body.begin(), response.body.end());
  return FetchResult::kOk;
}

bool S3Backend::Put(const std::string& name, const std::vector<uint8_t>& data) {
  const std::string key = settings_.prefix + name;
  const std::string body(data.begin(), data.end());
  const auto headers = SignedHeaders("PUT", CanonicalUri(key), "", body, false);
  const HttpResponse response =
      HttpRequest(L"PUT", u8tow(ObjectUrl(key)), headers, body);
  return response.ok();
}

PutIfAbsentResult S3Backend::PutIfAbsent(const std::string& name,
                                         const std::vector<uint8_t>& data) {
  const std::string key = settings_.prefix + name;
  const std::string body(data.begin(), data.end());
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto headers =
        SignedHeaders("PUT", CanonicalUri(key), "", body, true);
    const HttpResponse response =
        HttpRequest(L"PUT", u8tow(ObjectUrl(key)), headers, body);
    if (response.ok())
      return PutIfAbsentResult::kCreated;
    if (response.status == 412)
      return PutIfAbsentResult::kAlreadyExists;
    // S3 reports a simultaneous conditional write as 409 and explicitly
    // requires retrying the conditional request.
    if (response.status != 409)
      return PutIfAbsentResult::kError;
  }
  return PutIfAbsentResult::kError;
}

}  // namespace hare
