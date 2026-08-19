// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "WebDavBackend.h"

#include <WeaselUtility.h>

#include <algorithm>
#include <limits>
#include <map>
#include <shlwapi.h>
#include <urlmon.h>
#include <xmllite.h>

#include "CloudHttp.h"

namespace hare {

namespace {

// Asking only for resourcetype keeps the response small; it is the single
// property needed to tell a collection from a file.
constexpr const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/></D:prop>"
    "</D:propfind>";
constexpr const wchar_t* kDavNamespace = L"DAV:";
constexpr DWORD kUriFlags = Uri_CREATE_CANONICALIZE |
                            Uri_CREATE_NO_PRE_PROCESS_HTML_URI |
                            Uri_CREATE_NO_IE_SETTINGS;
constexpr DWORD kHttpPort = 80;
constexpr DWORD kHttpsPort = 443;
constexpr const char* kConditionalProbeName =
    "keys/conditional-put-v1.bin";
constexpr const char* kConditionalProbeA =
    "hare-webdav-conditional-put-v1-a";
constexpr const char* kConditionalProbeB =
    "hare-webdav-conditional-put-v1-b";

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

bool ParseHttpStatus(const std::wstring& value, unsigned* status) {
  const std::wstring line = TrimXmlWhitespace(value);
  if (line.compare(0, 5, L"HTTP/") != 0)
    return false;
  const size_t first_space = line.find(L' ', 5);
  const size_t version_dot = line.find(L'.', 5);
  if (first_space == std::wstring::npos || version_dot == std::wstring::npos ||
      version_dot == 5 || version_dot + 1 == first_space ||
      version_dot > first_space || first_space + 4 > line.size()) {
    return false;
  }
  for (size_t i = 5; i < first_space; ++i) {
    if (i == version_dot)
      continue;
    if (line[i] < L'0' || line[i] > L'9')
      return false;
  }
  if (line[first_space + 1] < L'0' || line[first_space + 1] > L'9' ||
      line[first_space + 2] < L'0' || line[first_space + 2] > L'9' ||
      line[first_space + 3] < L'0' || line[first_space + 3] > L'9' ||
      (first_space + 4 < line.size() && line[first_space + 4] != L' ')) {
    return false;
  }
  *status = static_cast<unsigned>((line[first_space + 1] - L'0') * 100 +
                                  (line[first_space + 2] - L'0') * 10 +
                                  line[first_space + 3] - L'0');
  return true;
}

struct DavResponse {
  std::wstring href;
  bool is_collection = false;
};

enum class DavElement {
  kOther,
  kRoot,
  kResponse,
  kHref,
  kPropstat,
  kProp,
  kStatus,
  kResourceType,
  kCollection,
};

struct DavResponseState {
  bool href_seen = false;
  std::wstring href;
  bool resource_type_ok = false;
  bool is_collection = false;
};

struct DavPropstatState {
  bool prop_seen = false;
  bool status_seen = false;
  bool status_ok = false;
  bool resource_type_seen = false;
  bool is_collection = false;
};

bool IsDavStructuralElement(const std::wstring& local_name) {
  return local_name == L"multistatus" || local_name == L"response" ||
         local_name == L"href" || local_name == L"propstat" ||
         local_name == L"prop" || local_name == L"status" ||
         local_name == L"resourcetype" || local_name == L"collection";
}

bool ParseDavMultistatus(const std::string& body,
                         std::vector<DavResponse>* responses) {
  XmlInput input;
  if (!OpenXml(body, &input))
    return false;

  std::vector<DavElement> stack;
  std::vector<DavResponse> parsed;
  DavResponseState current_response;
  DavPropstatState current_propstat;
  std::wstring text;
  bool root_closed = false;
  bool response_active = false;
  bool propstat_active = false;

  const auto close_element = [&]() -> bool {
    if (stack.empty())
      return false;
    const DavElement element = stack.back();
    switch (element) {
      case DavElement::kHref:
        current_response.href = text;
        break;
      case DavElement::kStatus: {
        unsigned status = 0;
        if (!ParseHttpStatus(text, &status))
          return false;
        current_propstat.status_ok = status >= 200 && status < 300;
        break;
      }
      case DavElement::kPropstat:
        if (!current_propstat.prop_seen || !current_propstat.status_seen)
          return false;
        if (current_propstat.status_ok && current_propstat.resource_type_seen) {
          if (current_response.resource_type_ok &&
              current_response.is_collection !=
                  current_propstat.is_collection) {
            return false;
          }
          current_response.resource_type_ok = true;
          current_response.is_collection = current_propstat.is_collection;
        }
        propstat_active = false;
        break;
      case DavElement::kResponse:
        if (!current_response.href_seen || current_response.href.empty() ||
            !current_response.resource_type_ok || propstat_active) {
          return false;
        }
        parsed.push_back(
            {current_response.href, current_response.is_collection});
        response_active = false;
        break;
      case DavElement::kRoot:
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
      if (!stack.empty() && (stack.back() == DavElement::kHref ||
                             stack.back() == DavElement::kStatus)) {
        return false;
      }

      std::wstring local_name;
      std::wstring namespace_uri;
      if (!CurrentElement(input.reader, &local_name, &namespace_uri))
        return false;

      DavElement element = DavElement::kOther;
      if (stack.empty()) {
        if (root_closed || local_name != L"multistatus" ||
            namespace_uri != kDavNamespace) {
          return false;
        }
        element = DavElement::kRoot;
      } else if (namespace_uri != kDavNamespace &&
                 IsDavStructuralElement(local_name)) {
        return false;
      } else if (namespace_uri == kDavNamespace &&
                 stack.back() == DavElement::kRoot &&
                 local_name == L"response") {
        if (response_active)
          return false;
        current_response = DavResponseState();
        response_active = true;
        element = DavElement::kResponse;
      } else if (namespace_uri == kDavNamespace && response_active &&
                 stack.back() == DavElement::kResponse &&
                 local_name == L"href") {
        if (current_response.href_seen)
          return false;
        current_response.href_seen = true;
        text.clear();
        element = DavElement::kHref;
      } else if (namespace_uri == kDavNamespace && response_active &&
                 stack.back() == DavElement::kResponse &&
                 local_name == L"propstat") {
        if (propstat_active)
          return false;
        current_propstat = DavPropstatState();
        propstat_active = true;
        element = DavElement::kPropstat;
      } else if (namespace_uri == kDavNamespace && propstat_active &&
                 stack.back() == DavElement::kPropstat &&
                 local_name == L"prop") {
        if (current_propstat.prop_seen)
          return false;
        current_propstat.prop_seen = true;
        element = DavElement::kProp;
      } else if (namespace_uri == kDavNamespace && propstat_active &&
                 stack.back() == DavElement::kPropstat &&
                 local_name == L"status") {
        if (current_propstat.status_seen)
          return false;
        current_propstat.status_seen = true;
        text.clear();
        element = DavElement::kStatus;
      } else if (namespace_uri == kDavNamespace && propstat_active &&
                 stack.back() == DavElement::kProp &&
                 local_name == L"resourcetype") {
        if (current_propstat.resource_type_seen)
          return false;
        current_propstat.resource_type_seen = true;
        element = DavElement::kResourceType;
      } else if (namespace_uri == kDavNamespace && propstat_active &&
                 stack.back() == DavElement::kResourceType &&
                 local_name == L"collection") {
        current_propstat.is_collection = true;
        element = DavElement::kCollection;
      } else if (namespace_uri == kDavNamespace &&
                 IsDavStructuralElement(local_name)) {
        return false;
      }

      stack.push_back(element);
      if (input.reader->IsEmptyElement() && !close_element())
        return false;
    } else if (node_type == XmlNodeType_EndElement) {
      if (!close_element())
        return false;
    } else if ((node_type == XmlNodeType_Text ||
                node_type == XmlNodeType_CDATA ||
                node_type == XmlNodeType_Whitespace) &&
               !stack.empty() &&
               (stack.back() == DavElement::kHref ||
                stack.back() == DavElement::kStatus)) {
      if (!AppendCurrentValue(input.reader, &text))
        return false;
    }
  }

  if (result != S_FALSE || !stack.empty() || !root_closed || response_active ||
      propstat_active) {
    return false;
  }
  *responses = std::move(parsed);
  return true;
}

int HexDigit(wchar_t value) {
  if (value >= L'0' && value <= L'9')
    return value - L'0';
  if (value >= L'a' && value <= L'f')
    return value - L'a' + 10;
  if (value >= L'A' && value <= L'F')
    return value - L'A' + 10;
  return -1;
}

bool HasValidPercentEscapes(const std::wstring& value) {
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != L'%')
      continue;
    if (i + 2 >= value.size() || HexDigit(value[i + 1]) < 0 ||
        HexDigit(value[i + 2]) < 0) {
      return false;
    }
    i += 2;
  }
  return true;
}

