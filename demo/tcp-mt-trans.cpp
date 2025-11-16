/******************************************************************************
 * 基本示例：使用 Boost.Asio 多线程收发 TCP 数据                              *
 ******************************************************************************/

#include <atomic>
#include <boost/asio.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <forward_list>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

namespace Ba = boost::asio;
namespace Ip = Ba::ip;

using BoostEC = boost::system::error_code;
using Executor = Ba::any_io_executor;
using Tcp = Ip::tcp;

/******************************************************************************/
constexpr std::size_t kBufferSize = 65536;
/******************************************************************************/

const std::uint8_t kData[kBufferSize] = {};

class Server;

// Session 类的声明周期与会话生命周期相绑定: 类创建时会话就开始,
// 类析构时会话就终止. 这种设计有好有坏: 好处是简单明了,
// 坏处是会话终止后类中也许仍然保有有用的其它信息 (例如统计数据).
// 其实会话终止后的 Session 对象性质也许就有点像僵尸进程.
class Session
{
public:
  Server& mServer;
  Tcp::socket mSock;

  Session(Server& server, Tcp::socket&& sock)
    : mServer(server)
    , mSock(std::move(sock))
  {
    Ba::post(mSock.get_executor(), [this]() {
      do_send(), do_receive(); // 全双工收发
    });
  }

  void delete_later(std::function<void(std::string)> cb = {})
  {
    // 可以被 Session 之外的类调用, 以暴力关闭会话.
    // 为了保证这个方法从 Session 之外调用是安全的, 需要用 post 让 Session
    // 所在的执行器去主动结束所有异步过程.
    Ba::post(mSock.get_executor(),
             [this, cb = std::move(cb)]() { cb ? cb(do_close()) : void(0); });
    // 在多线程语境下, 一旦该方法返回, this 就应当被视为已经被无效化.
  }

  using Clock = std::chrono::steady_clock;

  struct Statistics
  {
    Tcp::endpoint mRemoteEndpoint;
    Clock::time_point mTimeEstablished;
    Clock::time_point mTimeLastActive;
    std::size_t mAmountSent;
    std::size_t mAmountReceive;
  };

  Statistics statistics() const
  {
    return {
      .mRemoteEndpoint = mRemoteEndpoint,
      .mTimeEstablished = mTimeEstablished,
      .mTimeLastActive = mTimeLastActive.load(std::memory_order_relaxed),
      .mAmountSent = mAmountSent.load(std::memory_order_relaxed),
      .mAmountReceive = mAmountReceive.load(std::memory_order_relaxed),
    };
  }

private:
  std::array<std::uint8_t, kBufferSize> mBuf;

  const Tcp::endpoint mRemoteEndpoint{ mSock.remote_endpoint() };
  const Clock::time_point mTimeEstablished{ Clock::now() };
  std::atomic<Clock::time_point> mTimeLastActive;
  std::atomic<std::size_t> mAmountSent;
  std::atomic<std::size_t> mAmountReceive;

  bool mGrace{ false };

  std::string do_close();

  void do_send()
  {
    mTimeLastActive.store(Clock::now(), std::memory_order_relaxed);
    if (mGrace) {
      BoostEC ec;
      mSock.shutdown(Tcp::socket::shutdown_send, ec);
      if (ec)
        std::cerr << "session " << this << " shutdown failed: " << ec.message()
                  << '\n';
      auto err = do_close();
      if (!err.empty())
        std::cerr << err << '\n';
      return;
    }

    mSock.async_send(Ba::buffer(kData),
                     [this](const BoostEC& ec, std::size_t len) {
                       if (ec) {
                         if (ec == Ba::error::operation_aborted)
                           return;

                         std::cerr << "session " << this
                                   << " send failed: " << ec.message() << '\n';
                         auto err = do_close();
                         if (!err.empty())
                           std::cerr << err << '\n';
                         return;
                       }

                       mAmountSent.fetch_add(len, std::memory_order_relaxed);
                       do_send();
                     });
  }

