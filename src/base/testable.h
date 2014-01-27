#pragma once

#include "base/attributes.h"

#include <memory>

namespace dist_clang {
namespace base {

template <class T, class Default = T, class ... Args>
class Testable {
  public:
    class Factory {
      public:
        virtual ~Factory() {}

        virtual std::unique_ptr<T> Create(const Args& ...) = 0;
    };

    class DefaultFactory: public Factory {
      public:
        virtual std::unique_ptr<T> Create(const Args& ... args) override {
          return std::unique_ptr<T>(new Default(args ...));
        }
    };

    static std::unique_ptr<T> Create(const Args& ... args) {
      if (!factory_()) {
        return typename std::unique_ptr<T>();
      }
      return factory_()->Create(args ...);
    }

    template <class F>
    static F* WEAK_PTR SetFactory() {
      factory_().reset(new F());
      return static_cast<F*>(factory_().get());
    }

  private:
    static std::unique_ptr<Factory>& factory_() {
      static std::unique_ptr<Factory> factory(new DefaultFactory());
      return factory;
    }
};

}  // namespace base
}  // namespace dist_clang