bool HasUriProperty(IUri* uri, Uri_PROPERTY property, bool* present) {
  BOOL value = FALSE;
  if (FAILED(uri->HasProperty(property, &value)))
    return false;
  *present = value != FALSE;
  return true;
}

bool IsHttpUri(IUri* uri) {
  CComBSTR scheme;
  CComBSTR host;
  if (uri->GetSchemeName(&scheme) != S_OK || uri->GetHost(&host) != S_OK ||
      !scheme || !host || host.Length() == 0) {
    return false;
  }
  return _wcsicmp(scheme, L"http") == 0 || _wcsicmp(scheme, L"https") == 0;
}

bool HasUnsupportedUriParts(IUri* uri) {
  bool present = false;
  return !HasUriProperty(uri, Uri_PROPERTY_USER_INFO, &present) || present ||
         !HasUriProperty(uri, Uri_PROPERTY_QUERY, &present) || present ||
         !HasUriProperty(uri, Uri_PROPERTY_FRAGMENT, &present) || present;
}

bool EffectivePort(IUri* uri, DWORD* port) {
  const HRESULT result = uri->GetPort(port);
  if (result == S_OK)
    return true;
  if (result != S_FALSE)
    return false;

  CComBSTR scheme;
  if (uri->GetSchemeName(&scheme) != S_OK || !scheme)
    return false;
  if (_wcsicmp(scheme, L"https") == 0) {
    *port = kHttpsPort;
    return true;
  }
  if (_wcsicmp(scheme, L"http") == 0) {
    *port = kHttpPort;
    return true;
  }
  return false;
}

