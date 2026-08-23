#pragma once

#include <string>
#include <exception>

namespace WP_NAMESPACE {

class XmlException : public std::exception {
public:
  XmlException(std::string const& msg, std::string const& source)
      : mMessage(source == "" ? msg : source + ": " + msg) {
  }

  // std::exception(const char*) was removed in C++20, so the message is
  // stored and returned via what() (same behaviour as before).
  const char* what() const noexcept override {
    return mMessage.c_str();
  }

private:
  std::string mMessage;
};

class XmlPathException : public XmlException {
public:
  XmlPathException(std::string const& path, std::string const& source)
      : XmlException("Path does not exist: " + path, source) {
  }
};

}  // namespace WP_NAMESPACE