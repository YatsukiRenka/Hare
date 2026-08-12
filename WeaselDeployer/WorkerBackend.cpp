#include "stdafx.h"
#include "WorkerBackend.h"

#include <WeaselUtility.h>

#include "CloudHttp.h"

namespace hare {

namespace {

// The listing is a JSON array of plain strings. Anything richer would mean
// carrying a JSON parser for one endpoint.
std::vector<std::string> ParseStringArray(const std::string& json) {
  std::vector<std::string> values;
  size_t pos = 0;
  for (;;) {
    const size_t open = json.find('"', pos);
    if (open == std::string::npos)
      break;
    const size_t close = json.find('"', open + 1);
    if (close == std::string::npos)
      break;
    values.push_back(json.substr(open + 1, close - open - 1));
    pos = close + 1;
  }
  return values;
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

bool WorkerBackend::List(std::vector<std::string>* names) {
  const HttpResponse response =
      HttpRequest(L"GET", u8tow(settings_.url + "/list"), AuthHeaders(), "");
  if (!response.ok())
    return false;
  *names = ParseStringArray(response.body);
  return true;
}

bool WorkerBackend::Get(const std::string& name, std::vector<uint8_t>* out) {
  const std::wstring url =
      u8tow(settings_.url + "/o/" + UriEncode(name, false));
  const HttpResponse response = HttpRequest(L"GET", url, AuthHeaders(), "");
  if (!response.ok())
    return false;
  out->assign(response.body.begin(), response.body.end());
  return true;
}

bool WorkerBackend::Put(const std::string& name,
                        const std::vector<uint8_t>& data) {
  const std::wstring url =
      u8tow(settings_.url + "/o/" + UriEncode(name, false));
  const HttpResponse response =
      HttpRequest(L"PUT", url, AuthHeaders(),
                  std::string(data.begin(), data.end()));
  return response.ok();
}

}  // namespace hare
