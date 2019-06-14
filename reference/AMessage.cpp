/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AMessage"
//#define LOG_NDEBUG 0
//#define DUMP_STATS
#include <cutils/log.h>

#include "AMessage.h"

#include <ctype.h>

#include "AAtomizer.h"
#include "ABuffer.h"
#include "ADebug.h"
#include "ALooperRoster.h"
#include "AHandler.h"
#include "AString.h"

#include <binder/Parcel.h>
#include <media/stagefright/foundation/hexdump.h>

namespace android {

extern ALooperRoster gLooperRoster;

status_t AReplyToken::setReply(const sp<AMessage> &reply) {
    if (mReplied) {
        ALOGE("trying to post a duplicate reply");
        return -EBUSY;
    }
    CHECK(mReply == NULL);
    mReply = reply;
    mReplied = true;
    return OK;
}

AMessage::AMessage(void)
    : mWhat(0),
      mTarget(0),
      mNumItems(0) {
}

AMessage::AMessage(uint32_t what, const sp<const AHandler> &handler)
    : mWhat(what),
      mNumItems(0) {
    setTarget(handler);
}

AMessage::~AMessage() {
    clear();
}

void AMessage::setWhat(uint32_t what) {
    mWhat = what;
}

uint32_t AMessage::what() const {
    return mWhat;
}

void AMessage::setTarget(const sp<const AHandler> &handler) {
    if (handler == NULL) {
        mTarget = 0;
        mHandler.clear();
        mLooper.clear();
    } else {
        mTarget = handler->id();
        mHandler = handler->getHandler();
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

void AMessage::freeItemValue(Item *item) {
    switch (item->mType) {
        case kTypeString:
        {
            delete item->u.stringValue;
            break;
        }

        case kTypeObject:
        case kTypeMessage:
        case kTypeBuffer:
        {
            if (item->u.refValue != NULL) {
                item->u.refValue->decStrong(this);
            }
            break;
        }

        default:
            break;
    }
}

#ifdef DUMP_STATS
#include <utils/Mutex.h>

Mutex gLock;
static int32_t gFindItemCalls = 1;
static int32_t gDupCalls = 1;
static int32_t gAverageNumItems = 0;
static int32_t gAverageNumChecks = 0;
static int32_t gAverageNumMemChecks = 0;
static int32_t gAverageDupItems = 0;
static int32_t gLastChecked = -1;

static void reportStats() {
    int32_t time = (ALooper::GetNowUs() / 1000);
    if (time / 1000 != gLastChecked / 1000) {
        gLastChecked = time;
        ALOGI("called findItemIx %zu times (for len=%.1f i=%.1f/%.1f mem) dup %zu times (for len=%.1f)",
                gFindItemCalls,
                gAverageNumItems / (float)gFindItemCalls,
                gAverageNumChecks / (float)gFindItemCalls,
                gAverageNumMemChecks / (float)gFindItemCalls,
                gDupCalls,
                gAverageDupItems / (float)gDupCalls);
        gFindItemCalls = gDupCalls = 1;
        gAverageNumItems = gAverageNumChecks = gAverageNumMemChecks = gAverageDupItems = 0;
        gLastChecked = time;
    }
}
#endif

inline size_t AMessage::findItemIndex(const char *name, size_t len) const {
#ifdef DUMP_STATS
    size_t memchecks = 0;
#endif
    size_t i = 0;
    for (; i < mNumItems; i++) {
        if (len != mItems[i].mNameLength) {
            continue;
        }
#ifdef DUMP_STATS
        ++memchecks;
#endif
        if (!memcmp(mItems[i].mName, name, len)) {
            break;
        }
    }
#ifdef DUMP_STATS
    {
        Mutex::Autolock _l(gLock);
        ++gFindItemCalls;
        gAverageNumItems += mNumItems;
        gAverageNumMemChecks += memchecks;
        gAverageNumChecks += i;
        reportStats();
    }
#endif
    return i;
}

// assumes item's name was uninitialized or NULL
void AMessage::Item::setName(const char *name, size_t len) {
    mNameLength = len;
    mName = new char[len + 1];
    memcpy((void*)mName, name, len + 1);
}

AMessage::Item *AMessage::allocateItem(const char *name) {
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

const AMessage::Item *AMessage::findItem(
        const char *name, Type type) const {
    size_t i = findItemIndex(name, strlen(name));
    if (i < mNumItems) {
        const Item *item = &mItems[i];
        return item->mType == type ? item : NULL;

    }
    return NULL;
}

bool AMessage::contains(const char *name) const {
    size_t i = findItemIndex(name, strlen(name));
    return i < mNumItems;
}

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

#undef BASIC_TYPE

void AMessage::setString(
        const char *name, const char *s, ssize_t len) {
    Item *item = allocateItem(name);
    item->mType = kTypeString;
    item->u.stringValue = new AString(s, len < 0 ? strlen(s) : len);
}

void AMessage::setString(
        const char *name, const AString &s) {
    setString(name, s.c_str(), s.size());
}

void AMessage::setObjectInternal(
        const char *name, const sp<RefBase> &obj, Type type) {
    Item *item = allocateItem(name);
    item->mType = type;

    if (obj != NULL) { obj->incStrong(this); }
    item->u.refValue = obj.get();
}

void AMessage::setObject(const char *name, const sp<RefBase> &obj) {
    setObjectInternal(name, obj, kTypeObject);
}

void AMessage::setBuffer(const char *name, const sp<ABuffer> &buffer) {
    setObjectInternal(name, sp<RefBase>(buffer), kTypeBuffer);
}

void AMessage::setMessage(const char *name, const sp<AMessage> &obj) {
    Item *item = allocateItem(name);
    item->mType = kTypeMessage;

    if (obj != NULL) { obj->incStrong(this); }
    item->u.refValue = obj.get();
}

void AMessage::setRect(
        const char *name,
        int32_t left, int32_t top, int32_t right, int32_t bottom) {
    Item *item = allocateItem(name);
    item->mType = kTypeRect;

    item->u.rectValue.mLeft = left;
    item->u.rectValue.mTop = top;
    item->u.rectValue.mRight = right;
    item->u.rectValue.mBottom = bottom;
}

bool AMessage::findString(const char *name, AString *value) const {
    const Item *item = findItem(name, kTypeString);
    if (item) {
        *value = *item->u.stringValue;
        return true;
    }
    return false;
}

bool AMessage::findObject(const char *name, sp<RefBase> *obj) const {
    const Item *item = findItem(name, kTypeObject);
    if (item) {
        *obj = item->u.refValue;
        return true;
    }
    return false;
}

bool AMessage::findBuffer(const char *name, sp<ABuffer> *buf) const {
    const Item *item = findItem(name, kTypeBuffer);
    if (item) {
        *buf = (ABuffer *)(item->u.refValue);
        return true;
    }
    return false;
}

bool AMessage::findMessage(const char *name, sp<AMessage> *obj) const {
    const Item *item = findItem(name, kTypeMessage);
    if (item) {
        *obj = static_cast<AMessage *>(item->u.refValue);
        return true;
    }
    return false;
}

bool AMessage::findRect(
        const char *name,
        int32_t *left, int32_t *top, int32_t *right, int32_t *bottom) const {
    const Item *item = findItem(name, kTypeRect);
    if (item == NULL) {
        return false;
    }

    *left = item->u.rectValue.mLeft;
    *top = item->u.rectValue.mTop;
    *right = item->u.rectValue.mRight;
    *bottom = item->u.rectValue.mBottom;

    return true;
}

void AMessage::deliver() {
    sp<AHandler> handler = mHandler.promote();
    if (handler == NULL) {
        ALOGW("failed to deliver message as target handler %d is gone.", mTarget);
        return;
    }

    handler->deliverMessage(this);
}

status_t AMessage::post(int64_t delayUs) {
    sp<ALooper> looper = mLooper.promote();
    if (looper == NULL) {
        ALOGW("failed to post message as target looper for handler %d is gone.", mTarget);
        return -ENOENT;
    }

    looper->post(this, delayUs);
    return OK;
}

status_t AMessage::postAndAwaitResponse(sp<AMessage> *response) {
    sp<ALooper> looper = mLooper.promote();
    if (looper == NULL) {
        ALOGW("failed to post message as target looper for handler %d is gone.", mTarget);
        return -ENOENT;
    }

    sp<AReplyToken> token = looper->createReplyToken();
    if (token == NULL) {
        ALOGE("failed to create reply token");
        return -ENOMEM;
    }
    setObject("replyID", token);

    looper->post(this, 0 /* delayUs */);
    return looper->awaitResponse(token, response);
}

status_t AMessage::postReply(const sp<AReplyToken> &replyToken) {
    if (replyToken == NULL) {
        ALOGW("failed to post reply to a NULL token");
        return -ENOENT;
    }
    sp<ALooper> looper = replyToken->getLooper();
    if (looper == NULL) {
        ALOGW("failed to post reply as target looper is gone.");
        return -ENOENT;
    }
    return looper->postReply(replyToken, this);
}

bool AMessage::senderAwaitsResponse(sp<AReplyToken> *replyToken) {
    sp<RefBase> tmp;
    bool found = findObject("replyID", &tmp);

    if (!found) {
        return false;
    }

    *replyToken = static_cast<AReplyToken *>(tmp.get());
    tmp.clear();
    setObject("replyID", tmp);
    // TODO: delete Object instead of setting it to NULL

    return *replyToken != NULL;
}

sp<AMessage> AMessage::dup() const {
    sp<AMessage> msg = new AMessage(mWhat, mHandler.promote());
    msg->mNumItems = mNumItems;

#ifdef DUMP_STATS
    {
        Mutex::Autolock _l(gLock);
        ++gDupCalls;
        gAverageDupItems += mNumItems;
        reportStats();
    }
#endif

    for (size_t i = 0; i < mNumItems; ++i) {
        const Item *from = &mItems[i];
        Item *to = &msg->mItems[i];

        to->setName(from->mName, from->mNameLength);
        to->mType = from->mType;

        switch (from->mType) {
            case kTypeString:
            {
                to->u.stringValue =
                    new AString(*from->u.stringValue);
                break;
            }

            case kTypeObject:
            case kTypeBuffer:
            {
                to->u.refValue = from->u.refValue;
                to->u.refValue->incStrong(msg.get());
                break;
            }

            case kTypeMessage:
            {
                sp<AMessage> copy =
                    static_cast<AMessage *>(from->u.refValue)->dup();

                to->u.refValue = copy.get();
                to->u.refValue->incStrong(msg.get());
                break;
            }

            default:
            {
                to->u = from->u;
                break;
            }
        }
    }

    return msg;
}

static void appendIndent(AString *s, int32_t indent) {
    static const char kWhitespace[] =
        "                                        "
        "                                        ";

    CHECK_LT((size_t)indent, sizeof(kWhitespace));

    s->append(kWhitespace, indent);
}

static bool isFourcc(uint32_t what) {
    return isprint(what & 0xff)
        && isprint((what >> 8) & 0xff)
        && isprint((what >> 16) & 0xff)
        && isprint((what >> 24) & 0xff);
}

AString AMessage::debugString(int32_t indent) const {
    AString s = "AMessage(what = ";

    AString tmp;
    if (isFourcc(mWhat)) {
        tmp = AStringPrintf(
                "'%c%c%c%c'",
                (char)(mWhat >> 24),
                (char)((mWhat >> 16) & 0xff),
                (char)((mWhat >> 8) & 0xff),
                (char)(mWhat & 0xff));
    } else {
        tmp = AStringPrintf("0x%08x", mWhat);
    }
    s.append(tmp);

    if (mTarget != 0) {
        tmp = AStringPrintf(", target = %d", mTarget);
        s.append(tmp);
    }
    s.append(") = {\n");

    for (size_t i = 0; i < mNumItems; ++i) {
        const Item &item = mItems[i];

        switch (item.mType) {
            case kTypeInt32:
                tmp = AStringPrintf(
                        "int32_t %s = %d", item.mName, item.u.int32Value);
                break;
            case kTypeInt64:
                tmp = AStringPrintf(
                        "int64_t %s = %lld", item.mName, item.u.int64Value);
                break;
            case kTypeSize:
                tmp = AStringPrintf(
                        "size_t %s = %d", item.mName, item.u.sizeValue);
                break;
            case kTypeFloat:
                tmp = AStringPrintf(
                        "float %s = %f", item.mName, item.u.floatValue);
                break;
            case kTypeDouble:
                tmp = AStringPrintf(
                        "double %s = %f", item.mName, item.u.doubleValue);
                break;
            case kTypePointer:
                tmp = AStringPrintf(
                        "void *%s = %p", item.mName, item.u.ptrValue);
                break;
            case kTypeString:
                tmp = AStringPrintf(
                        "string %s = \"%s\"",
                        item.mName,
                        item.u.stringValue->c_str());
                break;
            case kTypeObject:
                tmp = AStringPrintf(
                        "RefBase *%s = %p", item.mName, item.u.refValue);
                break;
            case kTypeBuffer:
            {
                sp<ABuffer> buffer = static_cast<ABuffer *>(item.u.refValue);

                if (buffer != NULL && buffer->data() != NULL && buffer->size() <= 64) {
                    tmp = AStringPrintf("Buffer %s = {\n", item.mName);
                    hexdump(buffer->data(), buffer->size(), indent + 4, &tmp);
                    appendIndent(&tmp, indent + 2);
                    tmp.append("}");
                } else {
                    tmp = AStringPrintf(
                            "Buffer *%s = %p", item.mName, buffer.get());
                }
                break;
            }
            case kTypeMessage:
                tmp = AStringPrintf(
                        "AMessage %s = %s",
                        item.mName,
                        static_cast<AMessage *>(
                            item.u.refValue)->debugString(
                                indent + strlen(item.mName) + 14).c_str());
                break;
            case kTypeRect:
                tmp = AStringPrintf(
                        "Rect %s(%d, %d, %d, %d)",
                        item.mName,
                        item.u.rectValue.mLeft,
                        item.u.rectValue.mTop,
                        item.u.rectValue.mRight,
                        item.u.rectValue.mBottom);
                break;
            default:
                TRESPASS();
        }

        appendIndent(&s, indent);
        s.append("  ");
        s.append(tmp);
        s.append("\n");
    }

    appendIndent(&s, indent);
    s.append("}");

    return s;
}

// static
sp<AMessage> AMessage::FromParcel(const Parcel &parcel) {
    int32_t what = parcel.readInt32();
    sp<AMessage> msg = new AMessage();
    msg->setWhat(what);

    msg->mNumItems = static_cast<size_t>(parcel.readInt32());
    if (msg->mNumItems > kMaxNumItems) {
        ALOGE("Too large number of items clipped.");
        msg->mNumItems = kMaxNumItems;
    }

    for (size_t i = 0; i < msg->mNumItems; ++i) {
        Item *item = &msg->mItems[i];

        const char *name = parcel.readCString();
        if (name == NULL) {
            ALOGE("Failed reading name for an item. Parsing aborted.");
            msg->mNumItems = i;
            break;
        }

        item->mType = static_cast<Type>(parcel.readInt32());
        // setName() happens below so that we don't leak memory when parsing
        // is aborted in the middle.
        switch (item->mType) {
            case kTypeInt32:
            {
                item->u.int32Value = parcel.readInt32();
                break;
            }

            case kTypeInt64:
            {
                item->u.int64Value = parcel.readInt64();
                break;
            }

            case kTypeSize:
            {
                item->u.sizeValue = static_cast<size_t>(parcel.readInt32());
                break;
            }

            case kTypeFloat:
            {
                item->u.floatValue = parcel.readFloat();
                break;
            }

            case kTypeDouble:
            {
                item->u.doubleValue = parcel.readDouble();
                break;
            }

            case kTypeString:
            {
                const char *stringValue = parcel.readCString();
                if (stringValue == NULL) {
                    ALOGE("Failed reading string value from a parcel. "
                        "Parsing aborted.");
                    msg->mNumItems = i;
                    continue;
                    // The loop will terminate subsequently.
                } else {
                    item->u.stringValue = new AString(stringValue);
                }
                break;
            }

            case kTypeMessage:
            {
                sp<AMessage> subMsg = AMessage::FromParcel(parcel);
                subMsg->incStrong(msg.get());

                item->u.refValue = subMsg.get();
                break;
            }

            default:
            {
                ALOGE("This type of object cannot cross process boundaries.");
                TRESPASS();
            }
        }

        item->setName(name, strlen(name));
    }

    return msg;
}

void AMessage::writeToParcel(Parcel *parcel) const {
    parcel->writeInt32(static_cast<int32_t>(mWhat));
    parcel->writeInt32(static_cast<int32_t>(mNumItems));

    for (size_t i = 0; i < mNumItems; ++i) {
        const Item &item = mItems[i];

        parcel->writeCString(item.mName);
        parcel->writeInt32(static_cast<int32_t>(item.mType));

        switch (item.mType) {
            case kTypeInt32:
            {
                parcel->writeInt32(item.u.int32Value);
                break;
            }

            case kTypeInt64:
            {
                parcel->writeInt64(item.u.int64Value);
                break;
            }

            case kTypeSize:
            {
                parcel->writeInt32(static_cast<int32_t>(item.u.sizeValue));
                break;
            }

            case kTypeFloat:
            {
                parcel->writeFloat(item.u.floatValue);
                break;
            }

            case kTypeDouble:
            {
                parcel->writeDouble(item.u.doubleValue);
                break;
            }

            case kTypeString:
            {
                parcel->writeCString(item.u.stringValue->c_str());
                break;
            }

            case kTypeMessage:
            {
                static_cast<AMessage *>(item.u.refValue)->writeToParcel(parcel);
                break;
            }

            default:
            {
                ALOGE("This type of object cannot cross process boundaries.");
                TRESPASS();
            }
        }
    }
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

}  // namespace android
