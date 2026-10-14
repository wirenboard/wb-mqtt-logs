PREFIX = /usr

ifneq ($(DEB_HOST_MULTIARCH),)
	CROSS_COMPILE ?= $(DEB_HOST_MULTIARCH)-
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
DEB_VERSION:=$(shell dpkg-parsechangelog -S Version)

APP_BIN = wb-mqtt-logs
SRC_DIRS = src

COMMON_SRCS := $(shell find $(SRC_DIRS) \( -name *.cpp -or -name *.c \) -and -not -name main.cpp)
COMMON_OBJS := $(COMMON_SRCS:%=$(BUILD_DIR)/%.o)

LDFLAGS = -lpthread -lwbmqtt1 -lsystemd -lpcre2-8
CXXFLAGS = -std=c++20 -Wall -Werror -I$(SRC_DIRS) -DWBMQTT_COMMIT="$(GIT_REVISION)" -DWBMQTT_VERSION="$(DEB_VERSION)" -Wno-psabi

ifeq ($(DEBUG),)
	CXXFLAGS += -O3
else
	CXXFLAGS += -g -O0 --coverage
	LDFLAGS += --coverage
endif

TEST_DIR = test
TEST_SRCS := $(shell find $(TEST_DIR) -name "*.cpp")
TEST_OBJS := $(TEST_SRCS:%=$(BUILD_DIR)/%.o)
TEST_BIN = $(BUILD_DIR)/wb-mqtt-logs-test
TEST_LIBS = -lgtest

VALGRIND_FLAGS = --error-exitcode=180 -q

COV_REPORT ?= $(BUILD_DIR)/cov
GCOVR_FLAGS := -s --html $(COV_REPORT).html -x $(COV_REPORT).xml
ifneq ($(COV_FAIL_UNDER),)
	GCOVR_FLAGS += --fail-under-line $(COV_FAIL_UNDER)
endif

.PHONY: all clean test

all : $(APP_BIN)

$(APP_BIN): $(COMMON_OBJS) $(BUILD_DIR)/src/main.cpp.o
	$(CXX) -o $(BUILD_DIR)/$@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.cpp.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $^

$(TEST_BIN): $(COMMON_OBJS) $(TEST_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(TEST_LIBS)

test: $(TEST_BIN)
	if [ "$(shell arch)" != "armv7l" ] && [ "$(CROSS_COMPILE)" = "" ] || [ "$(CROSS_COMPILE)" = "x86_64-linux-gnu-" ]; then \
		valgrind $(VALGRIND_FLAGS) $(TEST_BIN) $(TEST_ARGS) || \
		if [ $$? = 180 ]; then \
			echo "*** VALGRIND DETECTED ERRORS ***" 1>& 2; \
			exit 1; \
		else exit 1; fi; \
	else \
		$(TEST_BIN) $(TEST_ARGS); \
	fi
ifneq ($(DEBUG),)
	gcovr $(GCOVR_FLAGS) $(BUILD_DIR)/$(SRC_DIR) $(BUILD_DIR)/$(TEST_DIR)
endif

clean :
	rm -rf $(BUILD_DIR)

install:
	install -Dm0755 $(BUILD_DIR)/$(APP_BIN) -t $(DESTDIR)$(PREFIX)/bin