  void do_receive()
  {
    mSock.async_receive(
      Ba::buffer(mBuf), [this](const BoostEC& ec, std::size_t len) {
        mTimeLastActive.store(Clock::now(), std::memory_order_relaxed);
        // send 和 receive 在更新活动时间的时机上有差别

        if (ec) {
          if (ec == Ba::error::operation_aborted)
            return;

          if (ec == Ba::error::eof) {
            BoostEC ec;
            mSock.shutdown(Tcp::socket::shutdown_receive, ec);
            if (ec) {
              std::cerr << "session " << this
                        << " shutdown receive failed: " << ec.message() << '\n';
              auto err = do_close();
              if (!err.empty())
                std::cerr << err << '\n';
              return;
            }
            mGrace = true; // 让发送协程优雅关闭
            return;
          }

          std::cerr << "session " << this << " receive failed: " << ec.message()
                    << '\n';
          auto err = do_close();
          if (!err.empty())
            std::cerr << err << '\n';
          return;
        }

        mAmountReceive.fetch_add(len, std::memory_order_relaxed);
        do_receive();
      });
  }
};

class Server
{
public:
  Server(Executor ex)
    : mEx(std::move(ex))
    , mAcpt(Ba::make_strand(mEx))
  {
  }

  virtual ~Server() noexcept
  {
    if (!mSessions.empty())
      abort();
    // 设计上, Session 对象的生命周期由 Server 管理.
    // 如果 Server 析构时还有 Session 在工作, 则那些对象极有可能发生指针悬挂,
    // 从而导致程序崩溃 -- 这种情况十分难于调试, 因此我们在这里杜绝它.
  }

  void start(const Tcp::endpoint& endpoint,
             int backlog = Ba::socket_base::max_listen_connections,
             std::function<void(std::string)> cb = {})
  {
    Ba::post(mAcpt.get_executor(), [=, cb = std::move(cb)]() {
      std::ostringstream errs;
      BoostEC ec;

      mAcpt.open(endpoint.protocol(), ec);
      if (ec) {
        errs << "open failed: " << ec.message();
        goto RETURN;
      };

      mAcpt.set_option(Ba::socket_base::reuse_address(true), ec);
      if (ec) {
        errs << "set_option failed: " << ec.message();
        goto RETURN;
      }

      mAcpt.bind(endpoint, ec);
      if (ec) {
        errs << "bind failed: " << ec.message();
        goto RETURN;
      }

      mAcpt.listen(backlog, ec);
      if (ec) {
        errs << "listen failed: " << ec.message();
        goto RETURN;
      }

      do_accept();
    RETURN:
      cb ? cb(std::move(errs).str()) : void(0);
    });
  }

  void stop(std::function<void(std::string)> cb = {})
  {
    Ba::post(mAcpt.get_executor(), [=, cb = std::move(cb)]() mutable {
      std::ostringstream errs;
      BoostEC ec;

      mAcpt.close(ec);
      if (ec) {
        errs << "close failed: " << ec.message();
        goto RETURN;
      }

      { // 关闭所有会话后再调用 cb
        struct Shared
        {
          std::function<void(std::string)> mCb;
          std::size_t mCnt;
          std::stringstream mErrs;
          std::mutex mLock;
        };
        std::shared_ptr<Shared> shared(new Shared{
          .mCb = std::move(cb),
          .mCnt = mSessions.size(),
        });

        for (auto sess : mSessions)
          sess->delete_later([shared](std::string err) {
            std::scoped_lock lock(shared->mLock);
            if (!err.empty())
              shared->mErrs << err << '\n';
            if (--shared->mCnt == 0 && shared->mCb)
              shared->mCb(std::move(shared->mErrs).str());
          });
      }

      return;
    RETURN:
      return cb ? cb(std::move(errs).str()) : void(0);
    });
  }

private:
  Executor mEx;
  Tcp::acceptor mAcpt;

