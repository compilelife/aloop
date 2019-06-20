#include <gtest/gtest.h>
#include "../src/aloop.h"

using namespace aloop;
using namespace std;

class EmptyHandler : public AHandler{
protected:
    virtual void onMessageReceived(const shared_ptr<AMessage> &msg){}
};

TEST(AHandler, InitState){
    shared_ptr<AHandler> handler(new EmptyHandler);

    ASSERT_EQ(INVALID_HANDLER_ID, handler->id());
    ASSERT_TRUE(handler->getLooper().expired());
}

TEST(AHandler, Register){
    auto looper = ALooper::create();
    shared_ptr<AHandler> handler(new EmptyHandler);

    auto id = looper->registerHandler(handler);
    ASSERT_EQ(id, handler->id());
    ASSERT_NE(INVALID_HANDLER_ID, id);
    ASSERT_EQ(looper, handler->getLooper().lock());
}

TEST(AHandler, RegisterTwice){
    auto looper = ALooper::create();
    shared_ptr<AHandler> handler(new EmptyHandler);

    ASSERT_NE(INVALID_HANDLER_ID, looper->registerHandler(handler));
    ASSERT_EQ(INVALID_HANDLER_ID, looper->registerHandler(handler));
}

TEST(AHandler, Unregister){
    auto looper = ALooper::create();
    shared_ptr<AHandler> handler(new EmptyHandler);

    auto id = looper->registerHandler(handler);
    ASSERT_NE(INVALID_HANDLER_ID, id);

    looper->unregisterHandler(id);
    ASSERT_EQ(INVALID_HANDLER_ID, handler->id());

    //unregister就可以重新regsiter了
    id = looper->registerHandler(handler);
    ASSERT_NE(INVALID_HANDLER_ID, id);
}

TEST(AHandler, AutoUnregister) {
    shared_ptr<AHandler> handler(new EmptyHandler);
    {
        auto looper = ALooper::create();
        auto id = looper->registerHandler(handler);
        ASSERT_NE(INVALID_HANDLER_ID, id);
    }
    ASSERT_EQ(INVALID_HANDLER_ID, handler->id());
}

TEST(AHandler, RegisterToTwoLooper){
    auto l1 = ALooper::create();
    auto l2 = ALooper::create();

    shared_ptr<AHandler> handler(new EmptyHandler);

    ASSERT_NE(INVALID_HANDLER_ID, l1->registerHandler(handler));
    ASSERT_EQ(INVALID_HANDLER_ID, l2->registerHandler(handler));
}

TEST(AHandler, GlobalId){
    auto l1 = ALooper::create();
    auto l2 = ALooper::create();

    shared_ptr<AHandler> h1(new EmptyHandler);
    shared_ptr<AHandler> h2(new EmptyHandler);

    auto id1 = l1->registerHandler(h1);
    auto id2 = l2->registerHandler(h2);

    ASSERT_NE(id1, id2);
}