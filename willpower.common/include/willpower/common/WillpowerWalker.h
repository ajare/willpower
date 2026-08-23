#pragma once

#include <string>
#include <vector>
#include <cassert>

#include "willpower/common/Platform.h"

#if WP_PLATFORM == WP_PLATFORM_WINDOWS

#include "willpower/common/StackWalker.h"
#include "willpower/common/Logger.h"

#ifdef WP_USE_ASSERT_TRACE
#ifdef _DEBUG
#define ASSERT_TRACE(expr)                                        \
  if (!(expr)) {                                                  \
    StackWalkerInstance::getInstance()->logStackTraceFormatted(); \
    assert(expr);                                                 \
  }
#else
#define ASSERT_TRACE(expr) (void)0
#endif
#else
#define ASSERT_TRACE(expr) assert(expr)
#endif

namespace WP_NAMESPACE {

class WP_COMMON_API WillpowerWalker : public StackWalker {
  Logger* mLogger;

  bool mNewTrace;

protected:
  void OnOutput(LPCSTR szText);

public:
  WillpowerWalker(std::string const& logfile);

  ~WillpowerWalker();

  void logStackTraceFormatted();
};

class WP_COMMON_API StackWalkerInstance {
  static WillpowerWalker* mInstance;

protected:
  StackWalkerInstance();

public:
  static WillpowerWalker* getInstance();

  static bool hasInstance();

  static void deleteInstance();
};

}  // namespace WP_NAMESPACE

#else
// Non-Windows fallback: ASSERT_TRACE degrades to a plain assert() and
// WillpowerWalker dumps a raw backtrace() to stderr. The public API mirrors
// the Windows version so callers are unchanged.

#define ASSERT_TRACE(expr) assert(expr)

namespace WP_NAMESPACE {

class WP_COMMON_API WillpowerWalker {
public:
  WillpowerWalker(std::string const& logfile);

  ~WillpowerWalker();

  void logStackTraceFormatted();
};

class WP_COMMON_API StackWalkerInstance {
  static WillpowerWalker* mInstance;

protected:
  StackWalkerInstance();

public:
  static WillpowerWalker* getInstance();

  static bool hasInstance();

  static void deleteInstance();
};

}  // namespace WP_NAMESPACE

#endif
