// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "WebDavBackend.h"

#include <WeaselUtility.h>

#include <map>

#include "CloudHttp.h"

namespace hare {

namespace {

// Asking only for resourcetype keeps the response small; it is the single
// property needed to tell a collection from a file.
constexpr const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/></D:prop>"
    "</D:propfind>";

std::string PathOf(const std::string& url) {
  const size_t scheme = url.find("://");
  const size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
  const size_t slash = url.find('/', begin);
  if (slash == std::string::npos)
    return "/";
  std::string path = url.substr(slash);
  if (path.empty() || path.back() != '/')
    path.push_back('/');
  return path;
}

std::string PercentDecode(const std::string& value) {
  std::string out;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const std::string hex = value.substr(i + 1, 2);
      out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

// Namespace prefixes differ between servers, so responses are scanned for the
// local tag name rather than a fixed "D:" prefix.
std::vector<std::string> ExtractTag(const std::string& xml,
                                    const std::string& tag) {
  std::vector<std::string> values;
  size_t pos = 0;
  while (pos < xml.size()) {
    const size_t open = xml.find("<", pos);
    if (open == std::string::npos)
      break;
    const size_t close = xml.find('>', open);
    if (close == std::string::npos)
      break;
    std::string name = xml.substr(open + 1, close - open - 1);
    const size_t colon = name.find(':');
    if (colon != std::string::npos)
      name = name.substr(colon + 1);
    if (name == tag) {
      const size_t end = xml.find("<", close);
      if (end == std::string::npos)
        break;
      values.push_back(xml.substr(close + 1, end - close - 1));
      pos = end;
    } else {
      pos = close + 1;
    }
  }
  return values;
}

}  // namespace

WebDavBackend::WebDavBackend(WebDavSettings settings)
    : settings_(std::move(settings)) {
  if (!settings_.url.empty() && settings_.url.back() == '/')
    settings_.url.pop_back();
  root_path_ = PathOf(settings_.url);
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

  const std::wstring url =
      relative.empty() ? u8tow(settings_.url) : ResourceUrl(relative);
  const HttpResponse response =
      HttpRequest(L"PROPFIND", url, headers, kPropfindBody);
  if (!response.ok())
    return false;

  // Each <response> carries an <href>; the collection itself is listed first
  // and is skipped by comparing against the requested path.
  const std::string base =
      root_path_ + (relative.empty() ? "" : relative + "/");
  for (const std::string& href : ExtractTag(response.body, "href")) {
    std::string path = PercentDecode(href);
    const size_t scheme = path.find("://");
    if (scheme != std::string::npos) {
      const size_t slash = path.find('/', scheme + 3);
      path = slash == std::string::npos ? "/" : path.substr(slash);
    }
    const bool is_collection = !path.empty() && path.back() == '/';
    if (path.size() <= base.size())
      continue;
    if (path.compare(0, base.size(), base) != 0)
      continue;

    std::string entry = path.substr(base.size());
    if (is_collection)
      entry.pop_back();
    if (entry.empty())
      continue;

    const std::string full = relative.empty() ? entry : relative + "/" + entry;
    (is_collection ? collections : files)->push_back(full);
  }
  return true;
}

void WebDavBackend::EnsureCollection(const std::string& relative) const {
  if (relative.empty())
    return;
  HttpRequest(L"MKCOL", ResourceUrl(relative), AuthHeaders(), "");
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
  *names = std::move(files);
  return true;
}

FetchResult WebDavBackend::Get(const std::string& name,
                               std::vector<uint8_t>* out) {
  const HttpResponse response =
      HttpRequest(L"GET", ResourceUrl(name), AuthHeaders(), "");
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

}  // namespace hare
