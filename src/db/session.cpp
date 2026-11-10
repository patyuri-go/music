#include "db/session.h"

#include <utility>

namespace db {

const char *errorKindName(ErrorKind kind) {
  switch (kind) {
  case ErrorKind::Constraint:
    return "constraint";
  case ErrorKind::NotNull:
    return "not_null";
  case ErrorKind::Busy:
    return "busy";
  case ErrorKind::Timeout:
    return "timeout";
  case ErrorKind::Unavailable:
    return "unavailable";
  case ErrorKind::Auth:
    return "auth";
  case ErrorKind::Unsupported:
    return "unsupported";
  case ErrorKind::Internal:
    return "internal";
  }
  return "internal";
}

Transaction::Transaction(Session &session, TransactionMode mode)
    : session_(session) {
  session_.begin(mode);
}

Transaction::~Transaction() {
  if (done_) {
    return;
  }
  try {
    session_.rollback();
  } catch (...) {
    // 析构不抛出。
  }
}

void Transaction::commit() {
  if (done_) {
    throw std::logic_error("db::Transaction: already finished");
  }
  session_.commit();
  done_ = true;
}

void Transaction::rollback() {
  if (done_) {
    throw std::logic_error("db::Transaction: already finished");
  }
  session_.rollback();
  done_ = true;
}

} // namespace db
