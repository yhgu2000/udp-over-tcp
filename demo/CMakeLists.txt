#
# 欢迎示例
#
add_executable(demo.hello hello.cpp)


#
# UDP 收发数据示例
#
add_executable(demo.udp-send-recv udp-send-recv.cpp)

target_link_libraries(demo.udp-send-recv PRIVATE Boost::system)
