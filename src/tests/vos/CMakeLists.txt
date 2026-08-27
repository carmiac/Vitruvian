add_subdirectory(testharness)

Application(testinput SOURCES testinput.cpp)

Application(testxkbfuzz SOURCES testxkbfuzz.cpp LIBS shared)
UsePrivateHeaders(testxkbfuzz shared)

Application(testwatch SOURCES testwatch.cpp)

add_executable(testinject testinject.cpp)

#Application(test_node_monitor SOURCES test_node_monitor.cpp)
#Application(test_node_monitor2 SOURCES test_node_monitor2.cpp)

#UsePrivateHeaders(test_node_monitor kernel)
#UsePrivateHeaders(test_node_monitor2 kernel)
