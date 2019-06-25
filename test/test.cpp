#include <gtest/gtest.h>
#include "../src/aloop.h"
#include <memory>
#include <functional>
#include <future>
#include <vector>
#include <mutex>
#include <condition_variable>

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

class ALoopTest : public testing::Test {
public:
    static void SetUpTestCase() {}
    static void TearDownTestCase() {}

    virtual void SetUp() {
        mLooper = ALooper::create();
        mHandler.reset(new MyHandler);
        ASSERT_EQ(OK, mLooper->start());
        ASSERT_NE(INVALID_HANDLER_ID, mLooper->registerHandler(mHandler));
    }
    virtual void TearDown() {
        if (mLooper){
            mLooper->stop();
            mLooper->unregisterHandler(mHandler->id());
			mHandler.reset();
			mLooper.reset();
        }
    }
protected:
    shared_ptr<ALooper> mLooper;
    shared_ptr<MyHandler> mHandler;
};

TEST_F(ALoopTest, post) {
    promise<void> barrier;
    auto barrierFuture = barrier.get_future();

    mHandler->setProcessor([&barrier](Msg msg){
        barrier.set_value();
    });

    AMessage::create(0, mHandler)->post();

    ASSERT_EQ(future_status::ready, barrierFuture.wait_for(chrono::milliseconds(100)));
}

TEST_F(ALoopTest, postAndAwaitResponse) {
    mHandler->setProcessor([](Msg msg){
        shared_ptr<AReplyToken> token;
        ASSERT_TRUE(msg->senderAwaitsResponse(&token));

        auto response = AMessage::create();
        response->setInt32("int32", 1);
        ASSERT_EQ(OK, response->postReply(token));
    });

    auto response = AMessage::createNull();
    ASSERT_EQ(OK, AMessage::create(0, mHandler)->postAndAwaitResponse(&response));
    int32_t value;
    ASSERT_TRUE(response->findInt32("int32", &value));
    ASSERT_EQ(1, value);
}

TEST_F(ALoopTest, postInsideHandler){
    vector<int> extra;
    promise<void> barrier;
    
    mHandler->setProcessor([&](Msg msg){
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

    auto msg = AMessage::create(0, mHandler);
    msg->setInt32("extra", 1);
    ASSERT_EQ(OK, msg->post());

    auto barrierFuture = barrier.get_future();
    ASSERT_EQ(future_status::ready, barrierFuture.wait_for(chrono::milliseconds(100)));

    ASSERT_EQ(2, extra.size());
    ASSERT_EQ(1, extra[0]);
    ASSERT_EQ(2, extra[1]);
}

TEST_F(ALoopTest, postAfterFree){
    mHandler->setProcessor([](Msg msg){
        ASSERT_TRUE(false)<<"post after stop should not be handlered";
    });

    mLooper.reset();

    ASSERT_NE(OK, AMessage::create(0, mHandler)->post());
}

TEST_F(ALoopTest, Delay){
    int64_t begin = mLooper->GetNowUs();
    promise<int64_t> duration;
    mHandler->setProcessor([&](Msg msg){
        duration.set_value(mLooper->GetNowUs() - begin);
    });

    const int64_t delay = 100*1000L;
    ASSERT_EQ(OK, AMessage::create(0, mHandler)->post(delay));

    auto durationFuture = duration.get_future();
    ASSERT_EQ(future_status::ready, durationFuture.wait_for(chrono::milliseconds(200)));
    auto diff = abs(durationFuture.get() - delay);

    ASSERT_TRUE(diff < 10*1000L);
}

TEST(ALoop, stopInsideThread) {//该项测试不应该卡死或异常
    class Sync{
    public:
        int step{0};
        mutex lock;
        condition_variable cv;
    };

    class LooperHandler : public AHandler{
    public:
        shared_ptr<ALooper> looper;
        Sync* sync;

        LooperHandler(){
            looper = ALooper::create();
            looper->start();
        }
    protected:
        void onMessageReceived(const shared_ptr<AMessage> &msg){
            //当调用onMessageReceived的时候，looper线程需要持有handler的强引用.
            //当外面执行reset后，这里就是唯一引用了
            //onMessageReceived返回后就会调用alooper的析构
            unique_lock<mutex> l(sync->lock);
            sync->step = 1;
            sync->cv.notify_one();

            sync->cv.wait(l, [this]{return this->sync->step == 2;});
        }
    };

    shared_ptr<LooperHandler> lh(new LooperHandler);
    Sync sync;
    lh->looper->registerHandler(lh);
    lh->sync = &sync;

    AMessage::create(0, lh)->post();
    weak_ptr<LooperHandler> wp;

    {
        unique_lock<mutex> l(sync.lock);
        sync.cv.wait(l, [&sync]{return sync.step==1;});

        wp = lh;
        lh.reset();
        ASSERT_EQ(1, wp.use_count());

        sync.step = 2;
        sync.cv.notify_one();
    }

    std::this_thread::sleep_for(chrono::milliseconds(10));
    ASSERT_EQ(0, wp.use_count());
}