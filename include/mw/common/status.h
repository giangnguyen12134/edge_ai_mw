// Copyright 2026 Genio Middleware Authors.
//
// Lightweight error type used instead of exceptions (Google style forbids
// exceptions). Every fallible function returns a Status.

#ifndef MW_COMMON_STATUS_H_
#define MW_COMMON_STATUS_H_

#include <string>

namespace mw {

class Status {
 public:
  enum Code {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kPermissionDenied,
    kResourceExhausted,
    kFailedPrecondition,
    kUnavailable,
    kTimeout,
    kUnimplemented,
    kInternal,
  };

  Status() : code_(kOk) {}
  Status(Code code, const std::string& message)
      : code_(code), message_(message) {}

  static Status Ok() { return Status(); }

  bool ok() const { return code_ == kOk; }
  Code code() const { return code_; }
  const std::string& message() const { return message_; }

  // Returns "OK" or "<CodeName>: <message>".
  std::string ToString() const;

 private:
  Code code_;
  std::string message_;
};

Status InvalidArgumentError(const std::string& message);
Status NotFoundError(const std::string& message);
Status AlreadyExistsError(const std::string& message);
Status PermissionDeniedError(const std::string& message);
Status ResourceExhaustedError(const std::string& message);
Status FailedPreconditionError(const std::string& message);
Status UnavailableError(const std::string& message);
Status TimeoutError(const std::string& message);
Status UnimplementedError(const std::string& message);
Status InternalError(const std::string& message);

// Maps an errno value to a Status. |context| describes the failed operation.
Status ErrnoStatus(const std::string& context, int error_number);

// Returns the stable textual name of |code| (e.g. "Unavailable").
const char* StatusCodeName(Status::Code code);

}  // namespace mw

// Evaluates |expr| (a Status) and returns it from the enclosing function if it
// is not OK.
#define MW_RETURN_IF_ERROR(expr)            \
  do {                                      \
    const ::mw::Status mw_status_ = (expr); \
    if (!mw_status_.ok()) {                 \
      return mw_status_;                    \
    }                                       \
  } while (0)

#endif  // MW_COMMON_STATUS_H_
