// Copyright 2026 Genio Middleware Authors.

#include "mw/common/status.h"

#include <errno.h>
#include <string.h>

namespace mw {

const char* StatusCodeName(Status::Code code) {
  switch (code) {
    case Status::kOk:
      return "Ok";
    case Status::kInvalidArgument:
      return "InvalidArgument";
    case Status::kNotFound:
      return "NotFound";
    case Status::kAlreadyExists:
      return "AlreadyExists";
    case Status::kPermissionDenied:
      return "PermissionDenied";
    case Status::kResourceExhausted:
      return "ResourceExhausted";
    case Status::kFailedPrecondition:
      return "FailedPrecondition";
    case Status::kUnavailable:
      return "Unavailable";
    case Status::kTimeout:
      return "Timeout";
    case Status::kUnimplemented:
      return "Unimplemented";
    case Status::kInternal:
      return "Internal";
  }
  return "Unknown";
}

std::string Status::ToString() const {
  if (ok()) {
    return "OK";
  }
  return std::string(StatusCodeName(code_)) + ": " + message_;
}

Status InvalidArgumentError(const std::string& message) {
  return Status(Status::kInvalidArgument, message);
}
Status NotFoundError(const std::string& message) {
  return Status(Status::kNotFound, message);
}
Status AlreadyExistsError(const std::string& message) {
  return Status(Status::kAlreadyExists, message);
}
Status PermissionDeniedError(const std::string& message) {
  return Status(Status::kPermissionDenied, message);
}
Status ResourceExhaustedError(const std::string& message) {
  return Status(Status::kResourceExhausted, message);
}
Status FailedPreconditionError(const std::string& message) {
  return Status(Status::kFailedPrecondition, message);
}
Status UnavailableError(const std::string& message) {
  return Status(Status::kUnavailable, message);
}
Status TimeoutError(const std::string& message) {
  return Status(Status::kTimeout, message);
}
Status UnimplementedError(const std::string& message) {
  return Status(Status::kUnimplemented, message);
}
Status InternalError(const std::string& message) {
  return Status(Status::kInternal, message);
}

Status ErrnoStatus(const std::string& context, int error_number) {
  Status::Code code = Status::kInternal;
  switch (error_number) {
    case ENOENT:
      code = Status::kNotFound;
      break;
    case EACCES:
    case EPERM:
      code = Status::kPermissionDenied;
      break;
    case EINVAL:
      code = Status::kInvalidArgument;
      break;
    case ENODEV:
    case ENXIO:
    case EBUSY:
    case ENOTCONN:
    case EPIPE:
    case ESHUTDOWN:
      code = Status::kUnavailable;
      break;
    case ENOMEM:
    case ENOSPC:
    case EMFILE:
      code = Status::kResourceExhausted;
      break;
    case ETIMEDOUT:
      code = Status::kTimeout;
      break;
    default:
      break;
  }
  return Status(code, context + ": " + strerror(error_number));
}

}  // namespace mw
