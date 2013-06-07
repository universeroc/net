// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_TOOLS_GDIG_FILE_NET_LOG_H_
#define NET_TOOLS_GDIG_FILE_NET_LOG_H_

#include <string>

#include "base/basictypes.h"
#include "base/synchronization/lock.h"
#include "base/time.h"
#include "net/base/net_log.h"

namespace net {

// FileNetLogObserver is a simple implementation of NetLog::ThreadSafeObserver
// that prints out all the events received into the stream passed
// to the constructor.
class FileNetLogObserver : public NetLog::ThreadSafeObserver {
 public:
  explicit FileNetLogObserver(FILE* destination);
  virtual ~FileNetLogObserver();

  // NetLog::ThreadSafeObserver implementation:
  virtual void OnAddEntry(const net::NetLog::Entry& entry) OVERRIDE;

 private:
  FILE* const destination_;
  base::Lock lock_;

  base::Time first_event_time_;
};

}  // namespace net

#endif  // NET_TOOLS_GDIG_FILE_NET_LOG_H_