  void do_accept()
  {
    auto _on_accept = [this](const BoostEC& ec, Tcp::socket&& sock) {
      if (ec) {
        if (ec != Ba::error::operation_aborted)
          std::cerr << "server " << mAcpt.local_endpoint()
                    << " accept failed: " << ec.message();
        return;
      }
      come(std::move(sock));
      do_accept();
    };
    mAcpt.async_accept(Ba::make_strand(mEx), std::move(_on_accept));
  }

public:
  void audit(std::function<void(const std::unordered_set<Session*>&)> cb)
  {
    Ba::post(mAcpt.get_executor(),
             [this, cb = std::move(cb)]() { cb(mSessions); });
  }

protected:
  friend class Session;

  std::unordered_set<Session*> mSessions;
  // 更适合使用侵入 Session 类的链表数据结构

  virtual void come(Tcp::socket&& sock)
  {
    auto endpoint = sock.remote_endpoint(); // 防止多线程竞争
    auto sess = new Session(*this, std::move(sock));

    std::ostringstream sout;
    sout << "session " << sess << " come: " << endpoint << '\n';
    std::cout << sout.str() << std::flush;
    mSessions.insert(sess);
  }

  virtual void over(Session& sess)
  {
    std::ostringstream sout;
    sout << "session " << &sess << " over\n";
    std::cout << sout.str() << std::flush;
    mSessions.erase(&sess);
    delete &sess; // 构造-析构匹配原则:
                  // Server new 的 Session, 就由 Server 去 delete
  }
};

std::string
Session::do_close()
{
  std::stringstream errs;
  BoostEC ec;
  mSock.close(ec);
  // 一定注意: close 后再访问 local_endpoint 或 remote_endpoint 会出错！
  if (ec)
    errs << "session " << this << " close failed: " << ec.message();
  // Session 的释放必须由 Server 完成, 由于 Server 工作在不同的执行器,
  // 同样必须用 post 让它自己去主动 over.
  Ba::post(mServer.mAcpt.get_executor(), [this]() { mServer.over(*this); });
  return {};
}

class Client
{
public:
  Tcp::socket mSock;

  Client(Executor ex)
    : mSock(Ba::make_strand(ex))
  {
  }

  void start(Tcp::endpoint ep, std::function<void(std::string)> cb = {})
  {
    Ba::post(mSock.get_executor(),
             [this, ep = std::move(ep), cb = std::move(cb)]() mutable {
               std::stringstream errs;
               BoostEC ec;
               mSock.open(ep.protocol(), ec);
               if (ec) {
                 errs << "open failed: " << ec.message();
                 cb ? cb(std::move(errs).str()) : void(0);
                 return;
               }

               mSock.async_connect(
                 ep, [this, cb = std::move(cb)](const BoostEC& ec) {
                   std::stringstream errs;
                   if (ec) {
                     errs << "connect failed: " << ec.message();
                     goto RETURN;
                   }
                   do_send(), do_receive();
                 RETURN:
                   cb ? cb(std::move(errs).str()) : void(0);
                 });
             });
  }

  void stop(std::function<void(std::string)> cb = {})
  {
    Ba::post(mSock.get_executor(), [this, cb = std::move(cb)]() {
      std::stringstream errs;
      BoostEC ec;
      mSock.close(ec);
      if (ec) {
        errs << "close failed: " << ec.message() << '\n';
        goto RETURN;
      }
    RETURN:
      cb ? cb(std::move(errs).str()) : void(0);
    });
  }

  void over(std::function<void(std::string)> cb = [](std::string) {})
  {
    Ba::post(mSock.get_executor(), [this, cb = std::move(cb)]() mutable {
      if (mGraceCb) {
        cb("already cancelling");
        return;
      }
      mGraceCb = std::move(cb);
    });
  }

private:
  std::array<std::uint8_t, kBufferSize> mBuf;

  std::function<void(std::string)> mGraceCb;

