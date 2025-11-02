#
# 欢迎示例
#
add_executable(demo.hello hello.cpp)


#
# UDP 收发示例
#
add_executable(demo.udp-send-recv udp-send-recv.cpp)

target_link_libraries(demo.udp-send-recv PRIVATE Boost::system)


#
# TCP 多线程传输示例
#
add_executable(demo.tcp-mt-trans tcp-mt-trans.cpp)

target_link_libraries(demo.tcp-mt-trans PRIVATE Boost::system)
