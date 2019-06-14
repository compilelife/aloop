#include <gtest/gtest.h>
#include "../src/aloop.h"

using namespace std;
using namespace aloop;

TEST(AMessage, CreateNull) {
    auto nullMsg = AMessage::createNull();
    ASSERT_EQ(nullptr, nullMsg);
}

TEST(AMessage, Create) {
    auto msg = AMessage::create();
    ASSERT_EQ(NOT_FOUND, msg->post());
}

TEST(AMessage, Create2) {
    auto msg = AMessage::create(1, nullptr);
    ASSERT_EQ(1, msg->what());
}

static void checkMsg(const shared_ptr<AMessage>& msg, int index, AMessage::Type type, const char* name){
    AMessage::Type _type;
    const char* _name = msg->getEntryNameAt(index, &_type);
    ASSERT_EQ(type, _type);
    ASSERT_STREQ(name, _name);
}

TEST(AMessage, Entry) {
    auto msg = AMessage::create();

    msg->setInt32("int32", 1);
    msg->setInt64("int64", 1);
    msg->setSize("size", 5);
    msg->setFloat("float", 1.0f);
    msg->setDouble("double", 1.0);
    msg->setPointer("pointer", msg.get());
    msg->setObject("object", msg);
    msg->setString("str", "aloop");

    ASSERT_EQ(8, msg->countEntries());
    checkMsg(msg, 0, AMessage::kTypeInt32, "int32");
    checkMsg(msg, 1, AMessage::kTypeInt64, "int64");
    checkMsg(msg, 2, AMessage::kTypeSize, "size");
    checkMsg(msg, 3, AMessage::kTypeFloat, "float");
    checkMsg(msg, 4, AMessage::kTypeDouble, "double");
    checkMsg(msg, 5, AMessage::kTypePointer, "pointer");
    checkMsg(msg, 6, AMessage::kTypeObject, "object");
    checkMsg(msg, 7, AMessage::kTypeString, "str");

    msg->clear();
    ASSERT_EQ(0, msg->countEntries());
}

TEST(AMessage, FindEntry) {
    auto msg = AMessage::create();

    msg->setInt32("int32", 10);
    ASSERT_TRUE(msg->contains("int32"));

    int32_t value = 0;
    ASSERT_TRUE(msg->findInt32("int32", &value));
    ASSERT_EQ(10, value);
}

TEST(AMessage, ObjectEntry) {
    class Object{
    public:
        ~Object(){
            printf("~Object\n");
        }
    };

    auto msg = AMessage::create();
    weak_ptr<Object> wObj;
    {
        shared_ptr<Object> obj(new Object);
        wObj = obj;
        msg->setObject("obj", obj);
    }
    
    ASSERT_EQ(1, wObj.use_count());

    msg->clear();

    ASSERT_EQ(0, wObj.use_count());
}

TEST(AMessage, setWhat){
    auto msg = AMessage::create();
    ASSERT_NE(1, msg->what());
    msg->setWhat(1);
    ASSERT_EQ(1, msg->what());
}

TEST(AMessage, dup){
    auto dupMsg = AMessage::createNull();
    weak_ptr<AMessage> watch;
    {
        auto msg = AMessage::create(1, nullptr);
        msg->setInt32("int32", 2);

        watch = msg;
        dupMsg = msg->dup();
    }

    ASSERT_TRUE(watch.expired());
    ASSERT_EQ(1, dupMsg->what());
    int value = 0;
    ASSERT_TRUE(dupMsg->findInt32("int32", &value));
    ASSERT_EQ(2, value);
    ASSERT_EQ(1, dupMsg->countEntries());
}