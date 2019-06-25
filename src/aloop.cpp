#include "aloop.h"
#include <stdio.h>
#include <stdarg.h>

#define CHECK assert

#define sp std::shared_ptr
#define wp std::weak_ptr
#define Autolock std::lock_guard<std::mutex>

namespace aloop{

using namespace std;

using PrintFunc = function<void(int level, const char* msg)>;
static PrintFunc g_doPrint = [](int level, const char* msg){
    const char* slevel[3] = {"INFO", "WARN", "ERR"};
    printf("[%s] %s\n", slevel[level], msg);
};
void setPrintFunc(PrintFunc doPrint) {
    g_doPrint = doPrint;
}

static void log(int level, const char* fmt, ...) {
    if (!g_doPrint)
        return;

    const int size = 1024;
    char buf[size] = {0};

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    g_doPrint(level, buf);
}

#define logi(fmt, args...) log(ALOOP_LOG_LEVEL_INFO, fmt, ##args)
#define logw(fmt, args...) log(ALOOP_LOG_LEVEL_WARN, fmt, ##args)
#define loge(fmt, args...) log(ALOOP_LOG_LEVEL_ERR, fmt, ##args)

class AReplyToken {
public:
    AReplyToken(const sp<ALooper> &looper)
        : mLooper(looper),
          mReplied(false) {
    }

private:
    friend class AMessage;
    friend class ALooper;
    wp<ALooper> mLooper;
    sp<AMessage> mReply;
    bool mReplied;

    sp<ALooper> getLooper() const {
        return mLooper.lock();
    }
    // if reply is not set, returns false; otherwise, it retrieves the reply and returns true
    bool retrieveReply(sp<AMessage> *reply) {
        if (mReplied) {
            *reply = mReply;//TODO: invalid reply here
            mReply.reset();
        }
        return mReplied;
    }
    // sets the reply for this token. returns OK or error
    status_t setReply(const sp<AMessage> &reply) {
        if (mReplied) {
            loge("trying to post a duplicate reply");
            return -EBUSY;
        }
        CHECK(mReply == NULL);
        mReply = reply;
        mReplied = true;
        return OK;
    }
};


AHandler::AHandler()
    : mID(INVALID_HANDLER_ID){
}

handler_id AHandler::id() const {
    return mID;
}

wp<ALooper> AHandler::getLooper() const {
    return mLooper;
}

void AHandler::setID(handler_id id, wp<ALooper> looper) {
    mID = id;
    mLooper = looper;
}

void AHandler::deliverMessage(const sp<AMessage> &msg) {
    onMessageReceived(msg);
}

const handler_id INVALID_HANDLER_ID = 0;

//统一不同looper的register/unregister到一个地方，可以避免多线程情况把一个handler注册到多个looper中
class ALooperRoster {
public:
    struct HandlerInfo {
        ALooper* mLooper;
        wp<AHandler> mHandler;
    };

    map<handler_id, HandlerInfo> mHandlers;
    mutex mLock;
    handler_id mNextHandlerID{1};

    handler_id registerHandler(
            const sp<ALooper> looper, const sp<AHandler> &handler){
        Autolock autoLock(mLock);

        if (handler->id() != INVALID_HANDLER_ID) {
            loge("A handler must only be registered once.");
            return INVALID_HANDLER_ID;
        }

        handler_id handlerID = INVALID_HANDLER_ID;
        do{
            handlerID = mNextHandlerID++;
        } while(handlerID == INVALID_HANDLER_ID);
        
        mHandlers[handlerID] = HandlerInfo{looper.get(), handler};
        handler->setID(handlerID, looper);

        return handlerID;
    }

    void unregisterHandler(handler_id handlerID){
        Autolock autoLock(mLock);

        sp<AHandler> handler = mHandlers[handlerID].mHandler.lock();

        if (handler != NULL) {
            handler->setID(0, wp<ALooper>());
        }

        mHandlers.erase(handlerID);
    }

