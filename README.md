轻量的c++ looper实现，移植自aosp的libstagefright。

# 特色
1. 纯c++11实现，可在任意平台编译
2. 只有一个头文件，一个源文件，容易集成
3. 扩展性强

# 如何使用

## 编译

将src下的文件复制到你的工程中，编译时开启`std=c++11`支持即可。

如`g++ -std=c++11 main.cpp aloop.cpp -lpthread`

## 概念说明

主要的类是ALooper, AHandler, AMessage。

ALooper封装了线程循环，已经进行事件分发；

AHandler需要用户继承，主要用来进行事件处理。一个ALooper可以注册多个AHandler复用线程资源。

AMessage代表一个消息，可附带各种类型的用户自定义数据，甚至附带一个函数。一般，一个AMessage只发往一个指定的AHandler处理。

## 代码调用

这里只演示基础用法，更高级的用法见`example/main.cpp`

### 启动Looper

```c++
auto looper = ALooper::create();
looper->start();
```

### 准备Handler

```c++
class MyHanlder: public AHandler{
protected:
    void onMessageReceived(const std::shared_ptr<AMessage> &msg){
        //在这里处理事件
    }
};

auto handler = std::shared_ptr<MyHandler>(new MyHandler);

looper->registerHandler(handler);
```

### 发送消息

```c++
auto msg = AMessage::create(1, handler);//1是自定义的整数消息类型
msg->setInt32("extraInt", 3);//使用setXXX函数设置附加数据
msg->post();//发送消息
```

# 目录说明
- reference: 参考目录。源码来自aosp 6.0的`android/frameworks/av/media/libstagefright`。源码分析见：https://zhuanlan.zhihu.com/p/68713221
- src: 源码目录。使用所需的所有文件
- test: 测试代码
- example: 实例代码