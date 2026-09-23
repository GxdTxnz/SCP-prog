# make - сборка только под ОС на которой запускается сборка
# make debug - сборка с debug логом (для gdb)
# make portable - сборка для всех ОС их списка (glibc 2.17+; нужен docker или podman)
# make packages - сборка deb и rpm пакетов для всех ОС из списка (нужен docker или podman)
# make install - установить бинарник в /usr/local/bin (sudo)
# make uninstall - удалить установленный бинарник
# make clean - очистить файлы сборки

TARGET := scpanel
SRC := scpanel.cpp
CXX ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra
NCURSES_CFLAGS := $(shell pkg-config --cflags ncursesw 2>/dev/null)
NCURSES_LIBS := $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
APPDIR := $(PREFIX)/libexec/$(TARGET)
DOCKER ?= $(shell command -v docker >/dev/null 2>&1 && echo docker || echo podman)
RUN_OPTS := --rm --security-opt label=disable $(shell $(DOCKER) --version 2>/dev/null | grep -qi podman && echo --userns=keep-id || echo "-u $$(id -u):$$(id -g)")
BUILD_IMAGE ?= quay.io/pypa/manylinux2014_x86_64
NFPM_IMAGE ?= docker.io/goreleaser/nfpm:v2.43.0
VERSION ?= 1.0.0
INSTALL_BIN := $(if $(wildcard dist/$(TARGET)),dist/$(TARGET),$(TARGET))

.PHONY: all debug portable packages install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(NCURSES_CFLAGS) -o $@ $< $(NCURSES_LIBS)

debug:
	rm -f $(TARGET)
	$(MAKE) --no-print-directory $(TARGET) CXXFLAGS="-std=c++17 -Wall -Wextra -g -O0"

portable: dist/$(TARGET)

dist/$(TARGET): $(SRC) scripts/build-portable.sh
	mkdir -p dist
	$(DOCKER) run $(RUN_OPTS) \
		-v "$(CURDIR):/src:ro" -v "$(CURDIR)/dist:/out" \
		$(BUILD_IMAGE) bash /src/scripts/build-portable.sh

packages: dist/$(TARGET)
	for fmt in deb rpm; do \
		$(DOCKER) run $(RUN_OPTS) -e VERSION=$(VERSION) \
			-v "$(CURDIR):/work" -w /work \
			$(NFPM_IMAGE) package -f nfpm.yaml -p $$fmt -t dist/ || exit 1; \
	done

install: $(INSTALL_BIN)
	install -d "$(DESTDIR)$(APPDIR)" "$(DESTDIR)$(BINDIR)"
	install -m 755 $(INSTALL_BIN) "$(DESTDIR)$(APPDIR)/$(TARGET)"
	ln -sfn "$(APPDIR)/$(TARGET)" "$(DESTDIR)$(BINDIR)/$(TARGET)"
	@if command -v restorecon >/dev/null 2>&1; then restorecon -F "$(DESTDIR)$(APPDIR)/$(TARGET)"; fi
	@echo "Установлено: $(BINDIR)/$(TARGET) -> $(APPDIR)/$(TARGET) (из $(INSTALL_BIN))"

uninstall:
	@link="$(DESTDIR)$(BINDIR)/$(TARGET)"; \
	if [ -L "$$link" ] && [ "$$(readlink "$$link")" = "$(APPDIR)/$(TARGET)" ]; then \
		rm -f "$$link"; \
	elif [ -e "$$link" ] || [ -L "$$link" ]; then \
		echo "$$link не наша ссылка, оставляю"; \
	fi
	rm -f "$(DESTDIR)$(APPDIR)/$(TARGET)"
	@rmdir "$(DESTDIR)$(APPDIR)" 2>/dev/null || true

clean:
	rm -rf $(TARGET) a.out dist
