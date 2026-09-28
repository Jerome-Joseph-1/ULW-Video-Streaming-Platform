#include "net/slab.hpp"

#include <gtest/gtest.h>

namespace {

struct Tracked {
    using Handle = net::Slab<Tracked>::Handle;
    Tracked(Handle h, int v, int* destroyed) : handle(h), value(v), destroyed_(destroyed) {}
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    ~Tracked() { ++*destroyed_; }
    Handle handle;
    int value;
    bool ready = false;

private:
    int* destroyed_;
};

TEST(Slab, ObjectKnowsItsOwnHandle) {
    int destroyed = 0;
    net::Slab<Tracked> slab(4);
    const auto h = slab.emplace(7, &destroyed);
    ASSERT_TRUE(h);
    const Tracked* t = slab.get(*h);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->handle, *h);
    EXPECT_EQ(t->value, 7);
}

TEST(Slab, RefusesBeyondCapacity) {
    int destroyed = 0;
    net::Slab<Tracked> slab(2);
    EXPECT_TRUE(slab.emplace(1, &destroyed));
    EXPECT_TRUE(slab.emplace(2, &destroyed));
    EXPECT_FALSE(slab.emplace(3, &destroyed));
}

TEST(Slab, RetiredObjectIsUnreachableButAliveUntilReaped) {
    int destroyed = 0;
    net::Slab<Tracked> slab(2);
    const auto h = *slab.emplace(1, &destroyed);
    slab.retire(h);
    EXPECT_EQ(slab.get(h), nullptr);
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(slab.reap([](Tracked&) { return false; }), 1U);
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(slab.reap([](Tracked&) { return true; }), 0U);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(slab.size(), 0U);
}

TEST(Slab, StaleHandleDoesNotResolveToTheSlotsNextOccupant) {
    int destroyed = 0;
    net::Slab<Tracked> slab(1);
    const auto first = *slab.emplace(1, &destroyed);
    slab.retire(first);
    slab.reap([](Tracked&) { return true; });
    const auto second = *slab.emplace(2, &destroyed);
    EXPECT_EQ(second.index, first.index);
    EXPECT_EQ(slab.get(first), nullptr);
    const Tracked* t = slab.get(second);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->value, 2);
}

TEST(Slab, ReapKeepsOnlyObjectsThatAreNotReady) {
    int destroyed = 0;
    net::Slab<Tracked> slab(3);
    const auto a = *slab.emplace(1, &destroyed);
    const auto b = *slab.emplace(2, &destroyed);
    Tracked* tb = slab.get(b);
    ASSERT_NE(tb, nullptr);
    tb->ready = true;
    slab.retire(a);
    slab.retire(b);
    EXPECT_EQ(slab.reap([](Tracked& t) { return t.ready; }), 1U);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(slab.size(), 1U);
}

} // namespace
