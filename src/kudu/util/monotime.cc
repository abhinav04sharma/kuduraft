// Copyright (c) 2013, Cloudera, inc.

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <limits>

#include <glog/logging.h>

#include "kudu/gutil/stringprintf.h"
#include "kudu/util/monotime.h"

namespace kudu {

#define MAX_MONOTONIC_SECONDS \
  (((1ULL<<63) - 1ULL) /(int64_t)MonoTime::kNanosecondsPerSecond)


///
/// MonoDelta
///

MonoDelta MonoDelta::FromSeconds(double seconds) {
  int64_t delta = seconds * MonoTime::kNanosecondsPerSecond;
  return MonoDelta(delta);
}

MonoDelta MonoDelta::FromMilliseconds(int64_t ms) {
  return MonoDelta(ms * MonoTime::kNanosecondsPerMillisecond);
}

MonoDelta MonoDelta::FromMicroseconds(int64_t us) {
  return MonoDelta(us * MonoTime::kNanosecondsPerMicrosecond);
}

MonoDelta MonoDelta::FromNanoseconds(int64_t ns) {
  return MonoDelta(ns);
}

MonoDelta::MonoDelta()
  : nano_delta_(kUninitialized) {
}

bool MonoDelta::Initialized() const {
  return nano_delta_ != kUninitialized;
}

bool MonoDelta::LessThan(const MonoDelta &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return nano_delta_ < rhs.nano_delta_;
}

bool MonoDelta::MoreThan(const MonoDelta &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return nano_delta_ > rhs.nano_delta_;
}

bool MonoDelta::Equals(const MonoDelta &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return nano_delta_ == rhs.nano_delta_;
}

std::string MonoDelta::ToString() const {
  return StringPrintf("%.3fs", ToSeconds());
}

MonoDelta::MonoDelta(int64_t delta)
  : nano_delta_(delta) {
}

double MonoDelta::ToSeconds() const {
  DCHECK(Initialized());
  double d(nano_delta_);
  d /= MonoTime::kNanosecondsPerSecond;
  return d;
}

int64_t MonoDelta::ToNanoseconds() const {
  DCHECK(Initialized());
  return nano_delta_;
}

int64_t MonoDelta::ToMicroseconds() const {
  DCHECK(Initialized());
 return nano_delta_ / MonoTime::kNanosecondsPerMicrosecond;
}

int64_t MonoDelta::ToMilliseconds() const {
  DCHECK(Initialized());
  return nano_delta_ / MonoTime::kNanosecondsPerMillisecond;
}

void MonoDelta::ToTimeVal(struct timeval *tv) const {
  DCHECK(Initialized());
  tv->tv_sec = nano_delta_ / MonoTime::kNanosecondsPerSecond;
  tv->tv_usec = (nano_delta_ - (tv->tv_sec * MonoTime::kNanosecondsPerSecond))
      / MonoTime::kNanosecondsPerMicrosecond;

  // tv_usec must be between 0 and 999999.
  // There is little use for negative timevals so wrap it in PREDICT_FALSE.
  if (PREDICT_FALSE(tv->tv_usec < 0)) {
    --(tv->tv_sec);
    tv->tv_usec += 1000000;
  }

  // Catch positive corner case where we "round down" and could potentially set a timeout of 0.
  // Make it 1 usec.
  if (PREDICT_FALSE(tv->tv_usec == 0 && tv->tv_sec == 0 && nano_delta_ > 0)) {
    tv->tv_usec = 1;
  }

  // Catch negative corner case where we "round down" and could potentially set a timeout of 0.
  // Make it -1 usec (but normalized, so tv_usec is not negative).
  if (PREDICT_FALSE(tv->tv_usec == 0 && tv->tv_sec == 0 && nano_delta_ < 0)) {
    tv->tv_sec = -1;
    tv->tv_usec = 999999;
  }
}


void MonoDelta::NanosToTimeSpec(int64_t nanos, struct timespec* ts) {
  ts->tv_sec = nanos / MonoTime::kNanosecondsPerSecond;
  ts->tv_nsec = nanos - (ts->tv_sec * MonoTime::kNanosecondsPerSecond);

  // tv_nsec must be between 0 and 999999999.
  // There is little use for negative timespecs so wrap it in PREDICT_FALSE.
  if (PREDICT_FALSE(ts->tv_nsec < 0)) {
    --(ts->tv_sec);
    ts->tv_nsec += MonoTime::kNanosecondsPerSecond;
  }
}

void MonoDelta::ToTimeSpec(struct timespec *ts) const {
  DCHECK(Initialized());
  NanosToTimeSpec(nano_delta_, ts);
}

///
/// MonoTime
///

MonoTime MonoTime::Now(enum Granularity granularity) {
  struct timespec ts;
  CHECK_EQ(0, clock_gettime((granularity == COARSE) ?
                    CLOCK_MONOTONIC_COARSE : CLOCK_MONOTONIC, &ts));
  return MonoTime(ts);
}

MonoTime MonoTime::Max() {
  return MonoTime(std::numeric_limits<int64_t>::max());
}

const MonoTime& MonoTime::Earliest(const MonoTime& a, const MonoTime& b) {
  if (b.nanos_ < a.nanos_) {
    return b;
  }
  return a;
}

MonoTime::MonoTime()
  : nanos_(0) {
}

bool MonoTime::Initialized() const {
  return nanos_ != 0;
}

MonoDelta MonoTime::GetDeltaSince(const MonoTime &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  int64_t delta(nanos_);
  delta -= rhs.nanos_;
  return MonoDelta(delta);
}

void MonoTime::AddDelta(const MonoDelta &delta) {
  DCHECK(Initialized());
  nanos_ += delta.nano_delta_;
}

bool MonoTime::ComesBefore(const MonoTime &rhs) const {
  DCHECK(Initialized());
  DCHECK(rhs.Initialized());
  return nanos_ < rhs.nanos_;
}

std::string MonoTime::ToString() const {
  return StringPrintf("%.3fs", ToSeconds());
}

MonoTime::MonoTime(const struct timespec &ts) {
  // Monotonic time resets when the machine reboots.  The 64-bit limitation
  // means that we can't represent times larger than 292 years, which should be
  // adequate.
  CHECK_LT(ts.tv_sec, MAX_MONOTONIC_SECONDS);
  nanos_ = ts.tv_sec;
  nanos_ *= MonoTime::kNanosecondsPerSecond;
  nanos_ += ts.tv_nsec;
}

MonoTime::MonoTime(int64_t nanos)
  : nanos_(nanos) {
}

double MonoTime::ToSeconds() const {
  double d(nanos_);
  d /= MonoTime::kNanosecondsPerSecond;
  return d;
}

} // namespace kudu
