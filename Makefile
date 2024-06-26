CXXFLAGS += --std=c++17 -I/usr/local/include
LDFLAGS += -lTgBot -lboost_system -lboost_filesystem -lssl -lcrypto -lpthread

main: main.cpp
