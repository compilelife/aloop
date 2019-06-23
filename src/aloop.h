#ifndef A_LOOP_H
#define A_LOOP_H

#include <stdint.h>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <list>
#include <atomic>
#include <map>
#include <thread>
#include <chrono>
#include <cassert>
#include <string.h>
#include <functional>

#define ALOOP_LOG_LEVEL_INFO 0
#define ALOOP_LOG_LEVEL_WARN 1
#define ALOOP_LOG_LEVEL_ERR 2

namespace aloop
{
//通过声明但不实现拷贝构造和=，可以在被调用时引发crash，从而起到disalloc evil constructors的作用
#define DISALLOW_EVIL_CONSTRUCTORS(name) \
    name(const name &); \
    name &operator=(const name &)

typedef int32_t     status_t;
enum{
    OK                = 0,    // Everything's swell.
    INVALID_OPERATION   = -ENOSYS,
    NOT_FOUND      = -ENOENT,
    NO_MEM         = -ENOMEM,
    BUSY           = -EBUSY
};
typedef int32_t handler_id;
extern const handler_id INVALID_HANDLER_ID;

class AMessage;
class AReplyToken;
class AHandler;
class ALooper;

void setPrintFunc(std::function<void(int level, const char* msg)> doPrint);


/**
 * @brief 消息处理类。需要用户继承，并实现onMessageReceived方法，根据消息类型进行处理
 */
class AHandler{
public:
    AHandler();

    handler_id id() const;

    std::weak_ptr<ALooper> getLooper() const;

protected:
    virtual void onMessageReceived(const std::shared_ptr<AMessage> &msg) = 0;

private:
    friend class AMessage;      // deliverMessage()
    friend class ALooperRoster; // setID()

    std::atomic<handler_id> mID;
    std::weak_ptr<ALooper> mLooper;

    void setID(handler_id id, std::weak_ptr<ALooper> looper);

    void deliverMessage(const std::shared_ptr<AMessage> &msg);

    DISALLOW_EVIL_CONSTRUCTORS(AHandler);
};

/**
 * @brief 线程循环与消息分派类。
 * 
 * 内部默认会创建一个线程来执行消息派发。当然，也可以通过start(false)在自定义线程中运行
 */
class ALooper : public std::enable_shared_from_this<ALooper> {
private:
    ALooper();
public:
    static std::shared_ptr<ALooper> create();

    /**
     * @brief 设置looper名称，需要在start前调用
     *          该方法并不会设置线程的名字（为了更好地跨平台）
     * @param name 
     */
    void setName(const char *name);

    /**
     * @brief 将一个handler注册到该looper上执行。一个handler只能注册一次，如果要注册到其他looper上，需要先unregister
     * @param handler 要注册的handler
     * @return 注册成功后得到的handler。等同于handler->id()。如果注册失败，则返回INVALID_HANDLER_ID
     */
    handler_id registerHandler(const std::shared_ptr<AHandler> &handler);

    /**
     * @brief 注销一个handler
     * @param handlerID 要注销的handler的id
     */
    void unregisterHandler(handler_id handlerID);

    /**
     * @brief 启动looper，开始等待消息
     * @param runOnCallingThread true,在调用线程上执行loop循环; false,使用内部新建的线程
     * @return OK,执行成功；INVALID_OPERATION,重复启动
     */
    status_t start(bool runOnCallingThread = false);

    /**
     * @brief 停止loop循环。
     *      需要当前在处理消息执行完成才会停止。
     *      未处理的消息会滞留在消息队列中。
     * @return OK，停止成功
     */
    status_t stop();

    static int64_t GetNowUs();

    /**
     * @return looper名字
     */
    const char *getName() const;

// protected: //不强制要求继承ALooper
    virtual ~ALooper();

private:
    friend class AMessage;       // post()
    bool mRun;

    struct Event {
        int64_t mWhenUs;
        std::shared_ptr<AMessage> mMessage;
    };

    std::mutex mLock;
    std::condition_variable mQueueChangedCondition;

