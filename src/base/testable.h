#pragma once

#include <base/aliases.h>
#include <base/attributes.h>

namespace dist_clang {
namespace base {

template <class Base, class Default = Base, class... Args>
// |Args| goes to the unique and default constructor of the |Default|.
class Testable {
 public:
  class Factory {
   public:
    virtual ~Factory() {}

    virtual UniquePtr<Base> Create(Args...) = 0;
  };

  class DefaultFactory : public Factory {
   public:
    virtual UniquePtr<Base> Create(Args... args) override {
      return UniquePtr<Base>(new Default(args...));
    }
  };

  static UniquePtr<Base> Create(Args... args) {
    if (!factory_()) {
      return typename dist_clang::UniquePtr<Base>();
    }
    return factory_()->Create(args...);
  }

  template <class F>
  static F* WEAK_PTR SetFactory() {
    factory_().reset(new F());
    return static_cast<F*>(factory_().get());
  }

 private:
  static UniquePtr<Factory>& factory_() {
    static UniquePtr<Factory> factory(new DefaultFactory());
    return factory;
  }
};

}  // namespace base
}  // namespace dist_clang
