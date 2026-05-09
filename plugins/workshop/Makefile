# workshop plugin — bundles Lua 5.4 and yaml-cpp into a single .so
#
# Build from the rAthena root with `make plugin`, or directly:
#   ./fetch_lua.sh           # one-time, downloads Lua 5.4 source into _deps/lua
#   make
#
# Output: workshop.so next to this Makefile.

HERE       := $(shell pwd)
RATHENA_ROOT ?= $(HERE)/../..
SRC_DIR    := $(RATHENA_ROOT)/src
YAML_SRC   := $(RATHENA_ROOT)/3rdparty/yaml-cpp
LUA_SRC    := $(HERE)/_deps/lua/src

OUT        := workshop.so

# ---- Plugin C++ sources ----
PLUGIN_CPP := $(wildcard $(HERE)/*.cpp)

# ---- Lua 5.4 sources (omit lua.c and luac.c — those are standalone drivers) ----
LUA_C      := $(filter-out $(LUA_SRC)/lua.c $(LUA_SRC)/luac.c, $(wildcard $(LUA_SRC)/*.c))

# ---- yaml-cpp sources (recursive) ----
YAML_CPP   := $(shell find $(YAML_SRC)/src -name '*.cpp' 2>/dev/null)

OBJ_DIR    := $(HERE)/obj
PLUGIN_OBJ := $(patsubst $(HERE)/%.cpp,$(OBJ_DIR)/plugin/%.o,$(PLUGIN_CPP))
LUA_OBJ    := $(patsubst $(LUA_SRC)/%.c,$(OBJ_DIR)/lua/%.o,$(LUA_C))
YAML_OBJ   := $(patsubst $(YAML_SRC)/src/%.cpp,$(OBJ_DIR)/yaml/%.o,$(YAML_CPP))

CXXFLAGS_COMMON := -fPIC -fvisibility=hidden -O2 -g -Wall
CXXFLAGS   := $(CXXFLAGS_COMMON) -std=c++17 -I$(SRC_DIR) -I$(LUA_SRC) \
              -I$(YAML_SRC)/include -DYAML_CPP_STATIC_DEFINE
CFLAGS     := $(CXXFLAGS_COMMON) -DLUA_USE_LINUX -DLUA_USE_DLOPEN

LDFLAGS    := -shared -ldl -lm -lpthread

.PHONY: all clean check_lua

all: check_lua $(OUT)

check_lua:
	@if [ ! -f $(LUA_SRC)/lua.h ]; then \
		echo "Lua source missing — running fetch_lua.sh"; \
		$(HERE)/fetch_lua.sh; \
	fi

$(OUT): $(PLUGIN_OBJ) $(LUA_OBJ) $(YAML_OBJ)
	@echo "  LD    $@"
	@g++ $(CXXFLAGS_COMMON) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/plugin/%.o: $(HERE)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@g++ $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/lua/%.o: $(LUA_SRC)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@gcc $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/yaml/%.o: $(YAML_SRC)/src/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@g++ $(CXXFLAGS) -c $< -o $@

clean:
	@rm -rf $(OBJ_DIR) $(OUT)
	@echo "  cleaned workshop"
