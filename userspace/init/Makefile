CXX = x86_64-pc-fishos-mlibc-g++

CXXFLAGS = -O0 -g3 -pipe -Wall -Wextra
LDFLAGS =

BIN = build/init

CXXFILES := $(shell find ./src -type f -name '*.cpp')
OBJ := $(CXXFILES:./src/%.cpp=build/%.o)

.PHONY: all clean install

all: $(BIN)

# Link rules for the final executable.
$(BIN): $(OBJ)
	@echo "[LD] $@"
	@$(CXX) $(OBJ) $(LDFLAGS) -o $@

# Compilation rules for *.cpp files.
build/%.o: src/%.cpp
	@echo "[CXX] $< | $@"
	@mkdir -p $(shell dirname $@)
	@$(CXX) $(CXXFLAGS) -c $< -o $@

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install --strip-program=$(STRIP) -s build/init $(DESTDIR)$(PREFIX)/bin/init

# Remove object files and the final executable.
clean:
	rm -rf $(BIN) build/