bool SameOrigin(IUri* left, IUri* right) {
  CComBSTR left_scheme;
  CComBSTR right_scheme;
  CComBSTR left_host;
  CComBSTR right_host;
  DWORD left_port = 0;
  DWORD right_port = 0;
  return left->GetSchemeName(&left_scheme) == S_OK &&
         right->GetSchemeName(&right_scheme) == S_OK &&
         left->GetHost(&left_host) == S_OK &&
         right->GetHost(&right_host) == S_OK && left_scheme && right_scheme &&
         left_host && right_host && EffectivePort(left, &left_port) &&
         EffectivePort(right, &right_port) && left_port == right_port &&
         _wcsicmp(left_scheme, right_scheme) == 0 &&
         _wcsicmp(left_host, right_host) == 0;
}

bool CreateAbsoluteHttpUri(const std::wstring& value, CComPtr<IUri>& uri) {
  if (!HasValidPercentEscapes(value))
    return false;
  IUri* raw_uri = nullptr;
  const HRESULT result = ::CreateUri(value.c_str(), kUriFlags, 0, &raw_uri);
  if (FAILED(result))
    return false;
  uri.Attach(raw_uri);
  return IsHttpUri(uri) && !HasUnsupportedUriParts(uri);
}

bool ResolveHref(IUri* base,
                 const std::wstring& href,
                 CComPtr<IUri>& resolved) {
  if (href.empty() || !HasValidPercentEscapes(href))
    return false;

  IUri* raw_reference = nullptr;
  HRESULT result = ::CreateUri(
      href.c_str(), kUriFlags | Uri_CREATE_ALLOW_RELATIVE, 0, &raw_reference);
  if (FAILED(result))
    return false;
  CComPtr<IUri> reference;
  reference.Attach(raw_reference);

  IUri* raw_resolved = nullptr;
  result = CoInternetCombineIUri(base, reference, 0, &raw_resolved, 0);
  if (FAILED(result))
    return false;
  resolved.Attach(raw_resolved);
  return IsHttpUri(resolved) && !HasUnsupportedUriParts(resolved) &&
         SameOrigin(base, resolved);
}

