/******************************************************************************
 * 基本示例：使用 Boost.Asio 的收发 UDP 数据                                  *
 ******************************************************************************/

#include <boost/asio.hpp>
#include <boost/system/result.hpp>
#include <iostream>
#include <memory>
#include <string>

namespace Ba = boost::asio;
namespace Ip = Ba::ip;

using BoostEC = boost::system::error_code;
using Executor = Ba::any_io_executor;
using Udp = Ip::udp;

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
#define SERVER_BUFSIZE 10
#define CLIENT_BUFSIZE 20
#define CLIENT_INPUT_BUFSIZE 30
/******************************************************************************/

class Server
{
public:
  Server(Executor ex)
    : mEx(std::move(ex))
    , mSock(mEx)
  {
  }

  BoostEC start(const Udp::endpoint& ep)
  {
    BoostEC ec;
    mSock.open(ep.protocol(), ec);
    if (ec)
      return ec;
    mSock.bind(ep, ec);
    if (ec)
      return ec;

    do_receive();
    return ec;
  }

  void stop()
  {
    Ba::post(mSock.get_executor(), [this]() {
      BoostEC ec;
      mSock.cancel(ec);
      if (ec)
        std::cerr << "server cancel failed: " << ec.message() << '\n';
      mSock.close(ec);
      if (ec)
        std::cerr << "server close failed: " << ec.message() << '\n';
    });
  }

private:
  void do_receive()
  {
    mSock.async_receive_from(mDynBuf.prepare(SERVER_BUFSIZE),
                             mWho,
                             [this](auto&& a, auto b) { on_receive(a, b); });
    // dynamic_buffer 对 boost::asio::ip::udp 无效果, 如果 prepare
    // 的空间不够大, 多余的数据会被直接丢弃, 根本不会发送扩容.
  }

  void on_receive(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "server receive failed: " << ec.message() << '\n';
      return;
    }
    std::cout << "server recieve " << len << " bytes from " << mWho
              << std::endl;

    mDynBuf.commit(len);
    do_send();
  }

  void do_send()
  {
    mSock.async_send_to(
      Ba::buffer(mBuf), mWho, [this](auto&& a, auto b) { on_send(a, b); });
  }

  void on_send(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "server send failed: " << ec.message() << '\n';
      return;
    }
    std::cout << "server send " << len << " bytes to " << mWho << std::endl;

    mDynBuf.consume(len);
    do_receive();
  }

private:
  Executor mEx;
  Udp::socket mSock;
  std::vector<std::uint8_t> mBuf;
  decltype(Ba::dynamic_buffer(mBuf)) mDynBuf{ Ba::dynamic_buffer(mBuf) };
  Udp::endpoint mWho;
};

std::string
escape_bytes(const void* begin, std::size_t size)
{
  if (size == 0)
    return "\"\"";

  std::string ret = "\"";
  const char* const kHexChars = "0123456789abcdef";

  auto p = reinterpret_cast<const std::uint8_t*>(begin);
  for (auto end = p + size; p != end; ++p) {
    auto byte = *p;
    switch (byte) {
      case '"':
        ret += "\\\"";
        break;
      case '\\':
        ret += "\\\\";
        break;
      case '/':
        ret += "\\/";
        break;
      case '\b':
        ret += "\\b";
        break;
      case '\f':
        ret += "\\f";
        break;
      case '\n':
        ret += "\\n";
        break;
      case '\r':
        ret += "\\r";
        break;
      case '\t':
        ret += "\\t";
        break;
      default:
        if (std::isprint(static_cast<int>(byte))) {
          ret += static_cast<char>(byte);
        } else {
          ret += "\\x";
          ret += kHexChars[(byte >> 4) & 0x0F];
          ret += kHexChars[byte & 0x0F];
        }
        break;
    }
  }

  ret += "\"";
  return ret;
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

  BoostEC start(const Udp::endpoint& ep)
  {
    BoostEC ec;
    mSock.open(ep.protocol());
    if (ec)
      return ec;
    mWho = ep;
    do_receive();
    do_send();
    return ec;
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

private:
  void do_receive()
  {
    mSock.async_receive_from(mDynBuf.prepare(CLIENT_BUFSIZE),
                             mWho,
                             [this](auto&& a, auto b) { on_receive(a, b); });
  }

  void on_receive(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "client receive failed: " << ec.message() << '\n';
      return;
    }

    mDynBuf.commit(len);
    auto data = mDynBuf.data();
    std::cout << "client recieve " << escape_bytes(data.data(), data.size())
              << " from " << mWho << std::endl;

    mDynBuf.consume(len);
    do_receive();
  }

  void do_send()
  {
    mInput.async_read_some(mDynInputBuf.prepare(CLIENT_INPUT_BUFSIZE),
                           [this](auto&& a, auto b) { on_send_1(a, b); });
  }

  void on_send_1(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "client read failed: " << ec.message() << '\n';
      return;
    }
    std::cout << "client get " << len << " chars from input" << std::endl;

    mDynInputBuf.commit(len);
    mSock.async_send_to(
      mDynInputBuf.data(), mWho, [this](auto&& a, auto b) { on_send(a, b); });
  }

  void on_send(const BoostEC& ec, std::size_t len)
  {
    if (ec) {
      std::cerr << "client send failed: " << ec.message() << '\n';
      return;
    }
    std::cout << "client send " << len << " bytes to " << mWho << std::endl;

    mDynInputBuf.consume(len);
    do_send();
  }

private:
  Executor mEx;
  Udp::socket mSock;
  std::vector<std::uint8_t> mBuf;
  decltype(Ba::dynamic_buffer(mBuf)) mDynBuf{ Ba::dynamic_buffer(mBuf) };
  Udp::endpoint mWho;
  AsyncStream mInput;
  std::vector<std::uint8_t> mInputBuf;
  decltype(mDynBuf) mDynInputBuf{ Ba::dynamic_buffer(mInputBuf) };
};

int
main()
{
  Ba::io_context ioctx;

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
  if (auto ec = server.start({ Udp::v4(), 12345 })) {
    std::cout << "server start failed: " << ec.message() << std::endl;
    return 1;
  }

  Client client(ioctx.get_executor());
  if (auto ec = client.start({ localhost, 12345 })) {
    std::cout << "client start failed: " << ec.message() << std::endl;
    return 2;
  }

  ioctx.run();
}
