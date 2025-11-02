/******************************************************************************
 * 基本示例：使用 Boost.Asio 多线程收发 TCP 数据                              *
 ******************************************************************************/

#include <boost/asio.hpp>
#include <boost/system/result.hpp>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>

namespace Ba = boost::asio;
namespace Ip = Ba::ip;

using BoostEC = boost::system::error_code;
using Executor = Ba::any_io_executor;
using Tcp = Ip::tcp;

#ifdef _WIN32
#include <boost/asio/windows/stream_handle.hpp>
using AsyncStream = boost::asio::windows::stream_handle;

inline AsyncStream
async_stdin(Executor& ex)
{
  return { ex, GetStdHandle(STD_INPUT_HANDLE) };
}

#else

#include <boost/asio/posix/stream_descriptor.hpp>
using AsyncStream = boost::asio::posix::stream_descriptor;

inline AsyncStream
async_stdin(Executor& ex)
{
  return { ex, STDIN_FILENO };
}
#endif

/******************************************************************************/
#define SERVER_BUFSIZE 4096
#define CLIENT_BUFSIZE 1024
/******************************************************************************/

class Session;

class Server
{
public:
  Server(Executor ex)
    : mEx(std::move(ex))
    , mAcpt(Ba::make_strand(mEx))
  {
  }

  virtual ~Server() = default;

  // 所有 cb 参数都可以转换为带默认实现的虚函数，start stop
  // 都是在各种所属的 strand 中异步地执行。

  bool start(const Tcp::endpoint& endpoint,
             int backlog = Ba::socket_base::max_listen_connections,
             std::function<void(BoostEC&&)> cb = {})
  {
    // 这种写法不能避免重复 start 时 on_start 被多次触发,
    // 而且会使方法变得线程不安全, 让多线程在 mAcpt 上竞争.
    if (mAcpt.is_open())
      return false;

    Ba::post(mAcpt.get_executor(),
             [this, cb = std::move(cb), endpoint, backlog]() {
               BoostEC ec;
               mAcpt.open(endpoint.protocol(), ec);
               if (ec) {
                 std::cerr << "open failed: " << ec.message();
                 goto RETURN;
               }

               mAcpt.set_option(Ba::socket_base::reuse_address(true), ec);
               if (ec) {
                 std::cerr << "set_option failed: " << ec.message();
                 goto RETURN;
               }

               mAcpt.bind(endpoint, ec);
               if (ec) {
                 std::cerr << "bind failed: " << ec.message();
                 goto RETURN;
               }

               mAcpt.listen(backlog, ec);
               if (ec) {
                 std::cerr << "listen failed: " << ec.message();
                 goto RETURN;
               }

               do_accept();
             RETURN:
               cb ? cb(std::move(ec)) : (void)0;
             });
    return true;
  }

  // start 和 stop 都异步，但不线程安全

  bool stop(std::function<void(BoostEC&& ec)> cb)
  {
    if (!mAcpt.is_open())
      return false;

    Ba::post(mAcpt.get_executor(), [this, cb = std::move(cb)]() {
      BoostEC ec;

      mAcpt.cancel(ec);
      if (ec) {
        std::cerr << "cancel failed: " << ec.message();
        goto RETURN;
      }

      mAcpt.close(ec);
      if (ec) {
        std::cerr << "close failed: " << ec.message();
        goto RETURN;
      }

    RETURN:
      cb ? cb(std::move(ec)) : (void)0;
    });
    return true;
  }

private:
  Executor mEx;
  Tcp::acceptor mAcpt;

  void do_accept()
  {
    mAcpt.async_accept(Ba::make_strand(mEx), [this](auto&& a, auto b) {
      on_accept(a, std::move(b));
    });
  }
  void on_accept(const BoostEC& ec, Tcp::socket&& sock)
  {
    if (ec) {
      if (ec != Ba::error::operation_aborted)
        std::cerr << "accept failed: " << ec.message();
      return;
    }
    come(std::move(sock));
    do_accept();
  }

protected:
  friend class Session;

  std::unordered_set<std::shared_ptr<Session>> mSessions;
  // 更适合使用链表, 并且应当侵入 Session 类

  void come(Tcp::socket&& sock);
  void over(Session& sess);
};

class Session : public std::enable_shared_from_this<Session>
{
public:
  Session(Server& server, Tcp::socket&& sock)
    : mServer(server)
    , mSock(std::move(sock))
  {
  }

  bool start()
  {
    if (mRunning)
      return false;
    mRunning = true;
    Ba::post(mSock.get_executor(), [this]() {
      do_receive();
      on_start();
    });
    return true;
  }

  bool stop()
  {
    if (!mRunning)
      return false;
    Ba::post(mSock.get_executor(), [this]() {
      mRunning = false; // 在下一个事务处理前会检查这个标志并中断处理
    });
    return true;
  }

protected:
  virtual void on_start()
  {
    std::cout << "session " << mSock.remote_endpoint() << " started "
              << std::endl;
  }

  virtual void on_stop()
  {
    std::cout << "session " << mSock.remote_endpoint() << " stopped "
              << std::endl;
  }

private:
  Server& mServer;
  Tcp::socket mSock;
  bool mRunning{ false };
  std::vector<std::uint8_t> mBuf;
  decltype(Ba::dynamic_buffer(mBuf)) mDynBuf{ Ba::dynamic_buffer(mBuf) };

