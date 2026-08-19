// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "WorkerBackend.h"

#include <WeaselUtility.h>

#include <algorithm>
#include <cstdint>

#include "CloudHttp.h"

namespace hare {

namespace {

constexpr const char* kConditionalPutCapability =
    "hare-worker/1 conditional-put";
constexpr const char* kConditionalProbeName =
    "keys/conditional-put-v1.bin";
constexpr const char* kConditionalProbeA =
    "hare-worker-conditional-put-v1-a";
constexpr const char* kConditionalProbeB =
    "hare-worker-conditional-put-v1-b";

bool AppendUtf8(uint32_t code_point, std::string* out) {
  if (code_point > 0x10ffff || (code_point >= 0xd800 && code_point <= 0xdfff)) {
    return false;
  }
  if (code_point <= 0x7f) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    out->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    out->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
  return true;
}

int HexDigit(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

bool ParseHexCodeUnit(const std::string& json,
                      size_t* pos,
                      uint32_t* code_unit) {
  if (*pos > json.size() || json.size() - *pos < 4)
    return false;
  *code_unit = 0;
  for (size_t i = 0; i < 4; ++i) {
    const int digit = HexDigit(json[*pos + i]);
    if (digit < 0)
      return false;
    *code_unit = (*code_unit << 4) | static_cast<uint32_t>(digit);
  }
  *pos += 4;
  return true;
}

bool ParseJsonString(const std::string& json, size_t* pos, std::string* value) {
  if (*pos >= json.size() || json[*pos] != '"')
    return false;
  ++*pos;
  value->clear();
  while (*pos < json.size()) {
    const unsigned char current = static_cast<unsigned char>(json[(*pos)++]);
    if (current == '"')
      return true;
    if (current < 0x20)
      return false;
    if (current != '\\') {
      value->push_back(static_cast<char>(current));
      continue;
    }
    if (*pos >= json.size())
      return false;

    const char escaped = json[(*pos)++];
    switch (escaped) {
      case '"':
      case '\\':
      case '/':
        value->push_back(escaped);
        break;
      case 'b':
        value->push_back('\b');
        break;
      case 'f':
        value->push_back('\f');
        break;
      case 'n':
        value->push_back('\n');
        break;
      case 'r':
        value->push_back('\r');
        break;
      case 't':
        value->push_back('\t');
        break;
      case 'u': {
        uint32_t code_point = 0;
        if (!ParseHexCodeUnit(json, pos, &code_point))
          return false;
        if (code_point >= 0xd800 && code_point <= 0xdbff) {
          if (json.size() - *pos < 2 || json[*pos] != '\\' ||
              json[*pos + 1] != 'u') {
            return false;
          }
          *pos += 2;
          uint32_t low_surrogate = 0;
          if (!ParseHexCodeUnit(json, pos, &low_surrogate) ||
              low_surrogate < 0xdc00 || low_surrogate > 0xdfff) {
            return false;
          }
          code_point = 0x10000 + ((code_point - 0xd800) << 10) +
                       (low_surrogate - 0xdc00);
        } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
          return false;
        }
        if (!AppendUtf8(code_point, value))
          return false;
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

void SkipJsonWhitespace(const std::string& json, size_t* pos) {
  while (*pos < json.size() && (json[*pos] == ' ' || json[*pos] == '\t' ||
                                json[*pos] == '\r' || json[*pos] == '\n')) {
    ++*pos;
  }
}

// The endpoint returns exactly one JSON array of strings, so only that shape is
// parsed here rather than carrying a general-purpose JSON dependency.
bool ParseStringArray(const std::string& json,
                      std::vector<std::string>* values) {
  size_t pos = 0;
  SkipJsonWhitespace(json, &pos);
  if (pos >= json.size() || json[pos++] != '[')
    return false;

  std::vector<std::string> parsed;
  SkipJsonWhitespace(json, &pos);
  if (pos < json.size() && json[pos] == ']') {
    ++pos;
  } else {
    for (;;) {
      std::string value;
      if (!ParseJsonString(json, &pos, &value))
        return false;
      parsed.push_back(std::move(value));
      SkipJsonWhitespace(json, &pos);
      if (pos >= json.size())
        return false;
      if (json[pos] == ']') {
        ++pos;
        break;
      }
      if (json[pos++] != ',')
        return false;
      SkipJsonWhitespace(json, &pos);
    }
  }

  SkipJsonWhitespace(json, &pos);
  if (pos != json.size())
    return false;
  *values = std::move(parsed);
  return true;
}

}  // namespace

WorkerBackend::WorkerBackend(WorkerSettings settings)
    : settings_(std::move(settings)) {
  if (!settings_.url.empty() && settings_.url.back() == '/')
    settings_.url.pop_back();
}

std::wstring WorkerBackend::Describe() const {
  return L"Worker " + u8tow(settings_.url);
}

std::map<std::wstring, std::wstring> WorkerBackend::AuthHeaders() const {
  return {{L"Authorization", L"Bearer " + u8tow(settings_.token)}};
}

bool WorkerBackend::SupportsConditionalCreate() const {
  const HttpResponse capability = HttpRequest(
      L"GET", u8tow(settings_.url + "/capabilities"), AuthHeaders(), "");
  if (capability.status != 200 ||
      capability.body != kConditionalPutCapability) {
    return false;
  }

  const std::wstring probe_url =
      u8tow(settings_.url + "/o/" + UriEncode(kConditionalProbeName, false));
  const auto read_probe = [this, &probe_url]() {
    auto headers = AuthHeaders();
    headers[L"Cache-Control"] = L"no-cache";
    return HttpRequest(L"GET", probe_url, headers, "");
  };

  HttpResponse initial = read_probe();
  if (initial.status == 404) {
    auto headers = AuthHeaders();
    headers[L"If-None-Match"] = L"*";
    const HttpResponse created =
        HttpRequest(L"PUT", probe_url, headers, kConditionalProbeA);
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
  const HttpResponse rejected =
      HttpRequest(L"PUT", probe_url, headers, replacement);
  if (rejected.status != 412)
    return false;

  const HttpResponse confirmed = read_probe();
  return confirmed.status == 200 && confirmed.body == initial.body;
}

bool WorkerBackend::List(std::vector<std::string>* names) {
  const HttpResponse response =
      HttpRequest(L"GET", u8tow(settings_.url + "/list"), AuthHeaders(), "");
  if (!response.ok())
    return false;
  if (!ParseStringArray(response.body, names))
    return false;
  names->erase(std::remove(names->begin(), names->end(), kConditionalProbeName),
               names->end());
  return true;
}

FetchResult WorkerBackend::Get(const std::string& name,
                               std::vector<uint8_t>* out) {
  const std::wstring url =
      u8tow(settings_.url + "/o/" + UriEncode(name, false));
  const HttpResponse response = HttpRequest(L"GET", url, AuthHeaders(), "");
  if (response.failure == HttpFailure::kPayloadTooLarge)
    return FetchResult::kPayloadTooLarge;
  if (response.status == 404)
    return FetchResult::kNotFound;
  if (!response.ok())
    return FetchResult::kError;
  out->assign(response.body.begin(), response.body.end());
  return FetchResult::kOk;
}

bool WorkerBackend::Put(const std::string& name,
                        const std::vector<uint8_t>& data) {
  const std::wstring url =
      u8tow(settings_.url + "/o/" + UriEncode(name, false));
  const HttpResponse response = HttpRequest(
      L"PUT", url, AuthHeaders(), std::string(data.begin(), data.end()));
  return response.ok();
}

PutIfAbsentResult WorkerBackend::PutIfAbsent(const std::string& name,
                                             const std::vector<uint8_t>& data) {
  if (!SupportsConditionalCreate())
    return PutIfAbsentResult::kError;

  const std::wstring url =
      u8tow(settings_.url + "/o/" + UriEncode(name, false));
  auto headers = AuthHeaders();
  headers[L"If-None-Match"] = L"*";
  const HttpResponse response =
      HttpRequest(L"PUT", url, headers, std::string(data.begin(), data.end()));
  if (response.status == 201)
    return PutIfAbsentResult::kCreated;
  if (response.status == 412)
    return PutIfAbsentResult::kAlreadyExists;
  return PutIfAbsentResult::kError;
}

}  // namespace hare
