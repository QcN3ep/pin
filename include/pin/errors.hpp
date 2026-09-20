#pragma once

#include <stdexcept>

namespace pin {

class Error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class ResolveError : public Error {
  public:
    using Error::Error;
};

class ConfigError : public Error {
  public:
    using Error::Error;
};

} // namespace pin