  void do_send()
  {
    if (mGraceCb) {
      mSock.shutdown(Tcp::socket::shutdown_send);
      return;
    }

    mSock.async_send(
      Ba::buffer(kData), [this](const BoostEC& ec, std::size_t len) {
        if (ec) {
          if (ec == Ba::error::operation_aborted)
            return;
          std::cerr << "client " << this << " send failed: " << ec.message()
                    << std::endl;
          stop();
          return;
        }
        do_send();
      });
  }

  void do_receive()
  {
    mSock.async_receive(
      Ba::buffer(mBuf), [this](const BoostEC& ec, std::size_t len) {
        if (ec) {
          if (ec == Ba::error::operation_aborted)
            return;
          if (ec == Ba::error::eof) {
            BoostEC ec;
            // mSock.shutdown(Tcp::socket::shutdown_receive, ec);
            // if (ec) {
            //   std::cerr << "client " << this
            //             << " shutdown recieve failed: " << ec.message()
            //             << std::endl;
            // }
            mSock.close(ec);
            mGraceCb ? mGraceCb(ec ? ec.message() : "") : void(0);
            mGraceCb = {};
            return;
          }
          std::cerr << "client " << this << " receive failed: " << ec.message()
                    << std::endl;
          stop();
          return;
        }
        do_receive();
      });
  }
};

int
main()
{
  Ba::io_context ioctx;

  auto localhost = Ip::address_v4::from_string("127.0.0.100");

  Server server(ioctx.get_executor());
  server.start({ Tcp::v4(), 12345 }, 4096, [](std::string err) {
    if (!err.empty())
      std::cout << "server start failed: " << err << std::endl;
  });

  std::vector<std::thread> threads(std::thread::hardware_concurrency());
  for (auto& thread : threads)
    thread = std::thread([&]() { ioctx.run(); });

  std::forward_list<Client> clients;

  auto _op_show = [&]() {
    server.audit([](const std::unordered_set<Session*>& sessions) {
      for (auto sess : sessions) {
        auto stat = sess->statistics();
        std::cout << stat.mRemoteEndpoint //
                  << ":\n  TimeEstablished = "
                  << stat.mTimeEstablished.time_since_epoch().count() //
                  << "\n  TimeLastActive = "
                  << stat.mTimeLastActive.time_since_epoch().count()
                  << "\n  AmountReceive = " << stat.mAmountReceive //
                  << "\n  AmountSent = " << stat.mAmountSent       //
                  << std::endl;
      }
    });
  };

  auto _op_new = [&]() {
    auto& client = clients.emplace_front(ioctx.get_executor());
    client.start({ localhost, 12345 }, [&](std::string err) {
      if (!err.empty()) {
        std::cout << "client start failed: " << err << std::endl;
        return;
      }
      std::ostringstream sout;
      sout << "client " << &client
           << " started: " << client.mSock.local_endpoint() << '\n';
      std::cout << sout.str() << std::flush;
    });
  };

  auto _op_del = [&]() {
    auto it = clients.before_begin(), itEnd = clients.end();
    decltype(it) itNext;
    while ((itNext = std::next(it)) != itEnd) {
      if (it->mSock.is_open())
        break;
      clients.erase_after(it);
    }

    auto& client = *itNext;
    client.over([&](std::string err) {
      std::ostringstream sout;
      sout << "client " << &client << " over: " << err << '\n';
      std::cout << sout.str() << std::flush;
    });
  };

  std::cout << __LINE__ << std::endl, _op_show();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << __LINE__ << std::endl, _op_new();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << __LINE__ << std::endl, _op_show();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << __LINE__ << std::endl, _op_show();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << __LINE__ << std::endl, _op_new();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << __LINE__ << std::endl, _op_show();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << __LINE__ << std::endl, _op_del();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::cout << __LINE__ << std::endl, _op_del();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  ioctx.stop();
  for (auto& thread : threads)
    thread.join();
}
