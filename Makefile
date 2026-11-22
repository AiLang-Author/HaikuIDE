# Linux: document tests. The window links -lbe on Haiku (`make app`).
CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -Ieditor -Ilsp
UNAME_S := $(shell uname -s)
LSP_SRC := lsp/json.cpp lsp/rpc.cpp lsp/protocol.cpp lsp/client.cpp

.PHONY: test clean app host-test

test: editor/test/test_document lsp/test/test_lsp host-test
	./editor/test/test_document
	./lsp/test/test_lsp

host-test:
	cd host && npm test

editor/test/test_document: editor/document.cpp editor/test/test_document.cpp editor/document.hpp
	$(CXX) $(CXXFLAGS) -o $@ editor/document.cpp editor/test/test_document.cpp

lsp/test/test_lsp: $(LSP_SRC) lsp/test/test_lsp.cpp lsp/json.hpp lsp/rpc.hpp lsp/protocol.hpp lsp/client.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(LSP_SRC) lsp/test/test_lsp.cpp

clean:
	rm -f editor/test/test_document lsp/test/test_lsp

ifeq ($(UNAME_S),Haiku)
PREFIX ?= $(HOME)/config/non-packaged
APP := $(PREFIX)/apps/HaikuIDE

app: $(APP)

$(APP): ui/app.cpp ui/editor_view.cpp ui/lsp_bridge.cpp ui/host_link.cpp ui/editor_view.hpp ui/lsp_bridge.hpp ui/host_link.hpp \
		editor/document.cpp editor/document.hpp $(LSP_SRC) lsp/json.hpp lsp/rpc.hpp lsp/protocol.hpp lsp/client.hpp
	mkdir -p $(PREFIX)/apps
	$(CXX) -std=c++17 -Wall -Wextra -O2 -Ieditor -Ilsp -o $@ ui/app.cpp ui/editor_view.cpp ui/lsp_bridge.cpp ui/host_link.cpp \
		editor/document.cpp $(LSP_SRC) -ltracker -lbe -lpthread
else
app:
	@echo "The libbe window builds on Haiku (make app). On Linux: make test"
	@exit 1
endif
