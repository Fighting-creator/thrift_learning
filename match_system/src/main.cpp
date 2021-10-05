// This autogenerated skeleton file illustrates how to build a server.
// You should copy it to another filename to avoid overwriting it.

#include "match_server/Match.h"
#include "save_client/Save.h"
#include <thrift/concurrency/ThreadManager.h>
#include <thrift/concurrency/ThreadFactory.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TSimpleServer.h>
#include <thrift/server/TThreadPoolServer.h>
#include <thrift/server/TThreadedServer.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>
#include <thrift/TToString.h>

#include <iostream>
#include <algorithm>
#include <thread> //C++中有一个thread的库，是用来开线程。
#include <mutex> //锁
#include <condition_variable> //条件变量，对锁进行封装。
#include <queue> //自定消费队列使用普通队列就可以。
//通过条件变量和锁可以将队列包装成一个消费队列。
#include <vector>

using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::server;

using namespace  ::match_dao;
using namespace  ::save_service;
using namespace std;

//定义一个任务类型
struct Task
{
    User user;
    string type;
};

//自定义消息队列
struct MessageQueue
{
    queue<Task> q;
    mutex m;
    condition_variable cv;
}message_queue;

//定义一个匹配池
class Pool
{
    public:
        //将匹配结果保存
        void save_result(int a, int b)
        {
            printf("Match Result: %d %d\n", a, b);

            std::shared_ptr<TTransport> socket(new TSocket("123.57.47.211", 9090));
            std::shared_ptr<TTransport> transport(new TBufferedTransport(socket));
            std::shared_ptr<TProtocol> protocol(new TBinaryProtocol(transport));
            SaveClient client(protocol);

            try {
                transport->open();

                int res = client.save_data("acs_468", "e75e214f", a, b);

                if (!res) puts("success");
                else puts("failed");

                transport->close();
            } catch (TException& tx) {
                cout << "ERROR: " << tx.what() << endl;
            }

        }

        void match()
        {
            while (users.size() > 1)
            {
                sort(users.begin(), users.end(), [&](User a, User b){
                        return a.score < b.score;
                        });

                bool flag = true;
                for (uint32_t i = 1; i < users.size(); i ++)
                {
                    auto a = users[i - 1], b = users[i];
                    if (b.score - a.score <= 50)
                    {
                        //这个erase是一个左闭右开的区间
                        users.erase(users.begin() + i - 1, users.begin() + i + 1);
                        save_result(a.id, b.id);

                        flag = false;
                        break;
                    }
                }
                //没有玩家匹配时，退出循环。
                if (flag) break;
            }
        }

        void add(User user)
        {
            users.push_back(user);
        }

        void remove(User user)
        {
            //删除操作通过遍历编号完成
            for (uint32_t i = 0; i < users.size(); i ++)
                if (users[i].id == user.id)
                {
                    users.erase(users.begin() + i);
                    break;
                }
        }

    private:
        vector<User> users;
}pool;



/* 不管是添加还是删除，都需要将操作放入队列中。
 * 因为这里的对象是消费队列，我们需要将所有关于队列的操作都加上锁，防止多个线程同时操作队列，导致出现数据覆盖等问题。
 *
 * 说明:当一个进程优先执行含有队列的操作时，就会获取这个锁，那么其它进程执行队列的操作时，就无法获取到锁而卡在那里，直到获取了锁才能执行。因为锁是唯一一个。
 *
 */
class MatchHandler : virtual public MatchIf {
    public:
        MatchHandler() {
            // Your initialization goes here
        }

        int32_t add_user(const User& user, const std::string& info) {
            // Your implementation goes here
            printf("add_user\n");

            //通过消息队列中的锁将方法锁着。
            //好处:你不需要进行解锁操作，当方法执行完毕，这个变量就会自动注销掉
            unique_lock<mutex> lck(message_queue.m);
            message_queue.q.push({user, "add"});
            //唤醒所有条件变量
            message_queue.cv.notify_all();

            return 0;
        }

        int32_t remove_user(const User& user, const std::string& info) {
            // Your implementation goes here
            printf("remove_user\n");

            unique_lock<mutex> lck(message_queue.m);
            message_queue.q.push({user, "remove"});
            message_queue.cv.notify_all();

            return 0;
        }

};

class MatchCloneFactory : virtual public MatchIfFactory {
    public:
        ~MatchCloneFactory() override = default;
        MatchIf* getHandler(const ::apache::thrift::TConnectionInfo& connInfo) override
        {
            std::shared_ptr<TSocket> sock = std::dynamic_pointer_cast<TSocket>(connInfo.transport);

            /*cout << "Incoming connection\n";
            cout << "\tSocketInfo: "  << sock->getSocketInfo() << "\n";
            cout << "\tPeerHost: "    << sock->getPeerHost() << "\n";
            cout << "\tPeerAddress: " << sock->getPeerAddress() << "\n";
            cout << "\tPeerPort: "    << sock->getPeerPort() << "\n";*/
            return new MatchHandler;
        }
        void releaseHandler(MatchIf* handler) override {
            delete handler;
        }
};

//写死循环的目的:充当监控器，不停地死循环保证时刻监控是否有用户传进来。
void consume_task()
{
    while (true)
    {
        unique_lock<mutex> lck(message_queue.m);
        if (message_queue.q.empty())
        {
            /* 因为消费者线程（不止一个）会频繁判断队列是否为空，导致CPU做无用功。
             * 所以使用条件变量的wait()函数可使得当前线程阻塞，直至条件变量唤醒。
             * 当线程阻塞的时候，该函数会自动解锁，允许其他线程执行。
             */
            // message_queue.cv.wait(lck);

            /* 每一秒匹配一次。
             * 这样消费者线程（不止一个）不会频繁判断队列是否为空，导致CPU做无用功。
             * 然后实现匹配的过程中，还在向队列中加人或减人。
             */
            lck.unlock();
            pool.match();
            sleep(1);
        }
        else
        {
            auto task = message_queue.q.front();
            message_queue.q.pop();
            /* 将队头的任务拿出，就不对队列进行操作。
             * 故将锁解除并交给需要它的线程，提高效率。
             * 这里也就是便执行任务的同时边添加和删除用户，两不误。
             * 别忘记这里是两个线程同时运行哦！
             *
             * 经验：处理完共享的对象后，后续不在使用，一定要及时解锁。
             */
            lck.unlock();

            /* do task
             * 做任务，本质是将所有内容放入一个类似于池的东西。
             * 这里我们定义为匹配池。
             *
             */
            if (task.type == "add") pool.add(task.user);
            else if (task.type == "remove") pool.remove(task.user);

            pool.match();
        }
    }
}

int main(int argc, char **argv) {
    TThreadedServer server(
            std::make_shared<MatchProcessorFactory>(std::make_shared<MatchCloneFactory>()),
            std::make_shared<TServerSocket>(9090), //port
            std::make_shared<TBufferedTransportFactory>(),
            std::make_shared<TBinaryProtocolFactory>());

    cout << "Start Match Server" << endl;

    /*thread 变量名(函数指针) 表示开一个线程。
     *开线程的目的:如果将死循环写在这里，导致serve执行不了。
     *故需要开一个线程。
     */
    thread matching_thread(consume_task);

    server.serve();
    return 0;
}

