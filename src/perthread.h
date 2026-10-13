/**
 * Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD)
 * License: MIT
 *
 * perthread.h: one lazily created copy of T per thread (search caches and
 * NNUE accumulators), plus a registry so a stopped engine can reset every
 * copy at once (new game, new network). The hot path is a plain
 * thread-local pointer.
 */
#pragma once
#include <algorithm>
#include <mutex>
#include <vector>

// Set once any PerThread registry is destroyed (static destruction at exit).
// On MinGW the main thread's thread_local destructors can run *after* the
// static objects are gone, so ~Holder must not touch the registry then.
inline bool perThreadRegistryDead = false;

template <class T>
class PerThread {
 public:
  ~PerThread() { perThreadRegistryDead = true; }
  T &get() {
    T *p = fast();
    return p ? *p : create();
  }
  // Call only while no search is running.
  template <class F>
  void forEach(F f) {
    std::lock_guard<std::mutex> g(mu_);
    for (T *p : all_) f(*p);
  }

 private:
  std::mutex mu_;
  std::vector<T *> all_;
  static T *&fast() {
    static thread_local T *p = nullptr;
    return p;
  }
  struct Holder {
    PerThread *owner = nullptr;
    T *p = nullptr;
    ~Holder() {
      if (!p) return;
      if (!perThreadRegistryDead) {
        std::lock_guard<std::mutex> g(owner->mu_);
        owner->all_.erase(std::remove(owner->all_.begin(), owner->all_.end(), p), owner->all_.end());
      }
      fast() = nullptr;
      delete p;
    }
  };
  T &create() {
    static thread_local Holder h;
    T *p = new T();
    h.owner = this;
    h.p = p;
    fast() = p;
    std::lock_guard<std::mutex> g(mu_);
    all_.push_back(p);
    return *p;
  }
};
