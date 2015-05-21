#include <base/locked_list.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(LockedListTest, BasicUsage) {
  LockedList<int> list;
  const int expected = 1;
  LockedList<int>::Optional actual;

  list.Append(expected);
  ASSERT_TRUE(!!(actual = list.Pop()));
  EXPECT_EQ(expected, *actual);

  for (int i = 1; i < 10; ++i) {
    list.Append(i);
  }
  for (int i = 1; i < 10; ++i) {
    ASSERT_TRUE(!!(actual = list.Pop()));
    EXPECT_EQ(i, *actual);
  }
}

TEST(LockedListTest, MoveSemantics) {
  struct TestClass {
    TestClass() { ++counter(); }
    TestClass(const TestClass& t) { ++counter(); }
    TestClass(TestClass&& t) {
      // Do nothing.
    }
    TestClass& operator=(TestClass&& t) { return *this; }

    static int& counter() {
      static int counter_ = 0;
      return counter_;
    }
  };

  LockedList<TestClass> list;
  TestClass expected;

  list.Append(std::move(expected));
  auto&& actual = list.Pop();
  ASSERT_TRUE(!!actual);
  EXPECT_EQ(2, actual->counter());  // |expected| and |list.head_|

  actual->counter() = 0;
}

TEST(LockedListTest, UniquePtrFriendliness) {
  class Observer {
   public:
    Observer(bool& exist) : exist_(exist) { exist_ = true; }
    ~Observer() { exist_ = false; }

   private:
    bool& exist_;
  };

  bool observer_exists = true;
  LockedList<UniquePtr<Observer>> list;

  UniquePtr<Observer> ptr(new Observer(observer_exists));
  list.Append(std::move(ptr));
  EXPECT_FALSE(ptr);
  EXPECT_TRUE(observer_exists);
  auto&& actual = list.Pop();
  EXPECT_TRUE(observer_exists);
  ASSERT_TRUE(!!actual);
  ASSERT_TRUE(!!(*actual));
}

TEST(LockedListTest, DISABLED_BasicMultiThreadedUsage) {
  LockedList<int> list;
  // TODO: implement this test.
}

}  // namespace base
}  // namespace dist_clang