bool DecodePathSegment(const std::wstring& encoded, std::string* decoded) {
  if (encoded.empty() || !HasValidPercentEscapes(encoded) ||
      encoded.size() >= MAXDWORD) {
    return false;
  }
  std::vector<wchar_t> buffer(encoded.size() + 1, L'\0');
  DWORD length = static_cast<DWORD>(buffer.size());
  std::wstring mutable_encoded = encoded;
  if (FAILED(UrlUnescapeW(mutable_encoded.data(), buffer.data(), &length,
                          URL_UNESCAPE_AS_UTF8))) {
    return false;
  }
  if (length >= buffer.size())
    return false;
  const std::wstring value(buffer.data(), length);
  if (value.empty() || value == L"." || value == L".." ||
      value.find(L'\0') != std::wstring::npos ||
      value.find(L'/') != std::wstring::npos ||
      value.find(L'\\') != std::wstring::npos) {
    return false;
  }
  *decoded = wtou8(value);
  return !decoded->empty();
}

bool UriPathComponents(IUri* uri, std::vector<std::string>* components) {
  CComBSTR path_value;
  if (uri->GetPath(&path_value) != S_OK || !path_value)
    return false;
  const std::wstring path(path_value, path_value.Length());
  if (path.empty() || path.front() != L'/')
    return false;

  std::vector<std::string> parsed;
  size_t begin = 1;
  while (begin < path.size()) {
    const size_t end = path.find(L'/', begin);
    if (end == begin) {
      if (end + 1 == path.size())
        break;
      return false;
    }
    const std::wstring encoded =
        path.substr(begin, end == std::wstring::npos ? end : end - begin);
    std::string decoded;
    if (!DecodePathSegment(encoded, &decoded))
      return false;
    parsed.push_back(std::move(decoded));
    if (end == std::wstring::npos)
      break;
    begin = end + 1;
  }
  *components = std::move(parsed);
  return true;
}

enum class RelativeHref { kInvalid, kSelf, kMember };

RelativeHref RelativeMember(IUri* base, IUri* href, std::string* member) {
  std::vector<std::string> base_components;
  std::vector<std::string> href_components;
  if (!UriPathComponents(base, &base_components) ||
      !UriPathComponents(href, &href_components) ||
      href_components.size() < base_components.size()) {
    return RelativeHref::kInvalid;
  }
  for (size_t i = 0; i < base_components.size(); ++i) {
    if (base_components[i] != href_components[i])
      return RelativeHref::kInvalid;
  }
  if (href_components.size() == base_components.size())
    return RelativeHref::kSelf;
  if (href_components.size() != base_components.size() + 1)
    return RelativeHref::kInvalid;
  *member = href_components.back();
  return RelativeHref::kMember;
}

}  // namespace

WebDavBackend::WebDavBackend(WebDavSettings settings)
    : settings_(std::move(settings)) {
  if (!settings_.url.empty() && settings_.url.back() == '/')
    settings_.url.pop_back();
}

std::wstring WebDavBackend::Describe() const {
  return L"WebDAV " + u8tow(settings_.url);
}

std::map<std::wstring, std::wstring> WebDavBackend::AuthHeaders() const {
  const std::string credentials =
      Base64(settings_.username + ":" + settings_.password);
  return {{L"Authorization", L"Basic " + u8tow(credentials)}};
}

std::wstring WebDavBackend::ResourceUrl(const std::string& name) const {
  return u8tow(settings_.url + "/" + UriEncode(name, false));
}

bool WebDavBackend::ListCollection(const std::string& relative,
                                   std::vector<std::string>* collections,
                                   std::vector<std::string>* files) const {
  auto headers = AuthHeaders();
  headers[L"Depth"] = L"1";
  headers[L"Content-Type"] = L"application/xml";

  // Collection request URIs end in a slash so relative DAV:href values resolve
  // against the collection rather than replacing its final path segment.
  const std::wstring url = ResourceUrl(relative.empty() ? "" : relative + "/");
  CComPtr<IUri> base_uri;
  if (!CreateAbsoluteHttpUri(url, base_uri))
    return false;

  const HttpResponse response =
      HttpRequest(L"PROPFIND", url, headers, kPropfindBody);
  if (response.status != 207)
    return false;

  std::vector<DavResponse> dav_responses;
  if (!ParseDavMultistatus(response.body, &dav_responses))
    return false;

  std::vector<std::string> parsed_collections;
  std::vector<std::string> parsed_files;
  for (const DavResponse& dav_response : dav_responses) {
    CComPtr<IUri> href_uri;
    if (!ResolveHref(base_uri, dav_response.href, href_uri))
      return false;
    std::string entry;
    const RelativeHref relation = RelativeMember(base_uri, href_uri, &entry);
    if (relation == RelativeHref::kInvalid)
      return false;
    if (relation == RelativeHref::kSelf)
      continue;

    const std::string full = relative.empty() ? entry : relative + "/" + entry;
    (dav_response.is_collection ? parsed_collections : parsed_files)
        .push_back(full);
  }
  collections->insert(collections->end(), parsed_collections.begin(),
                      parsed_collections.end());
  files->insert(files->end(), parsed_files.begin(), parsed_files.end());
  return true;
}

