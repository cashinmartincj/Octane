add_executable(hello_world main.cpp)
target_link_libraries(hello_world PRIVATE octane_lib)

configure_file(index.html ${CMAKE_CURRENT_BINARY_DIR}/index.html COPYONLY)
configure_file(style.css  ${CMAKE_CURRENT_BINARY_DIR}/style.css  COPYONLY)
configure_file(app.js     ${CMAKE_CURRENT_BINARY_DIR}/app.js     COPYONLY)