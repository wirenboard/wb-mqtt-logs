ifneq ($(DEB_HOST_MULTIARCH),)
	CROSS_COMPILE ?= $(DEB_HOST_MULTIARCH)-
endif

ifeq ($(origin CC),default)
	CC := $(CROSS_COMPILE)gcc
endif
ifeq ($(origin CXX),default)
	CXX := $(CROSS_COMPILE)g++
endif

ifeq ($(DEBUG),)
	BUILD_DIR ?= build/release
else
	BUILD_DIR ?= build/debug
endif

# extract Git revision and version number from debian/changelog
GIT_REVISION:=$(shell git rev-parse HEAD)
DEB_VERSION:=$(shell head -1 debian/changelog | awk '{ print $$2 }' | sed 's/[\(\)]//g')

APP_BIN = wb-mqtt-logs
SRC_DIRS = src

COMMON_SRCS := $(shell find $(SRC_DIRS) \( -name *.cpp -or -name *.c \) -and -not -name main.cpp)
COMMON_OBJS := $(COMMON_SRCS:%=$(BUILD_DIR)/%.o)

LDFLAGS = -lpthread -lwbmqtt1
CXXFLAGS = -std=c++14 -Wall -Werror -I$(SRC_DIRS) -DWBMQTT_COMMIT="$(GIT_REVISION)" -DWBMQTT_VERSION="$(DEB_VERSION)" -Wno-psabi
CFLAGS = -Wall -I$(SRC_DIR)

ifeq ($(DEBUG),)
	CXXFLAGS += -O3
else
	CXXFLAGS += -g -O0 -fprofile-arcs -ggdb
endif

.PHONY: all clean

all : $(APP_BIN)

$(APP_BIN): $(COMMON_OBJS) $(BUILD_DIR)/src/main.cpp.o
	${CXX} -o $(BUILD_DIR)/$@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.c.o: %.c
	${CC} -c $< -o $@ ${CFLAGS}

$(BUILD_DIR)/%.cpp.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $^

clean :
	rm -rf $(BUILD_DIR)

install:
	install -D -m 0755  $(BUILD_DIR)/$(APP_BIN) $(DESTDIR)/usr/bin/$(APP_BIN)
