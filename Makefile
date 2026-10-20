# CS375 A9 - Memory Simulator
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

SRC  := submission/memory_simulator.cpp
BIN  := build/memory_simulator

.PHONY: all run demo report clean

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<

# Interactive run (outputs such as results.txt land in submission/)
run: $(BIN)
	cd submission && ../$(BIN)

# Interactive run with the sample configuration and a fixed seed
demo: $(BIN)
	cd submission && ../$(BIN) --seed 42 --config sample_config.txt

# Build report/report.pdf and copy it into submission/
report:
	cd report && pdflatex -interaction=nonstopmode -halt-on-error report.tex >/dev/null && \
	             pdflatex -interaction=nonstopmode -halt-on-error report.tex >/dev/null
	cp report/report.pdf submission/report.pdf
	@echo "Report built: submission/report.pdf"

clean:
	rm -rf build
	rm -f report/*.aux report/*.log report/*.out report/*.toc report/*.pdf