    std::string mName;

    std::list<Event> mEventQueue;

    std::thread mThread;
    bool mRunningLocally;

    // use a separate lock for reply handling, as it is always on another thread
    // use a central lock, however, to avoid creating a mutex for each reply
    std::mutex mRepliesLock;
    std::condition_variable mRepliesCondition;

    // START --- methods used only by AMessage

    // posts a message on this looper with the given timeout
    void post(const std::shared_ptr<AMessage> &msg, int64_t delayUs);

    // creates a reply token to be used with this looper
    std::shared_ptr<AReplyToken> createReplyToken();

    // waits for a response for the reply token.  If status is OK, the response
    // is stored into the supplied variable.  Otherwise, it is unchanged.
    status_t awaitResponse(const std::shared_ptr<AReplyToken> &replyToken, std::shared_ptr<AMessage> *response);
    // posts a reply for a reply token.  If the reply could be successfully posted,
    // it returns OK. Otherwise, it returns an error value.
    status_t postReply(const std::shared_ptr<AReplyToken> &replyToken, const std::shared_ptr<AMessage> &reply);

    // END --- methods used only by AMessage

    bool loop();

    DISALLOW_EVIL_CONSTRUCTORS(ALooper);
};

/**
 * @brief 消息类。包含一条消息的类型、附加数据等信息
 * 
 * 消息只在目标Handler上执行
 */
class AMessage : public std::enable_shared_from_this<AMessage>{
private:
    AMessage();
    AMessage(uint32_t what, const std::shared_ptr<AHandler> &handler);

public:
    /**
     * @brief 创建一个以默认参数构造的AMessage
     *  该AMessage的target指向null
     *  一般用于回复一条消息，如AMessage::create()->postReply()
     * @return 创建成功的AMessage
     */
    static std::shared_ptr<AMessage> create();

    /**
     * @brief 创建一个指定waht和target的AMessage
     * @param what 消息号
     * @param handler 目标handler
     * @return 创建成功的AMessage
     */
    static std::shared_ptr<AMessage> create(uint32_t what, const std::shared_ptr<AHandler> &handler);

    /**
     * @brief 创建一个null AMessage
     *      注意AMessage::createNull() == nullptr
     *      一般用于接收response，如：
     *      auto response = AMessage::createNull();
     *      postAndAwaitResponse(&response);
     * @return 创建成功的AMessage
     */
    static std::shared_ptr<AMessage> createNull();

    /**
     * @brief 指定消息号
     */
    void setWhat(uint32_t what);
    /**
     * @return 消息号
     */
    uint32_t what() const;

    /**
     * @brief 指定目标handler
     * @param handler 目标handler
     */
    void setTarget(const std::shared_ptr<AHandler> &handler);

    /**
     * @brief 清空附加数据
     */
    void clear();

    enum Type {
        kTypeInt32,
        kTypeInt64,
        kTypeSize,
        kTypeFloat,
        kTypeDouble,
        kTypePointer,
        kTypeString,
        kTypeObject,
    };

    void setInt32(const char *name, int32_t value);
    void setInt64(const char *name, int64_t value);
    void setSize(const char *name, size_t value);
    void setFloat(const char *name, float value);
    void setDouble(const char *name, double value);
    void setPointer(const char *name, void *value);
    bool findInt32(const char *name, int32_t *value) const;
    bool findInt64(const char *name, int64_t *value) const;
    bool findSize(const char *name, size_t *value) const;
    bool findFloat(const char *name, float *value) const;
    bool findDouble(const char *name, double *value) const;
    bool findPointer(const char *name, void **value) const;

    void setObject(const char* name, const std::shared_ptr<void>& value);

    template<class T>
    bool findObject(const char* name, std::shared_ptr<T>* value){
        const Item *item = findItem(name, kTypeObject);
        if (item) {                                    
            auto ref = item->u.refValue;
            *value = *(reinterpret_cast<std::shared_ptr<T>*>(&(ref->value)));
            return true;                               
        }                                              
        return false;  
    }