    void unregisterHandlers(ALooper* looper){
        Autolock autoLock(mLock);

        auto it = mHandlers.begin();
        while (it != mHandlers.end()){
            if (it->second.mLooper == looper){
                sp<AHandler> handler =it->second.mHandler.lock();

                if (handler != NULL) {
                    handler->setID(0, wp<ALooper>());
                }
                
                mHandlers.erase(it++);
            }else{
                ++it;
            }
        }
        
    }
};

static ALooperRoster gLooperRoster;

ALooper::ALooper() 
    : mRun(false), mRunningLocally(false){
}

sp<ALooper> ALooper::create() {
    return sp<ALooper>(new ALooper);
}

/**
 * @brief 设置looper名称，需要在start前调用
 * @param name 
 */
void ALooper::setName(const char *name) {
    mName = name;
}

/**
 * @breif 将一个handler注册到该looper上执行。一个handler只能注册一次，如果要注册到其他looper上，需要先unregister
 * @param handler 要注册的handler
 * @return 注册成功后得到的handler。等同于handler->id()。如果注册失败，则返回INVALID_HANDLER_ID
 */
handler_id ALooper::registerHandler(const sp<AHandler> &handler) {
    return gLooperRoster.registerHandler(shared_from_this(), handler);
}

void ALooper::unregisterHandler(handler_id handlerID) {
    return gLooperRoster.unregisterHandler(handlerID);
}

status_t ALooper::start(bool runOnCallingThread) {
    if (runOnCallingThread){
        {
            Autolock l(mLock);

            if (mRun)
                return INVALID_OPERATION;

            mRunningLocally = true;
            mRun = true;
        }

        logi("start on calling thread");
        do{
        } while (mRun && loop());

        return OK;
    }

    Autolock l(mLock);

    if (mRun)
        return INVALID_OPERATION;

    mRun = true;
    logi("start on new thread");
    mThread = thread([](ALooper* looper){
        do{
        }while(looper->mRun && looper->loop());
    }, this);
    return OK;
}

status_t ALooper::stop() {
    bool runningLocally;
    thread thd;

    {
        Autolock l(mLock);
        if (!mRun)
            return INVALID_OPERATION;

        runningLocally = mRunningLocally;
        mThread.swap(thd);
        mRunningLocally = false;
        mRun = false;
    }

    mQueueChangedCondition.notify_one();
    {
        Autolock l(mRepliesLock);
        mRepliesCondition.notify_all();
    }

    if (!runningLocally && thd.joinable()) {
        if (thd.get_id() == this_thread::get_id()){
            logw("stop in looper thread, make detach");
            thd.detach();
        }else{
            thd.join();
        }
    }

    return OK;
}

int64_t ALooper::GetNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char *ALooper::getName() const {
    return mName.c_str();
}

ALooper::~ALooper() {
    stop();
    gLooperRoster.unregisterHandlers(this);
}

void ALooper::post(const sp<AMessage> &msg, int64_t delayUs) {
    Autolock l(mLock);

    int64_t whenUs;
    if (delayUs > 0) {
        whenUs = GetNowUs() + delayUs;
    } else {
        whenUs = GetNowUs();
    }

    std::list<Event>::iterator it = mEventQueue.begin();
    while (it != mEventQueue.end() && (*it).mWhenUs <= whenUs) {
        ++it;
    }

    Event event;
    event.mWhenUs = whenUs;
    event.mMessage = msg;

    if (it == mEventQueue.begin()) {
        mQueueChangedCondition.notify_one();
    }

    mEventQueue.insert(it, event);//在it前插入event，所以如果有时间相同的事件，那么就插入到时间相同的事件后
}

// creates a reply token to be used with this looper
sp<AReplyToken> ALooper::createReplyToken() {
    return std::shared_ptr<AReplyToken>(new AReplyToken(shared_from_this()));
}

// waits for a response for the reply token.  If status is OK, the response
// is stored into the supplied variable.  Otherwise, it is unchanged.
status_t ALooper::awaitResponse(const sp<AReplyToken> &replyToken, sp<AMessage> *response) {
    // return status in case we want to handle an interrupted wait
    std::unique_lock<std::mutex> l(mRepliesLock);
    CHECK(replyToken != NULL);
    while (!replyToken->retrieveReply(response)) {
        {
            Autolock l(mLock);
            if (!mRun) {
                return -ENOENT;
            }
        }
        mRepliesCondition.wait(l);
    }
    return OK;
}
// posts a reply for a reply token.  If the reply could be successfully posted,
// it returns OK. Otherwise, it returns an error value.
status_t ALooper::postReply(const sp<AReplyToken> &replyToken, const sp<AMessage> &reply) {
    Autolock l(mRepliesLock);
    status_t err = replyToken->setReply(reply);
    if (err == OK) {
        mRepliesCondition.notify_all();//mRepliesCondition不区分reply token，也就是唤醒所有在等待的reply token
    }
    return err;
}

// END --- methods used only by AMessage

bool ALooper::loop() {
    Event event;

    {
        std::unique_lock<std::mutex> l(mLock);
        if (!mRun) {
            return false;
        }
        if (mEventQueue.empty()) {
            mQueueChangedCondition.wait(l);
            return true;
        }
        int64_t whenUs = (*mEventQueue.begin()).mWhenUs;
        int64_t nowUs = GetNowUs();

        if (whenUs > nowUs) {
            using clock = std::chrono::steady_clock;
            clock::duration d(whenUs*1000ll);
            std::chrono::time_point<clock> targetTime(d);

            mQueueChangedCondition.wait_until(l, targetTime);

            return true;
        }

        event = *mEventQueue.begin();
        mEventQueue.erase(mEventQueue.begin());
    }

    event.mMessage->deliver();

    // NOTE: It's important to note that at this point our "ALooper" object
    // may no longer exist (its final reference may have gone away while
    // delivering the message). We have made sure, however, that loop()
    // won't be called again.

    return true;
}


AMessage::AMessage()
    : mWhat(0),
    mTarget(INVALID_HANDLER_ID),
    mNumItems(0) {
}

AMessage::AMessage(uint32_t what, const sp<AHandler> &handler)
    : mWhat(what),
    mNumItems(0) {
    setTarget(handler);
}

sp<AMessage> AMessage::create() {
    return sp<AMessage>(new AMessage);
}

sp<AMessage> AMessage::createNull() {
    return sp<AMessage>();
}

sp<AMessage> AMessage::create(uint32_t what, const sp<AHandler> &handler) {
    return sp<AMessage>(new AMessage(what, handler));
}

void AMessage::setWhat(uint32_t what) {
    mWhat = what;
}

uint32_t AMessage::what() const {
    return mWhat;
}

void AMessage::setTarget(const sp<AHandler> &handler) {
    if (handler == NULL) {
        mTarget = INVALID_HANDLER_ID;
        mHandler.reset();
        mLooper.reset();
    } else {
        mTarget = handler->id();
        mHandler = wp<AHandler>(handler);
        mLooper = handler->getLooper();
    }
}

void AMessage::clear() {
    for (size_t i = 0; i < mNumItems; ++i) {
        Item *item = &mItems[i];
        delete[] item->mName;
        item->mName = NULL;
        freeItemValue(item);
    }
    mNumItems = 0;
}

//setXXX(name, value), findXXX(name, &value)
#define BASIC_TYPE(NAME,FIELDNAME,TYPENAME)                             \
void AMessage::set##NAME(const char *name, TYPENAME value) {            \
    Item *item = allocateItem(name);                                    \
                                                                        \
    item->mType = kType##NAME;                                          \
    item->u.FIELDNAME = value;                                          \
}                                                                       \
                                                                        \