void WebDavBackend::EnsureCollection(const std::string& relative) const {
  if (relative.empty())
    return;
  HttpRequest(L"MKCOL", ResourceUrl(relative), AuthHeaders(), "");
}

bool WebDavBackend::SupportsConditionalCreate() const {
  EnsureCollection("keys");

  const auto read_probe = [this]() {
    auto headers = AuthHeaders();
    headers[L"Cache-Control"] = L"no-cache";
    return HttpRequest(L"GET", ResourceUrl(kConditionalProbeName), headers, "");
  };

  HttpResponse initial = read_probe();
  if (initial.status == 404) {
    auto headers = AuthHeaders();
    headers[L"If-None-Match"] = L"*";
    const HttpResponse created = HttpRequest(
        L"PUT", ResourceUrl(kConditionalProbeName), headers, kConditionalProbeA);
    if (created.status != 201 && created.status != 412)
      return false;
    initial = read_probe();
  }
  if (initial.status != 200)
    return false;

  const std::string replacement = initial.body == kConditionalProbeA
                                      ? kConditionalProbeB
                                      : kConditionalProbeA;
  auto headers = AuthHeaders();
  headers[L"If-None-Match"] = L"*";
  const HttpResponse rejected = HttpRequest(
      L"PUT", ResourceUrl(kConditionalProbeName), headers, replacement);
  if (rejected.status != 412)
    return false;

  const HttpResponse confirmed = read_probe();
  return confirmed.status == 200 && confirmed.body == initial.body;
}

bool WebDavBackend::List(std::vector<std::string>* names) {
  std::vector<std::string> collections;
  std::vector<std::string> files;
  if (!ListCollection("", &collections, &files))
    return false;

  // The layout is two levels deep: one collection per machine, plus keys/.
  // Recursing exactly one level avoids relying on Depth: infinity, which many
  // servers refuse.
  for (const std::string& collection : collections) {
    std::vector<std::string> nested_collections;
    if (!ListCollection(collection, &nested_collections, &files))
      return false;
  }
  files.erase(std::remove(files.begin(), files.end(), kConditionalProbeName),
              files.end());
  *names = std::move(files);
  return true;
}

FetchResult WebDavBackend::Get(const std::string& name,
                               std::vector<uint8_t>* out) {
  const HttpResponse response =
      HttpRequest(L"GET", ResourceUrl(name), AuthHeaders(), "");
  if (response.failure == HttpFailure::kPayloadTooLarge)
    return FetchResult::kPayloadTooLarge;
  if (response.status == 404)
    return FetchResult::kNotFound;
  if (!response.ok())
    return FetchResult::kError;
  out->assign(response.body.begin(), response.body.end());
  return FetchResult::kOk;
}

bool WebDavBackend::Put(const std::string& name,
                        const std::vector<uint8_t>& data) {
  const size_t slash = name.rfind('/');
  if (slash != std::string::npos)
    EnsureCollection(name.substr(0, slash));

  const HttpResponse response =
      HttpRequest(L"PUT", ResourceUrl(name), AuthHeaders(),
                  std::string(data.begin(), data.end()));
  return response.ok();
}

PutIfAbsentResult WebDavBackend::PutIfAbsent(const std::string& name,
                                             const std::vector<uint8_t>& data) {
  if (!SupportsConditionalCreate())
    return PutIfAbsentResult::kError;

  const size_t slash = name.rfind('/');
  if (slash != std::string::npos)
    EnsureCollection(name.substr(0, slash));

  auto headers = AuthHeaders();
  headers[L"If-None-Match"] = L"*";
  const HttpResponse response =
      HttpRequest(L"PUT", ResourceUrl(name), headers,
                  std::string(data.begin(), data.end()));
  if (response.status == 201)
    return PutIfAbsentResult::kCreated;
  if (response.status == 412)
    return PutIfAbsentResult::kAlreadyExists;
  return PutIfAbsentResult::kError;
}

}  // namespace hare
