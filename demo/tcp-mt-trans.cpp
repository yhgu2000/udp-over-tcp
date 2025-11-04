/******************************************************************************
 * 基本示例：使用 Boost.Asio 多线程收发 TCP 数据                              *
 ******************************************************************************/

#include <atomic>
#include <boost/asio.hpp>
#include <boost/system/result.hpp>
#include <chrono>
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

  void close()
  {
    // 可以被 Session 之外的类调用, 暴力关闭会话.
    // 为了保证这个方法从 Session 之外调用是安全的, 需要用 post 让 Session
    // 所在的执行器去主动结束所有异步过程.
    Ba::post(mSock.get_executor(), [this]() { do_close(); });
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

  bool mOver{ false };

  void do_close();

  void do_send()
  {
    mTimeLastActive.store(Clock::now(), std::memory_order_relaxed);
    if (mOver) {
      BoostEC ec;
      mSock.shutdown(Tcp::socket::shutdown_send, ec);
      if (ec)
        std::cerr << "session " << mSock.remote_endpoint()
                  << " shutdown failed: " << ec.message() << std::endl;
      return do_close();
    }

    mSock.async_send(
      Ba::buffer(kData), [this](const BoostEC& ec, std::size_t len) {
        if (ec) {
          std::cerr << "session " << mSock.remote_endpoint()
                    << " send failed: " << ec.message() << std::endl;
          return do_close();
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
          if (ec == Ba::error::eof) {
            BoostEC ec;
            mSock.shutdown(Tcp::socket::shutdown_receive, ec);
            if (ec)
              std::cerr << "session " << mSock.remote_endpoint()
                        << " shutdown receive failed: " << ec.message()
                        << std::endl;
          } else
            std::cerr << "session " << mSock.remote_endpoint()
                      << " receive failed: " << ec.message() << std::endl;

          mOver = true;
          return; // 总是让 send 异步过程优雅关闭连接
                  // 不能调用 close 因为只能在 send 中调用一次!
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
    for (auto sess : mSessions) {
    }
  }

  void start(const Tcp::endpoint& endpoint,
             int backlog = Ba::socket_base::max_listen_connections)
  {
    Ba::post(mAcpt.get_executor(), [=]() {
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
      on_start(std::move(errs).str());
    });
  }

  void stop()
  {
    Ba::post(mAcpt.get_executor(), [=]() {
      std::ostringstream errs;
      BoostEC ec;

      mAcpt.cancel(ec);
      if (ec) {
        errs << "cancel failed: " << ec.message();
        goto RETURN;
      }

      mAcpt.close(ec);
      if (ec) {
        errs << "close failed: " << ec.message();
        goto RETURN;
      }

    RETURN:
      on_stop(std::move(errs).str());
    });
  }

protected:
  virtual void on_start(std::string err)
  {
    if (!err.empty()) {
      std::cout << "server " << mAcpt.local_endpoint()
                << " start failed: " << err << std::endl;
      return;
    }
    std::cout << "server " << mAcpt.local_endpoint() << " started" << std::endl;
  }

  virtual void on_stop(std::string err)
  {
    if (!err.empty()) {
      std::cout << "server " << mAcpt.local_endpoint()
                << " stop failed: " << err << std::endl;
      return;
    }
    std::cout << "server " << mAcpt.local_endpoint() << " stopped" << std::endl;
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
    std::cout << "session " << sock.remote_endpoint() << " come" << std::endl;
    auto sess = new Session(*this, std::move(sock));
    mSessions.insert(sess);
  }

  virtual void over(Session& sess)
  {
    std::cout << "session " << sess.mSock.remote_endpoint() << " over"
              << std::endl;
    mSessions.erase(&sess);
    delete &sess; // 构造-析构匹配原则:
                  // Server new 的 Session, 就由 Server 去 delete
  }
};

void
Session::do_close()
{
  BoostEC ec;
  mSock.close(ec);
  if (ec)
    std::cerr << "session " << mSock.remote_endpoint()
              << " close failed: " << ec.message() << std::endl;
  // Session 的释放必须由 Server 完成, 由于 Server 工作在不同的执行器,
  // 同样必须用 post 让它自己去主动 over.
  Ba::post(mServer.mAcpt.get_executor(), [this]() { mServer.over(*this); });
}

class Client
{
public:
  Client(Executor ex)
    : mSock(Ba::make_strand(ex))
  {
  }

  void start(Tcp::endpoint ep)
  {
    static constexpr auto _on_connect = [](Client& self, const BoostEC& ec) {
      std::stringstream errs;
      if (ec) {
        errs << "connect failed: " << ec.message();
        goto RETURN;
      }
      self.do_send(), self.do_receive();
    RETURN:
      self.on_start(std::move(errs).str());
    };

    Ba::post(mSock.get_executor(), [this, ep = std::move(ep)]() {
      std::stringstream errs;
      BoostEC ec;
      mSock.open(ep.protocol(), ec);
      if (ec) {
        errs << "open failed: " << ec.message();
        goto RETURN;
      }
      mSock.async_connect(
        ep, [this](const BoostEC& ec) { _on_connect(*this, ec); });
      return;
    RETURN:
      on_start(std::move(errs).str());
    });
  }

  void stop()
  {
    Ba::post(mSock.get_executor(), [this]() {
      std::stringstream errs;
      BoostEC ec;
      mSock.close(ec);
      if (ec) {
        errs << "close failed: " << ec.message() << std::endl;
        goto RETURN;
      }
    RETURN:
      on_stop(std::move(errs).str());
    });
  }

  void over() {}

protected:
  virtual void on_start(std::string err)
  {
    if (ec) {
      std::cerr << "client connect failed: " << ec.message() << std::endl;
      return;
    }
    std::cout << "client " << mSock.local_endpoint() << " started" << std::endl;
  }

  virtual void on_stop(std::string err)
  {
    if (ec) {
      std::cerr << "client connect failed: " << ec.message() << std::endl;
      return;
    }
    std::cout << "client " << mSock.local_endpoint() << " stopped" << std::endl;
  }

private:
  Tcp::socket mSock;
  std::array<std::uint8_t, kBufferSize> mBuf;

  void do_send()
  {
    mSock.async_send(
      Ba::buffer(kData), [this](const BoostEC& ec, std::size_t len) {
        if (ec) {
          std::cerr << "session " << mSock.remote_endpoint()
                    << " send failed: " << ec.message() << std::endl;
          return stop();
        }
        do_send();
      });
  }

  void do_receive()
  {
    mSock.async_receive(
      Ba::buffer(mBuf), [this](const BoostEC& ec, std::size_t len) {
        if (ec) {
          std::cerr << "session " << mSock.remote_endpoint()
                    << " receive failed: " << ec.message() << std::endl;
          return stop();
        }
        do_receive();
      });
  }
};

int
main()
{
  Ba::io_context ioctx;

  Ba::signal_set grace(ioctx, SIGINT, SIGTERM);
  grace.async_wait([&ioctx](const BoostEC& ec, int signum) {
    if (ec) {
      std::cout << "signal failed: " << ec.message() << std::endl;
      return;
    }

    std::cout << "quit by signal " << signum << std::endl;
    ioctx.stop();
  });

  auto localhost = Ip::address_v4::from_string("127.0.0.1");

  Server server(ioctx.get_executor());
  server.start({ Tcp::v4(), 12345 }, 4096, [](BoostEC&& ec) {
    std::cout << "server start failed: " << ec.message() << std::endl;
  });

  Client client(ioctx.get_executor());
  client.start({ localhost, 12345 });

  std::vector<std::thread> threads(std::thread::hardware_concurrency());
  for (auto& thread : threads)
    thread = std::thread([&]() { ioctx.run(); });
  for (auto& thread : threads)
    thread.join();
}