bool AMessage::find##NAME(const char *name, TYPENAME *value) const {    \
    const Item *item = findItem(name, kType##NAME);                     \
    if (item) {                                                         \
        *value = item->u.FIELDNAME;                                     \
        return true;                                                    \
    }                                                                   \
    return false;                                                       \
}

BASIC_TYPE(Int32,int32Value,int32_t)
BASIC_TYPE(Int64,int64Value,int64_t)
BASIC_TYPE(Size,sizeValue,size_t)
BASIC_TYPE(Float,floatValue,float)
BASIC_TYPE(Double,doubleValue,double)
BASIC_TYPE(Pointer,ptrValue,void *)

void AMessage::setObject(const char* name, const sp<void>& value) {
    auto ref = new RefHolder(value);
    Item *item = allocateItem(name);
    item->mType = kTypeObject;      
    item->u.refValue = ref;
}

void AMessage::setString(const char *name, const char *s, ssize_t len) {
    auto str = new std::string(s, len <= 0 ? strlen(s) : len);
    Item *item = allocateItem(name);
    item->mType = kTypeString;
    item->u.stringValue = str;
}

void AMessage::setString(const char *name, const std::string &s){
    auto str = new std::string(s);
    Item *item = allocateItem(name);
    item->mType = kTypeString;
    item->u.stringValue = str;
}

bool AMessage::findString(const char *name, std::string *value) const {
    const Item *item = findItem(name, kTypeString);
    if (item) {                                    
        *value = *(item->u.stringValue);
        return true;                               
    }                                              
    return false;
}
    
bool AMessage::contains(const char *name) const {
    size_t i = findItemIndex(name, strlen(name));
    return i < mNumItems;
}

status_t AMessage::post(int64_t delayUs){
    sp<ALooper> looper = mLooper.lock();
    if (!looper) {
        logw("failed to post message as target looper for handler %d is gone.", mTarget);
        return NOT_FOUND;
    }

    looper->post(shared_from_this(), delayUs);
    return OK;
}

