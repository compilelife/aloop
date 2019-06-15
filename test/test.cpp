#include <gtest/gtest.h>
#include "../src/aloop.h"
#include <memory>
#include <functional>
#include <future>
#include <vector>

using namespace std;
using namespace aloop;

using Msg = const shared_ptr<AMessage>&;
using Processor = function<void(Msg msg)>;
class MyHandler : public AHandler{
private:
    Processor mProcessor;
public:
    void setProcessor(Processor processor) {
        mProcessor = processor;
    }
protected:
    virtual void onMessageReceived(const std::shared_ptr<AMessage> &msg){
        if (mProcessor)
            mProcessor(msg);
    }
};

static shared_ptr<ALooper> looper;
static shared_ptr<MyHandler> handler;

class ALoopTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}

    virtual void SetUp() {
        looper = ALooper::create();
        handler.reset(new MyHandler);
        ASSERT_EQ(OK, looper->start());
        ASSERT_NE(INVALID_HANDLER_ID, looper->registerHandler(handler));
    }
    virtual void TearDown() {
        if (looper){
            looper->stop();
            looper->unregisterHandler(handler->id());
        }
    }
};

TEST_F(ALoopTest, post) {
    promise<void> barrier;
    auto barrierFuture = barrier.get_future();

    handler->setProcessor([&barrier](Msg msg){
        barrier.set_value();
    });

    AMessage::create(0, handler)->post();

    ASSERT_EQ(future_status::ready, barrierFuture.wait_for(chrono::milliseconds(100)));
}

TEST_F(ALoopTest, postAndAwaitResponse) {
    handler->setProcessor([](Msg msg){
        shared_ptr<AReplyToken> token;
        ASSERT_TRUE(msg->senderAwaitsResponse(&token));

        auto response = AMessage::create();
        response->setInt32("int32", 1);
        ASSERT_EQ(OK, response->postReply(token));
    });

    auto response = AMessage::createNull();
    ASSERT_EQ(OK, AMessage::create(0, handler)->postAndAwaitResponse(&response));
    int32_t value;
    ASSERT_TRUE(response->findInt32("int32", &value));
    ASSERT_EQ(1, value);
}

TEST_F(ALoopTest, postInsideHandler){
    vector<int> extra;
    promise<void> barrier;
    
    handler->setProcessor([&](Msg msg){
        int value = 0;
        ASSERT_TRUE(msg->findInt32("extra", &value));
        extra.push_back(value);
        if (extra.size() == 1){//把msg再次post，这次设置extra为2.
            msg->setInt32("extra", 2);
            ASSERT_EQ(OK, msg->post());
        }else if (extra.size() == 2){
            barrier.set_value();
        }
    });

    auto msg = AMessage::create(0, handler);
    msg->setInt32("extra", 1);
    ASSERT_EQ(OK, msg->post());

    auto barrierFuture = barrier.get_future();
    ASSERT_EQ(future_status::ready, barrierFuture.wait_for(chrono::milliseconds(100)));

    ASSERT_EQ(2, extra.size());
    ASSERT_EQ(1, extra[0]);
    ASSERT_EQ(2, extra[1]);
}

TEST_F(ALoopTest, postAfterFree){
    handler->setProcessor([](Msg msg){
        ASSERT_TRUE(false)<<"post after stop should not be handlered";
    });

    looper.reset();

    ASSERT_NE(OK, AMessage::create(0, handler)->post());
}

TEST_F(ALoopTest, postDealy){
    int64_t begin = looper->GetNowUs();
    promise<int64_t> duration;
    handler->setProcessor([&](Msg msg){
        duration.set_value(looper->GetNowUs() - begin);
    });

    const int64_t delay = 100*1000L;
    ASSERT_EQ(OK, AMessage::create(0, handler)->post(delay));

    auto durationFuture = duration.get_future();
    ASSERT_EQ(future_status::ready, durationFuture.wait_for(chrono::milliseconds(200)));
    auto diff = abs(durationFuture.get() - delay);

    ASSERT_TRUE(diff < 10*1000L);
}