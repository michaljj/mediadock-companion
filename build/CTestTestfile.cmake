# CMake generated Testfile for 
# Source directory: C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp
# Build directory: C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(serializer_test "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/build/Debug/serializer_test.exe")
  set_tests_properties(serializer_test PROPERTIES  _BACKTRACE_TRIPLES "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;32;add_test;C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(serializer_test "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/build/Release/serializer_test.exe")
  set_tests_properties(serializer_test PROPERTIES  _BACKTRACE_TRIPLES "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;32;add_test;C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(serializer_test "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/build/MinSizeRel/serializer_test.exe")
  set_tests_properties(serializer_test PROPERTIES  _BACKTRACE_TRIPLES "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;32;add_test;C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(serializer_test "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/build/RelWithDebInfo/serializer_test.exe")
  set_tests_properties(serializer_test PROPERTIES  _BACKTRACE_TRIPLES "C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;32;add_test;C:/user_data/Repos/STM32/MediaDockSTM32C5/WinApp/CMakeLists.txt;0;")
else()
  add_test(serializer_test NOT_AVAILABLE)
endif()