// Posts the message to its target and waits for a response (or error)
// before returning.
status_t AMessage::postAndAwaitResponse(sp<AMessage> *response){
    sp<ALooper> looper = mLooper.lock();
    if (!looper) {
        logw("failed to post message as target looper for handler %d is gone.", mTarget);
        return NOT_FOUND;
    }

    sp<AReplyToken> token = looper->createReplyToken();
    if (!token) {
        loge("failed to create reply token");
        return NOT_FOUND;
    }
    setObject("replyID", token);

    looper->post(shared_from_this(), 0 /* delayUs */);
    return looper->awaitResponse(token, response);
}

// If this returns true, the sender of this message is synchronously
// awaiting a response and the reply token is consumed from the message
// and stored into replyID. The reply token must be used to send the response
// using "postReply" below.
bool AMessage::senderAwaitsResponse(sp<AReplyToken> *replyToken){
    bool found = findObject("replyID", replyToken);

    if (!found) {
        return false;
    }

    return (*replyToken)!=nullptr;
}

// Posts the message as a response to a reply token.  A reply token can
// only be used once. Returns OK if the response could be posted; otherwise,
// an error.
status_t AMessage::postReply(const sp<AReplyToken> &replyToken){
    if (replyToken == nullptr) {
        logw("failed to post reply to a NULL token");
        return NOT_FOUND;
    }
    sp<ALooper> looper = replyToken->getLooper();
    if (looper == NULL) {
        logw("failed to post reply as target looper is gone.");
        return NOT_FOUND;
    }
    return looper->postReply(replyToken, shared_from_this());
}

// Performs a deep-copy of "this", contained messages are in turn "dup'ed".
// Warning: RefBase items, i.e. "objects" are _not_ copied but only have
// their refcount incremented.
sp<AMessage> AMessage::dup() const {
    auto msg = AMessage::create(mWhat, mHandler.lock());
    msg->mNumItems = mNumItems;

    for (size_t i = 0; i < mNumItems; ++i) {
        const Item *from = &mItems[i];
        Item *to = &msg->mItems[i];

        to->setName(from->mName, from->mNameLength);
        to->mType = from->mType;

        switch (from->mType) {
            case kTypeString:{
                to->u.stringValue =
                    new std::string(*from->u.stringValue);
                break;
            }
            default:{
                to->u = from->u;
                break;
            }
        }
    }

    return msg;
}

AMessage::~AMessage() {
    clear();
}

// assumes item's name was uninitialized or NULL
void AMessage::Item::setName(const char *name, size_t len) {
    mNameLength = len;
    mName = new char[len + 1];
    memcpy((void*)mName, name, len + 1);
}

AMessage::Item *AMessage::allocateItem(const char *name){
    size_t len = strlen(name);
    size_t i = findItemIndex(name, len);
    Item *item;

    if (i < mNumItems) {
        item = &mItems[i];
        freeItemValue(item);
    } else {
        CHECK(mNumItems < kMaxNumItems);
        i = mNumItems++;
        item = &mItems[i];
        item->setName(name, len);
    }

    return item;
}

void AMessage::freeItemValue(Item *item){
    switch (item->mType) {
        case kTypeString:{
            delete item->u.stringValue;
            break;
        }

        case kTypeObject:{
            delete item->u.refValue;
            break;
        }

        default:
            break;
    }
}

const AMessage::Item *AMessage::findItem(const char *name, Type type) const {
    size_t i = findItemIndex(name, strlen(name));

    if (i < mNumItems) {
        const Item *item = &mItems[i];
        return item->mType == type ? item : NULL;
    }

    return NULL;
}

size_t AMessage::findItemIndex(const char *name, size_t len) const {
    size_t i = 0;
    for (; i < mNumItems; i++) {
        if (len != mItems[i].mNameLength) {
            continue;
        }
        if (!memcmp(mItems[i].mName, name, len)) {
            break;
        }
    }
    return i;
}

void AMessage::deliver() {
    sp<AHandler> handler = mHandler.lock();
    if (handler == NULL) {
        logw("failed to deliver message as target handler %d is gone.", mTarget);
        return;
    }

    handler->deliverMessage(shared_from_this());
}

size_t AMessage::countEntries() const {
    return mNumItems;
}

const char *AMessage::getEntryNameAt(size_t index, Type *type) const {
    if (index >= mNumItems) {
        *type = kTypeInt32;

        return NULL;
    }

    *type = mItems[index].mType;

    return mItems[index].mName;
}

}