    void setString(const char *name, const char *s, ssize_t len = -1);

    void setString(const char *name, const std::string &s);

    bool findString(const char *name, std::string *value) const;
    

    bool contains(const char *name) const;


    /**
     * @brief 发送当前消息到目标handler
     * @param delayUs 延迟delayUs的时间执行该消息
     * @return OK,发送成功；NOT_FOUND，目标handler所在的looper已经停止或未设置
     */
    status_t post(int64_t delayUs = 0);

    // Posts the message to its target and waits for a response (or error)
    // before returning.
    /**
     * @brief 发送当前消息到目标handler，并等待被回复
     * @param response 输出参数。如果handler回复了该消息，则存放回复的内容
     * @return OK,发送成功；NOT_FOUND，目标handler所在的looper已经停止或未设置;NO_MEM,没有足够内存创建AReplyToken
     */
    status_t postAndAwaitResponse(std::shared_ptr<AMessage> *response);

    // If this returns true, the sender of this message is synchronously
    // awaiting a response and the reply token is consumed from the message
    // and stored into replyID. The reply token must be used to send the response
    // using "postReply" below.
    /**
     * @brief 判断当前消息的发送线程是否正在阻塞等待消息。如果是则获取其AReplyToken
     * @param replyID 输出参数。用于存放当前消息的回复ID，用于作为postReply的参数
     * @return true，发送线程正在阻塞等待（即调用了postAndAwaitResponse）；false，发送线程没有在等待回复
     */
    bool senderAwaitsResponse(std::shared_ptr<AReplyToken> *replyID);

    // Posts the message as a response to a reply token.  A reply token can
    // only be used once. Returns OK if the response could be posted; otherwise,
    // an error.
    /**
     * @brief 向replyToken回复消息
     * @param replyToken 要回复的目标
     * @return OK，发送成功；NOT_FOUND，参数无效或looper已停止；BUSY，目标消息已经被回复过了
     */
    status_t postReply(const std::shared_ptr<AReplyToken> &replyToken);

    // Performs a deep-copy of "this", contained messages are in turn "dup'ed".
    // Warning: RefBase items, i.e. "objects" are _not_ copied but only have
    // their refcount incremented.
    /**
     * @brief 复制当前消息，包括附加数据
     * @return 复制后的消息。其生命周期是独立的。
     */
    std::shared_ptr<AMessage> dup() const;

    // std::string debugString(int32_t indent = 0) const;
   
    /**
     * @return 返回当前附加数据的数量
     */
    size_t countEntries() const;
    
    /**
     * @brief 获取index所指的附加数据的类型和名字
     * @param type 输出参数。所选附加数据的类型
     * @return NULL index非法；其他 所选附加数据的名称
     */
    const char *getEntryNameAt(size_t index, Type *type) const;

    virtual ~AMessage();

private:
    friend class ALooper; // deliver()

    uint32_t mWhat;
    // used only for debugging
    handler_id mTarget;

    std::weak_ptr<AHandler> mHandler;
    std::weak_ptr<ALooper> mLooper;

    struct RefHolder {
        RefHolder(const std::shared_ptr<void>& shptr) : value(shptr) {
        }

        std::shared_ptr<void> value;
    };

    struct Item {
        union {
            int32_t int32Value;
            int64_t int64Value;
            size_t sizeValue;
            float floatValue;
            double doubleValue;
            void *ptrValue;
            RefHolder* refValue;
            std::string* stringValue;
        } u;
        const char *mName;
        size_t      mNameLength;
        Type mType;
        void setName(const char *name, size_t len);
    };

    enum {
        kMaxNumItems = 64
    };
    Item mItems[kMaxNumItems];
    size_t mNumItems;

    Item *allocateItem(const char *name);
    void freeItemValue(Item *item);
    const Item *findItem(const char *name, Type type) const;

    size_t findItemIndex(const char *name, size_t len) const;

    void deliver();

    DISALLOW_EVIL_CONSTRUCTORS(AMessage);
};

} // namespace alooper


#endif