  void do_receive()
  {
    if (!mRunning) {
      on_stop();
      return;
    }

    mSock.async_receive(mDynBuf.prepare(SERVER_BUFSIZE),
                        [this](auto&& a, auto b) { on_receive_1(a, b); });
    // read_some 与 receive 语义完全相同, 只是为了满足多概念的要求而存在.
  }
  void on_receive_1(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "session " << mSock.remote_endpoint()
                << " receive failed: " << ec.message() << '\n';
      mServer.over(*this);
      return;
    }
    std::cout << "session " << mSock.remote_endpoint() << " receive " << len
              << " bytes" << std::endl;

    mDynBuf.commit(len);
    mSock.async_send(mDynBuf.data(),
                     [this](auto&& a, auto b) { on_recieve(a, b); });
  }
  void on_recieve(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "session " << mSock.remote_endpoint()
                << " send failed: " << ec.message() << '\n';
      mServer.over(*this);
      return;
    }
    std::cout << "session " << mSock.remote_endpoint() << " send " << len
              << " bytes" << std::endl;

    mDynBuf.consume(len);
    do_receive();
  }
};

void
Server::come(Tcp::socket&& sock)
{
  auto sess = std::make_shared<Session>(*this, std::move(sock));
  sess->start();
  mSessions.insert(std::move(sess));
}

void
Server::over(Session& sess)
{
  // 也可以用 mutex 保护 mSessions 后把这个方法实现为同步的
  Ba::post(mAcpt.get_executor(),
           [&, this]() { mSessions.erase(sess.shared_from_this()); });
}

class Client
{
public:
  Client(Executor ex)
    : mEx(std::move(ex))
    , mSock(mEx)
    , mInput(async_stdin(mEx))
  {
  }

  // Client 的这种 start stop 最好 (异步+虚方法回调+线程安全)

  void start(Tcp::endpoint ep)
  {
    Ba::post(mSock.get_executor(), [this, ep = std::move(ep)]() {
      BoostEC ec;
      mSock.open(ep.protocol(), ec);
      if (ec)
        return on_start(ec);
      mSock.async_connect(ep, [this](auto&& a) { on_start_1(a); });
    });
  }

  void stop()
  {
    Ba::post(mSock.get_executor(), [this]() {
      BoostEC ec;
      mSock.cancel(ec);
      if (ec)
        std::cerr << "client cancel failed: " << ec.message() << '\n';
      mSock.close(ec);
      if (ec)
        std::cerr << "client close failed: " << ec.message() << '\n';
    });
  }

protected:
  virtual void on_start(const BoostEC& ec)
  {
    if (ec) {
      std::cerr << "client connect failed: " << ec.message() << '\n';
      return;
    }
    std::cout << "client " << mSock.local_endpoint() << " started "
              << std::endl;
  }

  virtual void on_stop(const BoostEC& ec)
  {
    std::cout << "client " << mSock.local_endpoint() << " stopped "
              << std::endl;
  }

private:
  void on_start_1(const BoostEC& ec)
  {
    if (ec)
      return on_start(ec);
    do_send(), do_receive(); // （异步）同时收发
    on_start(ec);
  }

private:
  void do_receive()
  {
    mSock.async_receive(mDynBuf.prepare(CLIENT_BUFSIZE),
                        [this](auto&& a, auto b) { on_receive(a, b); });
  }

  void on_receive(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "client receive failed: " << ec.message() << '\n';
      return stop();
    }

    mDynBuf.commit(len);

    bool allZero = true;
    auto data = mDynBuf.data();
    for (auto i = reinterpret_cast<const std::uint8_t*>(data.data()),
              iE = i + data.size();
         i < iE;
         ++i) {
      if (*i != 0) {
        allZero = false;
        break;
      }
    }

    std::cout << "client recieve " << len << " bytes"
              << (allZero ? "" : "(check failed)") << std::endl;

    mDynBuf.consume(len);
    do_receive();
  }

  void do_send()
  {
    mInputBuf.resize(128);
    mInput.async_read_some(Ba::buffer(mInputBuf),
                           [this](auto&& a, auto b) { on_send_1(a, b); });
  }

  void on_send_1(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "client read failed: " << ec.message() << '\n';
      return;
    }
    std::cout << "client get " << len << " chars from input" << std::endl;

    mInputBuf.resize(len);
    auto size = std::stoul(mInputBuf);
    mData.resize(size);
    mSock.async_send(Ba::buffer(mData),
                     [this](auto&& a, auto b) { on_send(a, b); });
  }

  void on_send(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "client send failed: " << ec.message() << '\n';
      return;
    }
    std::cout << "client send " << len << " bytes" << std::endl;

    do_send();
  }

private:
  Executor mEx;
  Tcp::socket mSock;
  std::vector<std::uint8_t> mBuf;
  decltype(Ba::dynamic_buffer(mBuf)) mDynBuf{ Ba::dynamic_buffer(mBuf) };
  AsyncStream mInput;
  std::string mInputBuf;
  std::vector<std::uint8_t> mData;
};

int
main()
{
  Ba::io_context ioctx;
  // TODO 改成多线程的

  Ba::signal_set grace(ioctx, SIGINT, SIGTERM);
  grace.async_wait([&ioctx](const BoostEC& ec, int signum) {
    if (ec) {
      std::cerr << "signal failed: " << ec.message() << '\n';
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

  ioctx.run();
}
